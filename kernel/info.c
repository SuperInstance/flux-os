/*
 * FLUX OS — Kernel Info Subsystem
 *
 * Provides introspection into the kernel's state, version, and
 * capabilities. This module implements:
 *
 *   - flux_kernel_info() — returns a formatted string with system info
 *   - flux_kernel_state() — returns a pointer to the global state struct
 *
 * The global kernel state struct is the single source of truth for the
 * kernel's current status. It is accessible from all subsystems and
 * updated atomically where needed.
 *
 * In a hosted environment, this module also queries the HAL and
 * system environment for architecture and hardware details.
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
#define FLUX_CAP_MEMORY         0x0000000000000020ULL
#define FLUX_CAP_HARDWARE       0x0000000000000040ULL
#define FLUX_CAP_NETWORK        0x0000000000000080ULL
#define FLUX_CAP_FILESYSTEM     0x0000000000000100ULL
#define FLUX_CAP_DEBUG          0x0000000000000200ULL
#define FLUX_CAP_SUPERVISOR     0x0000000000000400ULL
#endif

/* ========================================================================
 * Global Kernel State (Single Instance)
 *
 * This is the one and only kernel state structure. All subsystems
 * read from and write to this struct. It is initialized once at boot
 * and never moved or freed.
 * ======================================================================== */

static flux_kernel_state_t s_kernel_state;

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/*
 * info_arch_name — Get a human-readable architecture name.
 */
static const char *info_arch_name(void)
{
    const flux_hal_t *hal = flux_hal_get();
    if (hal && hal->arch_name) {
        return hal->arch_name();
    }

    /* Fallback: detect from preprocessor */
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "ARM64";
#elif defined(__riscv) && (__riscv_xlen == 64)
    return "RISC-V 64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86 (32-bit)";
#elif defined(__wasm32__) || defined(__wasm64__)
    return "WebAssembly";
#elif defined(__EMSCRIPTEN__)
    return "WASM32 (Emscripten)";
#else
    return "unknown";
#endif
}

/*
 * info_compiler_info — Get compiler identification string.
 */
static const char *info_compiler_info(void)
{
#if defined(__GNUC__)
    return "GCC " __VERSION__;
#elif defined(__clang__)
    return "Clang " __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " _MSC_FULL_VER;
#else
    return "unknown compiler";
#endif
}

/*
 * info_format_size — Format a size in bytes as a human-readable string.
 * Produces output like "16.0 MB", "256 KB", "4096 B".
 */
static void info_format_size(flux_size_t bytes, char *buf, int buf_size)
{
    const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    int unit_idx = 0;
    double size = (double)bytes;

    while (size >= 1024.0 && unit_idx < 4) {
        size /= 1024.0;
        unit_idx++;
    }

    if (unit_idx == 0) {
        snprintf(buf, buf_size, "%llu %s",
                 (unsigned long long)bytes, units[0]);
    } else {
        snprintf(buf, buf_size, "%.1f %s", size, units[unit_idx]);
    }
}

/*
 * info_capability_string — Get a human-readable capability bitmask.
 */
static void info_capability_string(flux_cap_t caps, char *buf, int buf_size)
{
    int pos = 0;

#define CHECK_CAP(flag, name) \
    if ((caps & (flag)) && pos < buf_size - 2) { \
        if (pos > 0) buf[pos++] = ','; \
        const char *n = (name); \
        while (*n && pos < buf_size - 2) buf[pos++] = *n++; \
    }

    CHECK_CAP(FLUX_CAP_SPAWN,       "SPAWN");
    CHECK_CAP(FLUX_CAP_COMMUNICATE, "COMM");
    CHECK_CAP(FLUX_CAP_COMPILE,     "COMPILE");
    CHECK_CAP(FLUX_CAP_IO_READ,     "IO_R");
    CHECK_CAP(FLUX_CAP_IO_WRITE,    "IO_W");
    CHECK_CAP(FLUX_CAP_MEMORY,      "MEM");
    CHECK_CAP(FLUX_CAP_HARDWARE,    "HW");
    CHECK_CAP(FLUX_CAP_NETWORK,     "NET");
    CHECK_CAP(FLUX_CAP_FILESYSTEM,  "FS");
    CHECK_CAP(FLUX_CAP_DEBUG,       "DEBUG");
    CHECK_CAP(FLUX_CAP_SUPERVISOR,  "SUPER");

#undef CHECK_CAP

    buf[pos] = '\0';

    if (pos == 0) {
        snprintf(buf, buf_size, "NONE");
    }
}

/*
 * info_populate_hw — Fill in hardware info from HAL.
 */
