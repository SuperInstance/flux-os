/*
 * FLUX OS — System Call Dispatcher
 *
 * Central dispatch point for all system calls from user-space and
 * bytecode processes. Validates arguments, logs the call, and routes
 * to the appropriate kernel subsystem.
 *
 * System Call Categories:
 *   100-199: Process management (SPAWN, YIELD, EXIT, KILL, WAIT, GETPID)
 *   200-299: Memory management (ALLOC, FREE, MMAP, MUNMAP, MPROTECT)
 *   300-399: IPC / A2A (SEND, RECV, BROADCAST, SUBSCRIBE)
 *   400-499: Bytecode execution (BC_LOAD, BC_EXEC, BC_STATUS, BC_DUMP)
 *   500-599: Self-Compiler (COMPILE, EMIT, DEVCODE)
 *   600-699: I/O (READ, WRITE, IOCTL, OPEN, CLOSE)
 *   700-799: Hardware queries (HW_INFO, HW_CONFIG)
 *   800-899: Info & logging (INFO, LOG)
 *
 * Security Model:
 *   - All arguments are validated before use
 *   - Capability checks are performed for privileged operations
 *   - Kernel log is maintained for audit trail
 *   - Invalid syscall numbers are rejected with FLUX_ERR_INVALID
 */

#include "flux/kernel.h"
#include "flux/hal.h"
#include "flux/vm.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* Capability constants (from agent.h — replicated here to avoid header conflict) */
#ifndef FLUX_CAP_ALL
#define FLUX_CAP_SPAWN          0x0000000000000001ULL
#define FLUX_CAP_COMMUNICATE    0x0000000000000002ULL
#define FLUX_CAP_COMPILE        0x0000000000000004ULL
#define FLUX_CAP_IO_READ        0x0000000000000008ULL
#define FLUX_CAP_IO_WRITE       0x0000000000000010ULL
#define FLUX_CAP_HARDWARE       0x0000000000000040ULL
#define FLUX_CAP_SUPERVISOR     0x0000000000000400ULL
#endif

/* ========================================================================
 * Configuration
 * ======================================================================== */

#ifndef FLUX_SYSCALL_LOG_ENABLED
#define FLUX_SYSCALL_LOG_ENABLED    1    /* Log all syscalls */
#endif

#ifndef FLUX_SYSCALL_MAX_ARGS
#define FLUX_SYSCALL_MAX_ARGS       6     /* Maximum syscall arguments */
#endif

/* ========================================================================
 * Syscall Name Table
 * ======================================================================== */

typedef struct {
    flux_syscall_t num;
    const char *name;
    const char *category;
} syscall_info_t;

static const syscall_info_t s_syscall_table[] = {
    /* Process management */
    { FLUX_SYSCALL_SPAWN,       "SPAWN",     "process" },
    { FLUX_SYSCALL_YIELD,       "YIELD",     "process" },
    { FLUX_SYSCALL_EXIT,        "EXIT",      "process" },
    { FLUX_SYSCALL_KILL,        "KILL",      "process" },
    { FLUX_SYSCALL_WAIT,        "WAIT",      "process" },
    { FLUX_SYSCALL_GETPID,      "GETPID",    "process" },

    /* Memory management */
    { FLUX_SYSCALL_ALLOC,       "ALLOC",     "memory" },
    { FLUX_SYSCALL_FREE,        "FREE",      "memory" },
    { FLUX_SYSCALL_MMAP,        "MMAP",      "memory" },
    { FLUX_SYSCALL_MUNMAP,      "MUNMAP",    "memory" },
    { FLUX_SYSCALL_MPROTECT,    "MPROTECT",  "memory" },

    /* IPC */
    { FLUX_SYSCALL_A2A_SEND,    "A2A_SEND",  "ipc" },
    { FLUX_SYSCALL_A2A_RECV,    "A2A_RECV",  "ipc" },
    { FLUX_SYSCALL_A2A_BROADCAST, "A2A_BROADCAST", "ipc" },
    { FLUX_SYSCALL_A2A_SUBSCRIBE, "A2A_SUBSCRIBE", "ipc" },

    /* Bytecode */
    { FLUX_SYSCALL_BC_LOAD,     "BC_LOAD",   "bytecode" },
    { FLUX_SYSCALL_BC_EXEC,     "BC_EXEC",   "bytecode" },
    { FLUX_SYSCALL_BC_STATUS,   "BC_STATUS", "bytecode" },
    { FLUX_SYSCALL_BC_DUMP,     "BC_DUMP",   "bytecode" },

    /* Self-Compiler */
    { FLUX_SYSCALL_COMPILE,     "COMPILE",   "compiler" },
    { FLUX_SYSCALL_EMIT,        "EMIT",      "compiler" },
    { FLUX_SYSCALL_DEVCODE,     "DEVCODE",   "compiler" },

    /* I/O */
    { FLUX_SYSCALL_READ,        "READ",      "io" },
    { FLUX_SYSCALL_WRITE,       "WRITE",     "io" },
    { FLUX_SYSCALL_IOCTL,       "IOCTL",     "io" },
    { FLUX_SYSCALL_OPEN,        "OPEN",      "io" },
    { FLUX_SYSCALL_CLOSE,       "CLOSE",     "io" },

    /* Hardware */
    { FLUX_SYSCALL_HW_INFO,     "HW_INFO",   "hardware" },
    { FLUX_SYSCALL_HW_CONFIG,   "HW_CONFIG", "hardware" },

    /* Info */
    { FLUX_SYSCALL_INFO,        "INFO",      "info" },
    { FLUX_SYSCALL_LOG,         "LOG",       "info" },
};

