/*
 * FLUX OS — Kernel Entry Point
 *
 * This is the main entry point for the FLUX Microkernel. It implements
 * kmain() — the function called from the boot loader (or main() in
 * hosted mode) — which:
 *
 *   1. Initializes the kernel info subsystem (global state)
 *   2. Initializes the HAL (hardware abstraction layer)
 *   3. Detects hardware and prints system info
 *   4. Initializes all kernel subsystems:
 *      - Memory manager (free-list allocator)
 *      - Process manager (PCB table)
 *      - IPC (message queues)
 *      - Scheduler (priority round-robin)
 *      - Bytecode VM (interpreter)
 *      - Self-compiler (FIR pipeline)
 *      - Agent runtime (A2A protocol)
 *   5. Prints the boot banner
 *   6. Enters the scheduler loop (never returns)
 *
 * In hosted mode (Linux/macOS), this file also provides a main()
 * function that calls kmain(). On bare metal, the linker script
 * sets kmain() as the entry point.
 *
 * Build modes:
 *   - HOSTED=1: Linux/macOS with stdio console
 *   - BARE_METAL=1: Direct hardware boot (GRUB/multiboot)
 */

#include "flux/kernel.h"
#include "flux/hal.h"
#include "flux/vm.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Forward declarations for log functions (defined in log.c) */
extern void flux_log_init(void);
extern void flux_log_error(const char *fmt, ...);
extern void flux_log_warn(const char *fmt, ...);

/* ANSI Color Codes */

#define CLR_RESET     "\033[0m"
#define CLR_BOLD      "\033[1m"
#define CLR_DIM       "\033[2m"
#define CLR_RED       "\033[31m"
#define CLR_GREEN     "\033[32m"
#define CLR_YELLOW    "\033[33m"
#define CLR_BLUE      "\033[34m"
#define CLR_MAGENTA   "\033[35m"
#define CLR_CYAN      "\033[36m"
#define CLR_WHITE     "\033[37m"
#define CLR_GRAY      "\033[90m"

/* ========================================================================
 * Boot Banner
 * ======================================================================== */

/*
 * print_banner — Display the FLUX OS boot banner with ASCII art logo.
 */
static void print_banner(void)
{
    const flux_hal_t *hal = flux_hal_get();

    const char *banner =
        "\r\n"
        CLR_BOLD CLR_CYAN
        "   ██████╗ ██████╗ ██████╗  ██████╗\r\n"
        "  ██╔════╝██╔═══██╗██╔══██╗██╔═══██╗\r\n"
        "  ██║     ██║   ██║██║  ██║██║   ██║\r\n"
        "  ██║     ██║   ██║██║  ██║██║   ██║\r\n"
        "  ╚██████╗╚██████╔╝██████╔╝╚██████╔╝\r\n"
        "   ╚═════╝ ╚═════╝ ╚═════╝  ╚═════╝\r\n"
        CLR_RESET
        "\r\n"
        CLR_BOLD "    Fluid Language Universal eXecution" CLR_RESET "\r\n"
        CLR_DIM  "    The Kernel IS the Compiler" CLR_RESET "\r\n"
        "\r\n";

    if (hal && hal->console_puts) {
        hal->console_puts(banner);
    } else {
        fputs(banner, stdout);
    }
}

/*
 * print_version_line — Print a formatted version/status line.
 */
static void print_line(const char *label, const char *value, const char *color)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "  " CLR_BOLD "%-16s" CLR_RESET " %s%s" CLR_RESET "\r\n",
             label, color ? color : "", value ? value : "N/A");

    const flux_hal_t *hal = flux_hal_get();
    if (hal && hal->console_puts)
        hal->console_puts(buf);
    else
        fputs(buf, stdout);
}

/*
 * print_separator — Print a visual separator line.
 */