static void info_populate_hw(void)
{
    const flux_hal_t *hal = flux_hal_get();

    if (hal) {
        /* Architecture detection */
        if (hal->detect_arch) {
            flux_arch_t arch = hal->detect_arch();
            switch (arch) {
                case FLUX_ARCH_X86_64:  snprintf(s_kernel_state.hw.arch, sizeof(s_kernel_state.hw.arch), "x86_64"); break;
                case FLUX_ARCH_ARM64:   snprintf(s_kernel_state.hw.arch, sizeof(s_kernel_state.hw.arch), "ARM64"); break;
                case FLUX_ARCH_RISCV64: snprintf(s_kernel_state.hw.arch, sizeof(s_kernel_state.hw.arch), "RISC-V64"); break;
                case FLUX_ARCH_WASM32:  snprintf(s_kernel_state.hw.arch, sizeof(s_kernel_state.hw.arch), "WASM32"); break;
                case FLUX_ARCH_NATIVE:  snprintf(s_kernel_state.hw.arch, sizeof(s_kernel_state.hw.arch), "native"); break;
                default:                snprintf(s_kernel_state.hw.arch, sizeof(s_kernel_state.hw.arch), "unknown"); break;
            }
        }

        /* HAL version */
        if (hal->hal_version) {
            const char *ver = hal->hal_version();
            snprintf(s_kernel_state.hw.hal_version,
                     sizeof(s_kernel_state.hw.hal_version), "%s", ver);
        }

        /* Memory info */
        if (hal->mem_total) {
            s_kernel_state.hw.total_ram = hal->mem_total();
        }
    }

    /* Compiler info */
    const char *arch = info_arch_name();
    snprintf(s_kernel_state.hw.cpu_vendor, sizeof(s_kernel_state.hw.cpu_vendor),
             "%s", arch);

    /* Defaults for fields not available from HAL */
    s_kernel_state.hw.num_cores = 1;
    s_kernel_state.hw.cpu_freq_hz = 0;
}

/* ========================================================================
 * Kernel Info Initialization
 * ======================================================================== */

/*
 * flux_info_init — Initialize the kernel info subsystem.
 * Populates hardware info and sets up the global state struct.
 */