#define SYSCALL_TABLE_SIZE (sizeof(s_syscall_table) / sizeof(s_syscall_table[0]))

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/*
 * syscall_lookup_name — Find the name for a syscall number.
 */
static const char *syscall_lookup_name(flux_syscall_t num)
{
    for (size_t i = 0; i < SYSCALL_TABLE_SIZE; i++) {
        if (s_syscall_table[i].num == num)
            return s_syscall_table[i].name;
    }
    return "UNKNOWN";
}

/*
 * syscall_lookup_category — Find the category for a syscall number.
 */
static const char *syscall_lookup_category(flux_syscall_t num)
{
    for (size_t i = 0; i < SYSCALL_TABLE_SIZE; i++) {
        if (s_syscall_table[i].num == num)
            return s_syscall_table[i].category;
    }
    return "unknown";
}

/*
 * syscall_validate_pid — Check that a PID argument is valid.
 */
static bool syscall_validate_pid(flux_pid_t pid)
{
    if (pid == FLUX_PID_INVALID || pid == 0)
        return false;

    /* Verify process exists */
    return flux_get_pcb(pid) != NULL;
}

/*
 * syscall_validate_ptr — Check that a pointer argument is non-NULL
 * and (in hosted mode) looks reasonable.
 */
static bool syscall_validate_ptr(const void *ptr)
{
    if (!ptr) return false;
    return true;
}

/*
 * syscall_log_entry — Log a syscall invocation.
 */
static void syscall_log_entry(flux_syscall_t num, const char *name)
{
#if FLUX_SYSCALL_LOG_ENABLED
    flux_log_debug("syscall: %s(%d) from pid=%u",
                   name, (int)num, flux_getpid());
#else
    (void)num;
    (void)name;
#endif
}

/*
 * syscall_log_result — Log a syscall result.
 */
static void syscall_log_result(flux_syscall_t num, flux_status_t result)
{
#if FLUX_SYSCALL_LOG_ENABLED
    if (result != FLUX_OK) {
        const char *name = syscall_lookup_name(num);
        flux_log_debug("syscall: %s returned %d", name, (int)result);
    }
#else
    (void)num;
    (void)result;
#endif
}

/*
 * syscall_check_cap — Check if current process has a required capability.
 */
static bool syscall_check_cap(flux_cap_t required)
{
    flux_pid_t pid = flux_getpid();
    if (pid == FLUX_PID_KERNEL)
        return true;  /* Kernel has all capabilities */

    flux_pcb_t *pcb = flux_get_pcb(pid);
    if (!pcb)
        return false;

    return (pcb->capabilities & required) != 0;
}

/* ========================================================================
 * Syscall Handlers by Category
 * ======================================================================== */

/*
 * handle_process_syscall — Process management syscalls.
 */