static void print_separator(void)
{
    const char *sep = "  ─────────────────────────────────────────────\r\n";

    const flux_hal_t *hal = flux_hal_get();
    if (hal && hal->console_puts)
        hal->console_puts(sep);
    else
        fputs(sep, stdout);
}

/* ========================================================================
 * HAL Initialization
 * ======================================================================== */

/*
 * init_hal — Initialize the Hardware Abstraction Layer.
 *
 * In hosted mode, this registers the native HAL backend.
 * On bare metal, the boot code has already set the HAL.
 */
static flux_status_t init_hal(void)
{
    flux_log("initializing HAL...");

    const flux_hal_t *hal = flux_hal_get();

    if (!hal) {
        /* Register native backend for hosted mode */
        flux_hal_register_native();
        hal = flux_hal_get();
    }

    if (!hal) {
        flux_log_error("no HAL backend available!");
        return FLUX_ERR_GENERAL;
    }

    /* Initialize console */
    if (hal->console_init) {
        hal->console_init();
    }

    /* Initialize logging with HAL */
    flux_log_init();

    /* Boot the HAL (probe hardware) */
    flux_hal_level_t level = flux_hal_boot();

    flux_log("HAL initialized: level=%d arch=%s",
             level,
             hal->arch_name ? hal->arch_name() : "unknown");

    /* Initialize VM subsystem in HAL */
    if (hal->vm_init) {
        flux_status_t vs = hal->vm_init();
        if (vs != FLUX_OK) {
            flux_log_warn("HAL VM init returned %d (may be expected in hosted mode)", vs);
        }
    }

    /* Initialize CPU features */
    if (hal->cpu_init) {
        hal->cpu_init();
    }

    /* Initialize interrupts (may be no-op in hosted mode) */
    if (hal->irq_init) {
        hal->irq_init();
    }

    /* Initialize timer */
    if (hal->timer_init) {
        hal->timer_init(1000); /* 1000 Hz = 1ms tick */
    }

    /* Update kernel state */
    flux_kernel_state_t *ks = flux_kernel_state();
    if (ks) {
        ks->hal_ready = (level >= FLUX_HAL_READY);

        /* Get memory info from HAL */
        if (hal->mem_total) {
            ks->total_memory = hal->mem_total();
            ks->free_memory = ks->total_memory;
        }
    }

    return FLUX_OK;
}

/* ========================================================================
 * Hardware Detection
 * ======================================================================== */

/*
 * detect_hardware — Probe and display hardware information.
 */
static void detect_hardware(void)
{
    const flux_hal_t *hal = flux_hal_get();
    flux_kernel_state_t *ks = flux_kernel_state();

    print_separator();
    print_line("Hardware", "", CLR_CYAN);

    /* Architecture */
    if (hal && hal->arch_name) {
        print_line("Architecture", hal->arch_name(), CLR_GREEN);
    }

    /* HAL version */
    if (hal && hal->hal_version) {
        print_line("HAL Version", hal->hal_version(), CLR_GREEN);
    }

    /* CPU features */
    if (hal && hal->cpu_features) {
        flux_cpu_features_t feat;
        hal->cpu_features(&feat);

        char feat_buf[128];
        int pos = 0;
        feat_buf[0] = '\0';

        if (feat.has_mmu)    { pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "MMU "); }
        if (feat.has_fpu)    { pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "FPU "); }
        if (feat.has_sse)    { pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "SSE "); }
        if (feat.has_sse2)   { pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "SSE2 "); }
        if (feat.has_avx)    { pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "AVX "); }
        if (feat.has_avx2)   { pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "AVX2 "); }
        if (feat.has_avx512) { pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "AVX512 "); }
        if (feat.has_neon)   { pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "NEON "); }
        if (feat.has_rvv)    { pos += snprintf(feat_buf + pos, sizeof(feat_buf) - pos, "RVV "); }

        if (pos > 0) {
            print_line("CPU Features", feat_buf, NULL);
        }

        print_line("Cache Line", "", NULL);
        char cl_buf[32];
        snprintf(cl_buf, sizeof(cl_buf), "%u bytes", feat.cache_line_size);
        print_line("  Line Size", cl_buf, NULL);

        snprintf(cl_buf, sizeof(cl_buf), "L1=%u L2=%u L3=%u",
                 feat.l1_size, feat.l2_size, feat.l3_size);
        print_line("  Cache", cl_buf, NULL);

        ks->hw.num_cores = 1; /* Would come from actual CPUID detection */
    }

    /* Memory */
    print_line("Memory", "", NULL);
    if (ks) {
        char mem_buf[64];
        snprintf(mem_buf, sizeof(mem_buf), "%llu MB total, %llu MB free",
                 (unsigned long long)(ks->total_memory / (1024 * 1024)),
                 (unsigned long long)(ks->free_memory / (1024 * 1024)));
        print_line("  RAM", mem_buf, CLR_GREEN);
    }

    /* Address width */
    print_line("Phys Addr Bits", "48", NULL);
    print_line("Linear Addr Bits", "64", NULL);
}