flux_status_t flux_info_init(void)
{
    /* Clear the entire state */
    memset(&s_kernel_state, 0, sizeof(s_kernel_state));

    /* Set initial flags */
    s_kernel_state.initialized = false;
    s_kernel_state.hal_ready = false;
    s_kernel_state.vm_ready = false;
    s_kernel_state.compiler_ready = false;
    s_kernel_state.agent_ready = false;

    /* Populate hardware info */
    info_populate_hw();

    /* Set total memory from HAL or use default */
    if (s_kernel_state.hw.total_ram == 0) {
        const flux_hal_t *hal = flux_hal_get();
        if (hal && hal->mem_total) {
            s_kernel_state.total_memory = hal->mem_total();
        } else {
            s_kernel_state.total_memory = 16 * 1024 * 1024;  /* 16 MB default */
        }
        s_kernel_state.free_memory = s_kernel_state.total_memory;
    }

    /* Initial PID values */
    s_kernel_state.next_pid = FLUX_PID_FIRST_APP;
    s_kernel_state.current_pid = FLUX_PID_KERNEL;

    return FLUX_OK;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

/*
 * flux_kernel_state — Return pointer to the global kernel state.
 *
 * This function is called by virtually every kernel subsystem to
 * access the single source of truth for kernel status.
 */
flux_kernel_state_t *flux_kernel_state(void)
{
    return &s_kernel_state;
}

/*
 * flux_kernel_info — Return a formatted string with system information.
 *
 * The returned pointer points to a static buffer that is overwritten
 * on each call. Callers should copy the string if they need to preserve it.
 *
 * Information displayed:
 *   - FLUX OS version
 *   - Architecture
 *   - HAL version
 *   - Memory (total, used, free)
 *   - Active processes / agents
 *   - VM status
 *   - Compiler status
 *   - Scheduler stats
 *   - Syscall count
 *   - Compile count
 *   - Uptime (tick count)
 */
const char *flux_kernel_info(void)
{
    /* Static buffer for the formatted info string */
    static char info_buf[2048];
    int pos = 0;

    char size_buf[32];
    char cap_buf[256];

    /* ===== HEADER ===== */
    pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
        "╔══════════════════════════════════════════════════════════════╗\r\n"
        "║                    FLUX OS Kernel Info                      ║\r\n"
        "╚══════════════════════════════════════════════════════════════╝\r\n\r\n");

    /* ===== VERSION ===== */
    pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
        "  Version:     %s (%s)\r\n"
        "  Kernel:      %s\r\n"
        "  Arch Flags:  %s\r\n"
        "  Compiler:    %s\r\n",
        FLUX_OS_VERSION_STRING,
        info_compiler_info(),
        FLUX_KERNEL_NAME,
        FLUX_KERNEL_ARCH_FLAGS,
        info_compiler_info());

    /* ===== HARDWARE ===== */
    pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
        "\r\n  ── Hardware ──────────────────────────────────────\r\n"
        "  Architecture: %s\r\n"
        "  HAL Version:  %s\r\n"
        "  CPU Cores:    %u\r\n",
        s_kernel_state.hw.arch[0] ? s_kernel_state.hw.arch : "N/A",
        s_kernel_state.hw.hal_version[0] ? s_kernel_state.hw.hal_version : "N/A",
        s_kernel_state.hw.num_cores);

    /* ===== MEMORY ===== */
    info_format_size(s_kernel_state.total_memory, size_buf, sizeof(size_buf));
    pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
        "\r\n  ── Memory ────────────────────────────────────────\r\n"
        "  Total:   %s\r\n", size_buf);

    info_format_size(s_kernel_state.free_memory, size_buf, sizeof(size_buf));
    pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
        "  Free:    %s\r\n", size_buf);

    flux_size_t used = s_kernel_state.total_memory - s_kernel_state.free_memory;
    info_format_size(used, size_buf, sizeof(size_buf));
    pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
        "  Used:    %s\r\n", size_buf);

    /* ===== PROCESSES ===== */
    extern uint32_t flux_proc_count(void);
    extern int flux_agent_count(void);
    uint32_t procs = flux_proc_count();
    uint32_t agents = (uint32_t)flux_agent_count();

    pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
        "\r\n  ── Processes ─────────────────────────────────────\r\n"
        "  Active:      %u / 256\r\n"
        "  Agents:      %u\r\n"
        "  Current PID: %u\r\n",
        procs,
        agents,
        s_kernel_state.current_pid);

    /* ===== SUBSYSTEM STATUS ===== */
    pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
        "\r\n  ── Subsystem Status ──────────────────────────────\r\n"
        "  HAL:          %s\r\n"
        "  VM:           %s\r\n"
        "  Compiler:     %s\r\n"
        "  Agent Runtime: %s\r\n",
        s_kernel_state.hal_ready ? "READY" : "NOT READY",
        s_kernel_state.vm_ready ? "READY" : "NOT READY",
        s_kernel_state.compiler_ready ? "READY" : "NOT READY",
        s_kernel_state.agent_ready ? "READY" : "NOT READY");

    /* ===== COUNTERS ===== */
    pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
        "\r\n  ── Counters ──────────────────────────────────────\r\n"
        "  Ticks:         %u\r\n"
        "  Syscalls:      %u\r\n"
        "  Compilations:  %u\r\n",
        s_kernel_state.tick_count,
        s_kernel_state.syscall_count,
        s_kernel_state.compile_count);

    /* ===== KERNEL CAPABILITIES ===== */
    flux_pcb_t *kernel_pcb = flux_get_pcb(FLUX_PID_KERNEL);
    if (kernel_pcb) {
        info_capability_string(kernel_pcb->capabilities, cap_buf, sizeof(cap_buf));
        pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
            "\r\n  ── Kernel Capabilities ───────────────────────────\r\n"
            "  %s\r\n", cap_buf);
    }

    /* ===== MEMORY MAP SUMMARY ===== */
    pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
        "\r\n  ── Memory Stats ──────────────────────────────────\r\n"
        "  %s\r\n",
        /* Use the mem stats function if available */
        "  (see flux_mem_stats() for details)");

    /* ===== FOOTER ===== */
    pos += snprintf(info_buf + pos, sizeof(info_buf) - pos,
        "\r\n  ══════════════════════════════════════════════════════\r\n");

    return info_buf;
}

/*
 * flux_kernel_state_str — Compact one-line state summary.
 * Useful for quick status checks.
 */
const char *flux_kernel_state_str(void)
{
    static char buf[256];

    snprintf(buf, sizeof(buf),
             "FLUX v%s pid=%u procs=%u ticks=%u syscalls=%u "
             "hal=%s vm=%s compiler=%s agents=%s",
             FLUX_OS_VERSION_STRING,
             s_kernel_state.current_pid,
             s_kernel_state.num_processes,
             s_kernel_state.tick_count,
             s_kernel_state.syscall_count,
             s_kernel_state.hal_ready ? "ok" : "no",
             s_kernel_state.vm_ready ? "ok" : "no",
             s_kernel_state.compiler_ready ? "ok" : "no",
             s_kernel_state.agent_ready ? "ok" : "no");

    return buf;
}

/*
 * flux_kernel_uptime — Return the kernel uptime in ticks.
 */
flux_ticks_t flux_kernel_uptime(void)
{
    return s_kernel_state.tick_count;
}

/*
 * flux_kernel_update_memory — Update the memory tracking in kernel state.
 * Called by the memory manager after allocations/frees.
 */
void flux_kernel_update_memory(flux_size_t total, flux_size_t free_mem)
{
    s_kernel_state.total_memory = total;
    s_kernel_state.free_memory = free_mem;
}