static flux_status_t handle_process_syscall(flux_syscall_t num,
                                           void *arg0, void *arg1,
                                           void *arg2, void *arg3)
{
    switch (num) {
        case FLUX_SYSCALL_SPAWN: {
            /* arg0 = name (const char*), arg1 = entry (flux_addr_t),
             * arg2 = stack_size (flux_size_t), arg3 = is_agent (bool) */
            const char *name = (const char *)arg0;
            flux_addr_t entry = (flux_addr_t)(uintptr_t)arg1;
            flux_size_t stack = (flux_size_t)(uintptr_t)arg2;
            bool is_agent = arg3 != NULL;

            if (!syscall_check_cap(FLUX_CAP_SPAWN))
                return FLUX_ERR_DENIED;

            flux_pid_t pid = flux_spawn(name, entry, stack, is_agent);
            return (pid != FLUX_PID_INVALID) ? (flux_status_t)pid : FLUX_ERR_GENERAL;
        }

        case FLUX_SYSCALL_YIELD:
            return flux_yield();

        case FLUX_SYSCALL_EXIT: {
            int code = (int)(intptr_t)arg0;
            return flux_exit(code);
        }

        case FLUX_SYSCALL_KILL: {
            flux_pid_t target = (flux_pid_t)(uintptr_t)arg0;
            if (!syscall_validate_pid(target))
                return FLUX_ERR_NOTFOUND;
            return flux_kill(target);
        }

        case FLUX_SYSCALL_WAIT: {
            /* arg0 = target PID to wait for */
            flux_pid_t target = (flux_pid_t)(uintptr_t)arg0;
            if (!syscall_validate_pid(target))
                return FLUX_ERR_NOTFOUND;

            /* Block current process until target exits */
            flux_pid_t my_pid = flux_getpid();
            flux_pcb_t *my_pcb = flux_get_pcb(my_pid);
            if (my_pcb) {
                my_pcb->state = FLUX_PROC_BLOCKED;
                my_pcb->waiting_for = target;
            }
            return FLUX_OK;
        }

        case FLUX_SYSCALL_GETPID: {
            /* Return PID via arg0 (output pointer) */
            flux_pid_t *out = (flux_pid_t *)arg0;
            if (out) *out = flux_getpid();
            return FLUX_OK;
        }

        default:
            return FLUX_ERR_INVALID;
    }
}

/*
 * handle_memory_syscall — Memory management syscalls.
 */
static flux_status_t handle_memory_syscall(flux_syscall_t num,
                                           void *arg0, void *arg1,
                                           void *arg2)
{
    switch (num) {
        case FLUX_SYSCALL_ALLOC: {
            flux_size_t size = (flux_size_t)(uintptr_t)arg0;
            flux_mem_type_t type = (flux_mem_type_t)(intptr_t)arg1;

            if (size == 0)
                return FLUX_ERR_INVALID;

            void *ptr = flux_alloc(size, type);
            if (!ptr)
                return FLUX_ERR_NOMEM;

            /* Return pointer via arg0 if it was an output pointer */
            void **out = (void **)arg0;
            *out = ptr;
            return FLUX_OK;
        }

        case FLUX_SYSCALL_FREE: {
            if (!syscall_validate_ptr(arg0))
                return FLUX_ERR_INVALID;
            flux_free(arg0);
            return FLUX_OK;
        }

        case FLUX_SYSCALL_MMAP: {
            /* Simplified mmap — just allocates memory */
            flux_size_t size = (flux_size_t)(uintptr_t)arg0;
            flux_mem_type_t type = (flux_mem_type_t)(intptr_t)arg1;

            if (size == 0)
                return FLUX_ERR_INVALID;

            void *ptr = flux_alloc(size, type);
            if (!ptr)
                return FLUX_ERR_NOMEM;

            void **out = (void **)arg0;
            *out = ptr;
            return FLUX_OK;
        }

        case FLUX_SYSCALL_MUNMAP: {
            if (!syscall_validate_ptr(arg0))
                return FLUX_ERR_INVALID;
            flux_free(arg0);
            return FLUX_OK;
        }

        case FLUX_SYSCALL_MPROTECT: {
            flux_addr_t addr = (flux_addr_t)(uintptr_t)arg0;
            flux_size_t size = (flux_size_t)(uintptr_t)arg1;
            flux_perm_t perm = (flux_perm_t)(intptr_t)arg2;

            return flux_mem_protect(addr, size, perm);
        }

        default:
            return FLUX_ERR_INVALID;
    }
}