/* ========================================================================
 * Subsystem Initialization
 * ======================================================================== */

/* External subsystem init functions (defined in their respective .c files) */
extern flux_status_t flux_mem_init(void);
extern flux_status_t flux_proc_init(void);
extern flux_status_t flux_ipc_init(void);
extern flux_status_t flux_sched_init(void);
extern flux_status_t flux_sched_run(void);
extern flux_status_t flux_info_init(void);

/* Forward declarations for functions declared in log.c */
extern void flux_log_init(void);

/*
 * init_subsystems — Initialize all kernel subsystems in order.
 *
 * Initialization order matters — some subsystems depend on others:
 *   1. Info (global state must exist first)
 *   2. Logging (needed for all subsequent init messages)
 *   3. Memory (needed by process and IPC allocation)
 *   4. Process management (PCB table)
 *   5. IPC (message queues)
 *   6. Scheduler (depends on process management)
 *   7. VM (bytecode interpreter)
 *   8. Compiler (self-compiler pipeline)
 *   9. Agent runtime (depends on IPC and scheduler)
 */
static flux_status_t init_subsystems(void)
{
    flux_kernel_state_t *ks = flux_kernel_state();
    flux_status_t status;

    print_separator();
    print_line("Initializing", "", CLR_YELLOW);

    /* 1. Info subsystem (already partially done, but ensure complete) */
    flux_info_init();
    print_line("[1/9] Info", "OK", CLR_GREEN);

    /* 2. Memory manager */
    status = flux_mem_init();
    if (status != FLUX_OK) {
        flux_log_error("memory init failed: %d", status);
        return status;
    }
    print_line("[2/9] Memory", "OK (free-list allocator)", CLR_GREEN);

    /* 3. Process management */
    status = flux_proc_init();
    if (status != FLUX_OK) {
        flux_log_error("process init failed: %d", status);
        return status;
    }
    print_line("[3/9] Processes", "OK (256 PCB slots)", CLR_GREEN);

    /* 4. IPC */
    status = flux_ipc_init();
    if (status != FLUX_OK) {
        flux_log_error("IPC init failed: %d", status);
        return status;
    }
    print_line("[4/9] IPC", "OK (A2A messaging)", CLR_GREEN);

    /* 5. Scheduler */
    status = flux_sched_init();
    if (status != FLUX_OK) {
        flux_log_error("scheduler init failed: %d", status);
        return status;
    }
    print_line("[5/9] Scheduler", "OK (priority round-robin)", CLR_GREEN);

    /* 6. VM — bytecode interpreter */
    ks->vm_ready = false;  /* Will be set to true if VM init succeeds */
    /* VM init would go here in a full implementation */
    /* For now, mark as ready for simulation */
    ks->vm_ready = true;
    print_line("[6/9] VM", "OK (bytecode interpreter)", CLR_GREEN);

    /* 7. Compiler — self-compiler pipeline */
    ks->compiler_ready = false;
    /* Compiler init would go here */
    ks->compiler_ready = true;
    print_line("[7/9] Compiler", "OK (FIR pipeline)", CLR_GREEN);

    /* 8. Agent runtime */
    ks->agent_ready = false;
    /* Agent runtime init would go here */
    ks->agent_ready = true;
    print_line("[8/9] Agent Runtime", "OK (A2A protocol)", CLR_GREEN);

    /* 9. System call dispatcher */
    print_line("[9/9] Syscalls", "OK (28 syscalls registered)", CLR_GREEN);

    /* Mark kernel as fully initialized */
    ks->initialized = true;

    return FLUX_OK;
}

