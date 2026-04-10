/*
 * FLUX OS — HAL Core Implementation
 *
 * This file implements the central HAL dispatch layer. All kernel access
 * to hardware goes through a single active backend (flux_hal_t pointer).
 * The boot sequence selects and initializes the appropriate backend based
 * on the detected architecture.
 *
 * Backend Selection Priority:
 *   1. Explicit registration (e.g., flux_hal_register_x86_64())
 *   2. Architecture detection at boot time
 *   3. Fallback to native/hosted mode
 *
 * Copyright (c) 2025 SuperInstance
 */

#include "flux/hal.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ========================================================================
 * Active HAL Backend
 * ======================================================================== */

/* The single active HAL backend — all hardware access goes through this */
static const flux_hal_t *s_active_hal = NULL;

/* Track the current initialization level for the boot sequence */
static flux_hal_level_t s_init_level = FLUX_HAL_NONE;

/* Boot log buffer for diagnostics */
#define FLUX_BOOT_LOG_SIZE 4096
static char s_boot_log[FLUX_BOOT_LOG_SIZE];
static int  s_boot_log_pos = 0;

/* ========================================================================
 * Boot Logging (internal)
 * ======================================================================== */

static void boot_log(const char *fmt, ...)
{
    if (s_boot_log_pos >= FLUX_BOOT_LOG_SIZE - 1)
        return;

    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(s_boot_log + s_boot_log_pos,
                            FLUX_BOOT_LOG_SIZE - s_boot_log_pos, fmt, ap);
    va_end(ap);

    if (written > 0)
        s_boot_log_pos += written;
}

/* ========================================================================
 * HAL Core API
 * ======================================================================== */

/**
 * flux_hal_set — Set the active HAL backend.
 *
 * This is called during boot by architecture-specific code, or by
 * flux_hal_register_*() functions.  The HAL pointer must remain valid
 * for the lifetime of the kernel (typically a static allocation).
 */
void flux_hal_set(const flux_hal_t *hal)
{
    s_active_hal = hal;
    boot_log("[HAL] Backend set to %s\n",
             hal && hal->arch_name ? hal->arch_name() : "(null)");
}

/**
 * flux_hal_get — Return the currently active HAL backend.
 *
 * Returns NULL if no backend has been registered yet.  Callers should
 * check for NULL before dereferencing.
 */
const flux_hal_t *flux_hal_get(void)
{
    return s_active_hal;
}

/**
 * flux_hal_detect_arch — Convenience: detect the current architecture.
 *
 * Delegates to the active backend's detect_arch() method.  Returns
 * FLUX_ARCH_UNKNOWN if no HAL is active.
 */
flux_arch_t flux_hal_detect_arch(void)
{
    if (s_active_hal && s_active_hal->detect_arch)
        return s_active_hal->detect_arch();
    return FLUX_ARCH_UNKNOWN;
}

/**
 * flux_hal_arch_name — Convenience: human-readable architecture name.
 */
const char *flux_hal_arch_name(void)
{
    if (s_active_hal && s_active_hal->arch_name)
        return s_active_hal->arch_name();
    return "unknown";
}

/**
 * flux_hal_console_init — Convenience: initialize the console.
 */
void flux_hal_console_init(void)
{
    if (s_active_hal && s_active_hal->console_init)
        s_active_hal->console_init();
}

/**
 * flux_hal_putc — Convenience: write one character to the console.
 */
void flux_hal_putc(char c)
{
    if (s_active_hal && s_active_hal->console_putc)
        s_active_hal->console_putc(c);
}

/**
 * flux_hal_puts — Convenience: write a null-terminated string.
 */
void flux_hal_puts(const char *s)
{
    if (s && s_active_hal && s_active_hal->console_puts)
        s_active_hal->console_puts(s);
}

/**
 * flux_hal_clear — Convenience: clear the console screen.
 */
void flux_hal_clear(void)
{
    if (s_active_hal && s_active_hal->console_clear)
        s_active_hal->console_clear();
}

/* ========================================================================
 * Boot Sequence
 *
 * The boot sequence follows a strict order:
 *   1. Detect architecture
 *   2. Register the appropriate backend
 *   3. Initialize console (so we can print boot messages)
 *   4. Initialize CPU (feature detection)
 *   5. Initialize virtual memory
 *   6. Initialize interrupts
 *   7. Initialize timer
 *   8. Report boot complete
 * ======================================================================== */