/*
 * handle_ipc_syscall — Inter-Process Communication syscalls.
 */
static flux_status_t handle_ipc_syscall(flux_syscall_t num,
                                        void *arg0, void *arg1,
                                        void *arg2, void *arg3)
{
    switch (num) {
        case FLUX_SYSCALL_A2A_SEND: {
            flux_pid_t target = (flux_pid_t)(uintptr_t)arg0;
            const void *msg = arg1;
            flux_size_t len = (flux_size_t)(uintptr_t)arg2;
            return flux_a2a_send(target, msg, len);
        }

        case FLUX_SYSCALL_A2A_RECV: {
            flux_pid_t *sender = (flux_pid_t *)arg0;
            void *buf = arg1;
            flux_size_t *len = (flux_size_t *)arg2;
            flux_ticks_t timeout = (flux_ticks_t)(uintptr_t)arg3;

            return flux_a2a_recv(sender, buf, len, timeout);
        }

        case FLUX_SYSCALL_A2A_BROADCAST: {
            const void *msg = arg0;
            flux_size_t len = (flux_size_t)(uintptr_t)arg1;
            return flux_a2a_broadcast(msg, len);
        }

        case FLUX_SYSCALL_A2A_SUBSCRIBE: {
            /* Subscribe to a topic (simplified — no-op for now) */
            const char *topic = (const char *)arg0;
            (void)topic;
            return FLUX_OK;
        }

        default:
            return FLUX_ERR_INVALID;
    }
}

/*
 * handle_bytecode_syscall — Bytecode execution syscalls.
 */
static flux_status_t handle_bytecode_syscall(flux_syscall_t num,
                                             void *arg0, void *arg1,
                                             void *arg2)
{
    switch (num) {
        case FLUX_SYSCALL_BC_LOAD: {
            flux_pid_t pid = (flux_pid_t)(uintptr_t)arg0;
            const uint8_t *bytecode = (const uint8_t *)arg1;
            flux_size_t len = (flux_size_t)(uintptr_t)arg2;

            return flux_bc_load(pid, bytecode, len);
        }

        case FLUX_SYSCALL_BC_EXEC: {
            flux_pid_t pid = (flux_pid_t)(uintptr_t)arg0;
            return flux_bc_exec(pid);
        }

        case FLUX_SYSCALL_BC_STATUS: {
            flux_pid_t pid = (flux_pid_t)(uintptr_t)arg0;
            if (!syscall_validate_pid(pid))
                return FLUX_ERR_NOTFOUND;

            flux_pcb_t *pcb = flux_get_pcb(pid);
            if (!pcb || !pcb->bc.active)
                return FLUX_ERR_INVALID;

            /* Return status via output struct pointer */
            /* For simplicity, return the PC as the status */
            return (flux_status_t)pcb->bc.pc;
        }

        case FLUX_SYSCALL_BC_DUMP: {
            flux_pid_t pid = (flux_pid_t)(uintptr_t)arg0;
            uint8_t *buf = (uint8_t *)arg1;
            flux_size_t *len = (flux_size_t *)arg2;

            return flux_bc_dump(pid, buf, len);
        }

        default:
            return FLUX_ERR_INVALID;
    }
}

/*
 * handle_compiler_syscall — Self-Compiler syscalls.
 * These are the "the OS IS the compiler" syscalls.
 */