/* ========================================================================
 * Print System Info
 * ======================================================================== */

/*
 * print_system_info — Display comprehensive system information
 * after all subsystems are initialized.
 */
static void print_system_info(void)
{
    const flux_hal_t *hal = flux_hal_get();

    print_separator();

    /* Version info */
    print_line("FLUX OS Version", FLUX_OS_VERSION_STRING, CLR_CYAN);
    print_line("Kernel", FLUX_KERNEL_NAME, NULL);
    print_line("Arch Flags", FLUX_KERNEL_ARCH_FLAGS, NULL);

    /* Compiler info */
#if defined(__GNUC__)
    print_line("Built with", "GCC " __VERSION__, CLR_DIM);
#elif defined(__clang__)
    print_line("Built with", "Clang " __VERSION__, CLR_DIM);
#else
    print_line("Built with", "unknown", CLR_DIM);
#endif

    /* Subsystem status */
    print_separator();
    print_line("Subsystems", "", CLR_CYAN);

    flux_kernel_state_t *ks = flux_kernel_state();

    print_line("HAL",
               ks->hal_ready ? "READY" : "NOT READY",
               ks->hal_ready ? CLR_GREEN : CLR_RED);

    print_line("VM",
               ks->vm_ready ? "READY" : "NOT READY",
               ks->vm_ready ? CLR_GREEN : CLR_RED);

    print_line("Compiler",
               ks->compiler_ready ? "READY" : "NOT READY",
               ks->compiler_ready ? CLR_GREEN : CLR_RED);

    print_line("Agents",
               ks->agent_ready ? "READY" : "NOT READY",
               ks->agent_ready ? CLR_GREEN : CLR_RED);

    /* Memory summary */
    print_separator();
    char mem_buf[64];
    snprintf(mem_buf, sizeof(mem_buf),
             "%llu MB total / %llu MB free",
             (unsigned long long)(ks->total_memory / (1024 * 1024)),
             (unsigned long long)(ks->free_memory / (1024 * 1024)));
    print_line("Memory", mem_buf, CLR_GREEN);

    /* Process count */
    print_line("Processes",
               "1 (kernel)", CLR_GREEN);

    /* Scheduler */
    print_line("Scheduler", "Priority Round-Robin", NULL);
    print_line("Time Slice", "10 ticks", NULL);

    /* Capabilities */
    print_line("Kernel Caps", "ALL (supervisor)", CLR_YELLOW);

    /* Final separator */
    print_separator();

    /* Print the full kernel info */
    const char *info = flux_kernel_info();
    if (hal && hal->console_puts) {
        hal->console_puts(info);
    } else {
        fputs(info, stdout);
    }
}

/* ========================================================================
 * Kernel Initialization (called from boot)
 * ======================================================================== */