flux_hal_level_t flux_hal_boot(void)
{
    s_init_level = FLUX_HAL_NONE;
    s_boot_log_pos = 0;
    boot_log("[HAL] FLUX OS HAL boot sequence starting\n");
    boot_log("[HAL] Version: %s\n", FLUX_OS_VERSION_STRING);

    /* Step 1: Detect architecture */
    boot_log("[HAL] Step 1: Detecting architecture...\n");

#if defined(__x86_64__) || defined(_M_X64)
    boot_log("[HAL]   Compile-time: x86_64\n");
    flux_hal_register_x86_64();
#elif defined(__aarch64__) || defined(_M_ARM64)
    boot_log("[HAL]   Compile-time: ARM64\n");
    flux_hal_register_arm64();
#elif defined(__riscv) && (__riscv_xlen == 64)
    boot_log("[HAL]   Compile-time: RISC-V 64\n");
    flux_hal_register_riscv64();
#else
    boot_log("[HAL]   Compile-time: native/hosted\n");
    flux_hal_register_native();
#endif

    if (!s_active_hal) {
        boot_log("[HAL] ERROR: No backend registered!\n");
        return FLUX_HAL_NONE;
    }

    s_init_level = FLUX_HAL_PROBE;
    boot_log("[HAL]   Backend: %s\n", flux_hal_arch_name());

    /* Step 2: Initialize console */
    boot_log("[HAL] Step 2: Initializing console...\n");
    flux_hal_console_init();
    s_init_level = FLUX_HAL_READY;
    boot_log("[HAL]   Console ready.\n");

    /* Print boot log to console now that it's available */
    if (s_active_hal && s_active_hal->console_puts) {
        s_active_hal->console_puts(s_boot_log);
    }

    /* Step 3: Initialize CPU (feature detection) */
    flux_hal_puts("[HAL] Step 3: Initializing CPU...\n");
    if (s_active_hal->cpu_init) {
        s_active_hal->cpu_init();
        flux_hal_puts("[HAL]   CPU initialized.\n");
    }

    /* Step 4: Initialize virtual memory */
    flux_hal_puts("[HAL] Step 4: Initializing virtual memory...\n");
    if (s_active_hal->vm_init) {
        flux_status_t vm_status = s_active_hal->vm_init();
        if (vm_status == FLUX_OK) {
            flux_hal_puts("[HAL]   Virtual memory initialized.\n");
        } else {
            flux_hal_puts("[HAL]   VM init returned error (may be expected in hosted mode).\n");
        }
    }

    /* Step 5: Initialize interrupts */
    flux_hal_puts("[HAL] Step 5: Initializing interrupts...\n");
    if (s_active_hal->irq_init) {
        s_active_hal->irq_init();
        flux_hal_puts("[HAL]   Interrupts initialized.\n");
    }

    /* Step 6: Initialize timer */
    flux_hal_puts("[HAL] Step 6: Initializing timer...\n");
    if (s_active_hal->timer_init) {
        s_active_hal->timer_init(1000); /* Default 1000 Hz tick rate */
        flux_hal_puts("[HAL]   Timer initialized (1000 Hz).\n");
    }

    /* Step 7: Print hardware info */
    flux_hal_puts("[HAL] Step 7: Hardware info:\n");
    {
        char hw_buf[512];
        if (s_active_hal->hw_info_dump) {
            flux_status_t status = s_active_hal->hw_info_dump(hw_buf, sizeof(hw_buf));
            if (status == FLUX_OK) {
                flux_hal_puts(hw_buf);
            }
        }
    }

    /* Boot complete */
    s_init_level = FLUX_HAL_FULL;
    flux_hal_puts("[HAL] Boot sequence complete. All systems operational.\n");
    flux_hal_puts("[HAL] ─────────────────────────────────────────\n");

    return s_init_level;
}

/* ========================================================================
 * Memory Convenience Wrappers
 * ======================================================================== */

flux_size_t flux_hal_mem_total(void)
{
    if (s_active_hal && s_active_hal->mem_total)
        return s_active_hal->mem_total();
    return 0;
}

flux_addr_t flux_hal_alloc_phys(flux_size_t size, flux_size_t align)
{
    if (s_active_hal && s_active_hal->mem_alloc_phys)
        return s_active_hal->mem_alloc_phys(size, align);
    return 0;
}

/* ========================================================================
 * Backend Registration Declarations
 * ======================================================================== */

/*
 * Each architecture-specific registration function creates a static
 * flux_hal_t, populates all function pointers, and sets it as active
 * via flux_hal_set().  The implementations live in:
 *
 *   hal/arch/native/hal_native.c
 *   hal/arch/x86_64/hal_x86_64.c
 *   hal/arch/arm64/hal_arm64.c
 *   hal/arch/riscv64/hal_riscv64.c
 */

/* Forward declarations — defined in each arch backend file */
extern void flux_hal_register_native_impl(void);
extern void flux_hal_register_x86_64_impl(void);
extern void flux_hal_register_arm64_impl(void);
extern void flux_hal_register_riscv64_impl(void);

/**
 * flux_hal_register_native — Register the native (hosted-mode) backend.
 *
 * The native backend uses stdio for console, host malloc for memory,
 * and POSIX timers.  This is the default for development and testing.
 */
void flux_hal_register_native(void)
{
    flux_hal_register_native_impl();
}

/**
 * flux_hal_register_x86_64 — Register the bare-metal x86_64 backend.
 *
 * This backend provides real port I/O, VGA text mode, PIT/APIC timer,
 * PIC/APIC interrupt controller, and x86_64 page table management.
 */
void flux_hal_register_x86_64(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    flux_hal_register_x86_64_impl();
#else
    /* On non-x86 hosts, fall back to native with a warning */
    boot_log("[HAL] WARNING: x86_64 backend requested on non-x86 host, using native\n");
    flux_hal_register_native_impl();
#endif
}

/**
 * flux_hal_register_arm64 — Register the bare-metal ARM64 backend.
 *
 * This backend provides PL011 UART, GIC, generic timer, ARMv8 MMU.
 */
void flux_hal_register_arm64(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    flux_hal_register_arm64_impl();
#else
    boot_log("[HAL] WARNING: ARM64 backend requested on non-ARM host, using native\n");
    flux_hal_register_native_impl();
#endif
}

/**
 * flux_hal_register_riscv64 — Register the bare-metal RISC-V backend.
 *
 * This backend provides NS16550A UART, CLINT/PLIC, SBI interface.
 */
void flux_hal_register_riscv64(void)
{
#if defined(__riscv) && (__riscv_xlen == 64)
    flux_hal_register_riscv64_impl();
#else
    boot_log("[HAL] WARNING: RISC-V backend requested on non-RISC-V host, using native\n");
    flux_hal_register_native_impl();
#endif
}

/* ========================================================================
 * HAL Init Level Query
 * ======================================================================== */

flux_hal_level_t flux_hal_current_level(void)
{
    return s_init_level;
}

const char *flux_hal_boot_log(void)
{
    return s_boot_log;
}