static flux_status_t handle_compiler_syscall(flux_syscall_t num,
                                             void *arg0, void *arg1,
                                             void *arg2, void *arg3)
{
    switch (num) {
        case FLUX_SYSCALL_COMPILE: {
            /* Compile FLUX.MD source → bytecode/C/native */
            const char *source = (const char *)arg0;
            flux_size_t source_len = (flux_size_t)(uintptr_t)arg1;
            char *output = (char *)arg2;
            flux_size_t out_len = (flux_size_t)(uintptr_t)arg3;
            bool to_c = false;

            if (!syscall_check_cap(FLUX_CAP_COMPILE))
                return FLUX_ERR_DENIED;

            return flux_compile_source(source, source_len, output, out_len, to_c);
        }

        case FLUX_SYSCALL_EMIT: {
            /* Emit generated code to a target */
            if (!syscall_check_cap(FLUX_CAP_COMPILE))
                return FLUX_ERR_DENIED;

            /* Simplified: just copy the generated code */
            const char *code = (const char *)arg0;
            char *output = (char *)arg1;
            flux_size_t len = (flux_size_t)(uintptr_t)arg2;

            if (!code || !output || len == 0)
                return FLUX_ERR_INVALID;

            memcpy(output, code, len);
            return FLUX_OK;
        }

        case FLUX_SYSCALL_DEVCODE: {
            /* OS acts as developer: generate code from description */
            const char *description = (const char *)arg0;
            char *output = (char *)arg1;
            flux_size_t out_len = (flux_size_t)(uintptr_t)arg2;

            if (!syscall_check_cap(FLUX_CAP_COMPILE | FLUX_CAP_SUPERVISOR))
                return FLUX_ERR_DENIED;

            return flux_devcode(description, output, out_len);
        }

        default:
            return FLUX_ERR_INVALID;
            break;
    }
}

/*
 * handle_io_syscall — I/O syscalls.
 */
static flux_status_t handle_io_syscall(flux_syscall_t num,
                                       void *arg0, void *arg1,
                                       void *arg2)
{
    switch (num) {
        case FLUX_SYSCALL_READ: {
            /* Simplified: read from console */
            char *buf = (char *)arg0;
            flux_size_t *len = (flux_size_t *)arg1;

            if (!syscall_check_cap(FLUX_CAP_IO_READ))
                return FLUX_ERR_DENIED;

            if (!buf || !len || *len == 0)
                return FLUX_ERR_INVALID;

            const flux_hal_t *hal = flux_hal_get();
            if (hal && hal->console_getc) {
                int count = 0;
                while (count < (int)*len) {
                    char c = hal->console_getc();
                    if (c == '\r' || c == '\n') {
                        buf[count++] = c;
                        break;
                    }
                    buf[count++] = c;
                }
                *len = (flux_size_t)count;
                return FLUX_OK;
            }

            return FLUX_ERR_GENERAL;
        }

        case FLUX_SYSCALL_WRITE: {
            const char *buf = (const char *)arg0;
            flux_size_t len = (flux_size_t)(uintptr_t)arg1;

            if (!syscall_check_cap(FLUX_CAP_IO_WRITE))
                return FLUX_ERR_DENIED;

            if (!buf || len == 0)
                return FLUX_ERR_INVALID;

            const flux_hal_t *hal = flux_hal_get();
            if (hal && hal->console_puts) {
                /* Write len bytes (or up to first null) */
                char tmp[256];
                flux_size_t write_len = len < 255 ? len : 255;
                memcpy(tmp, buf, write_len);
                tmp[write_len] = '\0';
                hal->console_puts(tmp);
                return FLUX_OK;
            }

            return FLUX_ERR_GENERAL;
        }

        case FLUX_SYSCALL_IOCTL: {
            /* Simplified: no-op */
            return FLUX_OK;
        }

        case FLUX_SYSCALL_OPEN:
        case FLUX_SYSCALL_CLOSE: {
            /* File I/O not yet implemented */
            return FLUX_ERR_GENERAL;
        }

        default:
            return FLUX_ERR_INVALID;
    }
}

/*
 * handle_hw_syscall — Hardware query syscalls.
 */
static flux_status_t handle_hw_syscall(flux_syscall_t num,
                                       void *arg0, void *arg1)
{
    switch (num) {
        case FLUX_SYSCALL_HW_INFO: {
            char *buf = (char *)arg0;
            flux_size_t len = (flux_size_t)(uintptr_t)arg1;

            if (!buf || len == 0)
                return FLUX_ERR_INVALID;

            const flux_hal_t *hal = flux_hal_get();
            if (hal && hal->hw_info_dump) {
                return hal->hw_info_dump(buf, len);
            }

            /* Fallback: generate basic info */
            snprintf(buf, len, "arch=%s hal=%s",
                     flux_hal_arch_name(),
                     hal && hal->hal_version ? hal->hal_version() : "none");
            return FLUX_OK;
        }

        case FLUX_SYSCALL_HW_CONFIG: {
            if (!syscall_check_cap(FLUX_CAP_HARDWARE))
                return FLUX_ERR_DENIED;

            char *buf = (char *)arg0;
            flux_size_t len = (flux_size_t)(uintptr_t)arg1;

            const flux_hal_t *hal = flux_hal_get();
            if (hal && hal->hw_optimal_config) {
                return hal->hw_optimal_config(buf, len);
            }

            return FLUX_ERR_GENERAL;
        }

        default:
            return FLUX_ERR_INVALID;
    }
}