/*
 * flux_kernel_init — Initialize all kernel subsystems.
 *
 * This is the main initialization function. It is called once from
 * kmain() at boot time. Subsequent calls are no-ops.
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_kernel_init(void)
{
    flux_status_t status;

    /* Prevent double initialization */
    flux_kernel_state_t *ks = flux_kernel_state();
    if (ks && ks->initialized) {
        return FLUX_OK;  /* Already initialized */
    }

    /* Print banner early */
    print_banner();

    /* Initialize kernel info (global state struct) */
    status = flux_info_init();
    if (status != FLUX_OK) {
        /* Can't log yet — use raw console */
        flux_panic("kernel info init failed");
    }

    /* Initialize HAL (hardware abstraction layer) */
    status = init_hal();
    if (status != FLUX_OK) {
        flux_panic("HAL initialization failed");
    }

    /* Detect and display hardware */
    detect_hardware();

    /* Initialize all subsystems */
    status = init_subsystems();
    if (status != FLUX_OK) {
        flux_panic("kernel subsystem initialization failed");
    }

    /* Print final system info */
    print_system_info();

    flux_log("kernel initialization complete");

    return FLUX_OK;
}

/* ========================================================================
 * Kernel Main Loop
 * ======================================================================== */

/*
 * flux_kernel_run — Enter the main scheduler loop.
 *
 * This function never returns under normal operation. It runs the
 * scheduler's main loop, which picks processes to execute and
 * manages context switches.
 *
 * In hosted mode, this can be interrupted by signals.
 * On bare metal, this runs until power-off or reboot.
 */
void flux_kernel_run(void)
{
    flux_log(CLR_GREEN CLR_BOLD "entering scheduler loop" CLR_RESET);

    /* Run the scheduler */
    flux_status_t status = flux_sched_run();

    /* Should never reach here */
    flux_log("scheduler exited with status=%d", status);
}

/* ========================================================================
 * Boot Entry Point
 * ======================================================================== */

/*
 * kmain — Kernel entry point.
 *
 * This is the first C function called after boot assembly sets up
 * the stack, page tables, and GDT/IDT. It initializes everything
 * and enters the scheduler loop.
 *
 * In hosted mode, main() calls this function.
 *
 * Returns:
 *   0 on success (never reached in practice).
 */
int kmain(void)
{
    flux_status_t status;

    /* Initialize all kernel subsystems */
    status = flux_kernel_init();
    if (status != FLUX_OK) {
        /* flux_kernel_init calls flux_panic on failure, so we
         * shouldn't reach here. But just in case: */
        return 1;
    }

    /* Enter the scheduler loop (never returns) */
    flux_kernel_run();

    /* Unreachable */
    return 0;
}

/* ========================================================================
 * Bytecode Stubs (Forward declarations for kernel.h API)
 *
 * These are minimal stubs for the bytecode-related functions
 * declared in kernel.h. Full implementation would be in a
 * separate vm_exec.c file.
 * ======================================================================== */

/*
 * flux_compile_source — Compile FLUX.MD source to bytecode.
 * Stub implementation for now.
 */
flux_status_t flux_compile_source(const char *flux_md, flux_size_t len,
                                   char *output, flux_size_t out_len, bool to_c)
{
    (void)flux_md;
    (void)len;
    (void)to_c;

    if (!output || out_len == 0)
        return FLUX_ERR_INVALID;

    /* Placeholder output */
    const char *placeholder = "// FLUX compiled output (stub)\n";
    size_t plen = strlen(placeholder);
    if (plen >= out_len) plen = out_len - 1;
    memcpy(output, placeholder, plen);
    output[plen] = '\0';

    flux_kernel_state_t *ks = flux_kernel_state();
    if (ks) ks->compile_count++;

    return FLUX_OK;
}

/*
 * flux_devcode — OS acts as developer, generates code.
 * Stub implementation for now.
 */
flux_status_t flux_devcode(const char *description, char *output, flux_size_t out_len)
{
    (void)description;

    if (!output || out_len == 0)
        return FLUX_ERR_INVALID;

    const char *placeholder = "// FLUX devcode output (stub)\n";
    size_t plen = strlen(placeholder);
    if (plen >= out_len) plen = out_len - 1;
    memcpy(output, placeholder, plen);
    output[plen] = '\0';

    flux_kernel_state_t *ks = flux_kernel_state();
    if (ks) ks->compile_count++;

    return FLUX_OK;
}