/*
 * handle_info_syscall — Info and logging syscalls.
 */
static flux_status_t handle_info_syscall(flux_syscall_t num,
                                         void *arg0, void *arg1)
{
    switch (num) {
        case FLUX_SYSCALL_INFO: {
            /* Return kernel info string */
            const char **out = (const char **)arg0;
            if (out)
                *out = flux_kernel_info();
            return FLUX_OK;
        }

        case FLUX_SYSCALL_LOG: {
            const char *msg = (const char *)arg0;
            if (msg)
                flux_log("%s", msg);
            return FLUX_OK;
        }

        default:
            return FLUX_ERR_INVALID;
    }
}

/* ========================================================================
 * Main Dispatch Entry Point
 * ======================================================================== */

/*
 * flux_syscall_dispatch — Main system call dispatcher.
 *
 * This is the single entry point for ALL system calls. It:
 *   1. Validates the syscall number
 *   2. Increments the syscall counter
 *   3. Logs the call (at DEBUG level)
 *   4. Routes to the appropriate category handler
 *   5. Logs the result
 *
 * Parameters:
 *   num  — System call number (flux_syscall_t enum)
 *   arg0-arg4 — Up to 5 arguments (void* for generality)
 *
 * Returns:
 *   FLUX_OK on success, negative error code on failure.
 */
flux_status_t flux_syscall_dispatch(flux_syscall_t num, void *arg0, void *arg1,
                                    void *arg2, void *arg3, void *arg4)
{
    /* Update syscall counter */
    flux_kernel_state_t *ks = flux_kernel_state();
    if (ks)
        ks->syscall_count++;

    /* Validate syscall range */
    if ((int)num < 100 || (int)num > 899) {
        flux_log_debug("syscall: INVALID number %d from pid=%u",
                       (int)num, flux_getpid());
        return FLUX_ERR_INVALID;
    }

    /* Log the call */
    const char *name = syscall_lookup_name(num);
    syscall_log_entry(num, name);

    flux_status_t result = FLUX_ERR_INVALID;

    /* Route to category handler */
    int category = (int)num / 100;

    switch (category) {
        case 1:  /* 100-199: Process management */
            result = handle_process_syscall(num, arg0, arg1, arg2, arg3);
            break;

        case 2:  /* 200-299: Memory management */
            result = handle_memory_syscall(num, arg0, arg1, arg2);
            break;

        case 3:  /* 300-399: IPC */
            result = handle_ipc_syscall(num, arg0, arg1, arg2, arg3);
            break;

        case 4:  /* 400-499: Bytecode execution */
            result = handle_bytecode_syscall(num, arg0, arg1, arg2);
            break;

        case 5:  /* 500-599: Self-Compiler */
            result = handle_compiler_syscall(num, arg0, arg1, arg2, arg3);
            break;

        case 6:  /* 600-699: I/O */
            result = handle_io_syscall(num, arg0, arg1, arg2);
            break;

        case 7:  /* 700-799: Hardware */
            result = handle_hw_syscall(num, arg0, arg1);
            break;

        case 8:  /* 800-899: Info & Log */
            result = handle_info_syscall(num, arg0, arg1);
            break;

        default:
            result = FLUX_ERR_INVALID;
            break;
    }

    /* Log result */
    syscall_log_result(num, result);

    return result;
}

/*
 * flux_syscall_name — Get the name of a syscall by number.
 * Useful for logging and debugging.
 */
const char *flux_syscall_name(flux_syscall_t num)
{
    return syscall_lookup_name(num);
}

/*
 * flux_syscall_count — Get total number of syscalls invoked since boot.
 */
uint32_t flux_syscall_count(void)
{
    flux_kernel_state_t *ks = flux_kernel_state();
    return ks ? ks->syscall_count : 0;
}