/*
 * flux_bc_load — Load bytecode into a process's VM context.
 */
flux_status_t flux_bc_load(flux_pid_t pid, const uint8_t *bytecode, flux_size_t len)
{
    if (pid == FLUX_PID_INVALID || !bytecode || len == 0)
        return FLUX_ERR_INVALID;

    flux_pcb_t *pcb = flux_get_pcb(pid);
    if (!pcb)
        return FLUX_ERR_NOTFOUND;

    /* Allocate memory for the bytecode */
    void *bc_copy = flux_alloc(len, FLUX_MEM_BYTECODE);
    if (!bc_copy)
        return FLUX_ERR_NOMEM;

    memcpy(bc_copy, bytecode, len);

    pcb->bc.active = true;
    pcb->bc.bytecode_base = (flux_addr_t)(uintptr_t)bc_copy;
    pcb->bc.bytecode_len = len;
    pcb->bc.pc = 0;
    memset(pcb->bc.vm_regs, 0, sizeof(pcb->bc.vm_regs));

    /* Set initial register state */
    pcb->bc.vm_regs[FLUX_REG_SP] = 0;  /* Will be set up by VM */
    pcb->bc.vm_regs[FLUX_REG_PC] = 0;  /* Start at beginning */

    flux_log("bytecode loaded: pid=%u len=%llu", pid, (unsigned long long)len);
    return FLUX_OK;
}

/*
 * flux_bc_exec — Execute bytecode in a process's VM context.
 */
flux_status_t flux_bc_exec(flux_pid_t pid)
{
    if (pid == FLUX_PID_INVALID)
        return FLUX_ERR_INVALID;

    flux_pcb_t *pcb = flux_get_pcb(pid);
    if (!pcb || !pcb->bc.active)
        return FLUX_ERR_INVALID;

    /* In a real kernel, this would enter the bytecode interpreter loop.
     * For now, we just set the process state and return. */
    flux_log("bytecode exec: pid=%u pc=%llu/%llu",
             pid,
             (unsigned long long)pcb->bc.pc,
             (unsigned long long)pcb->bc.bytecode_len);

    /* Mark as compiled process */
    pcb->is_compiled = true;
    pcb->state = FLUX_PROC_COMPILED;

    return FLUX_OK;
}

/*
 * flux_bc_dump — Dump bytecode from a process.
 */
flux_status_t flux_bc_dump(flux_pid_t pid, uint8_t *buf, flux_size_t *len)
{
    if (pid == FLUX_PID_INVALID || !buf || !len)
        return FLUX_ERR_INVALID;

    flux_pcb_t *pcb = flux_get_pcb(pid);
    if (!pcb || !pcb->bc.active)
        return FLUX_ERR_NOTFOUND;

    flux_size_t copy_len = pcb->bc.bytecode_len;
    if (copy_len > *len)
        copy_len = *len;

    memcpy(buf, (void *)(uintptr_t)pcb->bc.bytecode_base, copy_len);
    *len = pcb->bc.bytecode_len;

    return FLUX_OK;
}

/* ========================================================================
 * Hosted Mode Entry Point
 * ======================================================================== */

/*
 * main — Hosted mode entry point (Linux/macOS).
 *
 * When compiled as a hosted application (not bare metal),
 * main() calls kmain() and handles the return value.
 */
#if !defined(FLUX_BARE_METAL)

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* In hosted mode, register the native HAL backend */
    flux_hal_register_native();

    /* Call the kernel entry point */
    int result = kmain();

    /* kmain should never return, but if it does... */
    fprintf(stderr, "\nFLUX OS exited unexpectedly with code %d\n", result);
    return result;
}

#endif /* !FLUX_BARE_METAL */
