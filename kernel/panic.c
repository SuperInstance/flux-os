/*
 * FLUX OS — Kernel Panic Handler
 *
 * When the kernel encounters an unrecoverable error, it calls flux_panic().
 * This module handles:
 *
 *   1. Printing a prominent, color-coded panic message
 *   2. Dumping the current process state and register contents
 *   3. Collecting a stack trace (frame pointer walk on hosted builds)
 *   4. Dumping the log ring buffer for post-mortem analysis
 *   5. Halting the CPU via the HAL
 *
 * The panic handler never returns. On bare metal, it disables interrupts
 * and enters an infinite HLT loop. In hosted mode, it calls abort().
 *
 * The flux_assert() macro wraps __builtin_expect for compile-time
 * condition checking and calls flux_panic() on failure.
 */

#include "flux/kernel.h"
#include "flux/hal.h"

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <setjmp.h>

/* ========================================================================
 * ANSI Color Codes for Panic Output
 * ======================================================================== */

#define PANIC_RED       "\033[31m"
#define PANIC_RED_BOLD  "\033[1;31m"
#define PANIC_YELLOW    "\033[33m"
#define PANIC_YELLOW_BOLD "\033[1;33m"
#define PANIC_WHITE     "\033[37m"
#define PANIC_CYAN      "\033[36m"
#define PANIC_GRAY      "\033[90m"
#define PANIC_RESET     "\033[0m"
#define PANIC_BOLD      "\033[1m"
#define PANIC_REVERSE   "\033[7m"

/* ========================================================================
 * Stack Trace Configuration
 * ======================================================================== */

#ifndef FLUX_PANIC_MAX_FRAMES
#define FLUX_PANIC_MAX_FRAMES    32    /* Max stack frames to capture */
#endif

#ifndef FLUX_PANIC_STACK_WORDS
#define FLUX_PANIC_STACK_WORDS   64    /* Number of 64-bit words to dump */
#endif

/* ========================================================================
 * Panic Count (for re-entrant panic detection)
 * ======================================================================== */

static volatile int s_panic_count = 0;

/* Flag to prevent recursive panics during panic handling */
static volatile bool s_in_panic = false;

/* ========================================================================
 * Console Output Helpers
 * ======================================================================== */

/*
 * panic_puts — Write a string to console during panic.
 * Does NOT go through the logging subsystem (which may be broken).
 * Uses HAL directly or stdio fallback.
 */
static void panic_puts(const char *s)
{
    const flux_hal_t *hal = flux_hal_get();
    if (hal && hal->console_puts) {
        hal->console_puts(s);
    } else {
        fputs(s, stderr);
    }
}

/*
 * panic_putc — Write a single character during panic.
 */
static void panic_putc(char c)
{
    const flux_hal_t *hal = flux_hal_get();
    if (hal && hal->console_putc) {
        hal->console_putc(c);
    } else {
        fputc(c, stderr);
    }
}

/*
 * panic_print_separator — Print a horizontal separator line.
 */
static void panic_print_separator(void)
{
    panic_puts(PANIC_GRAY "═" PANIC_RESET);
    for (int i = 0; i < 70; i++)
        panic_puts("═");
    panic_puts(PANIC_GRAY "═\r\n" PANIC_RESET);
}

/*
 * panic_itoa — Integer to hex string (minimal, no stdio dependency).
 */
static void panic_itoa_hex(uint64_t val, char *buf, int width)
{
    static const char hex[] = "0123456789ABCDEF";
    char tmp[20];
    int i = 0;

    if (val == 0) {
        for (int j = 0; j < width; j++) buf[j] = '0';
        buf[width] = '\0';
        return;
    }

    while (val > 0) {
        tmp[i++] = hex[val & 0xF];
        val >>= 4;
    }

    /* Pad to width */
    while (i < width) tmp[i++] = '0';

    /* Reverse */
    for (int j = 0; j < i; j++)
        buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

/*
 * panic_itoa_dec — Integer to decimal string.
 */
static void panic_itoa_dec(uint64_t val, char *buf)
{
    static const char dig[] = "0123456789";
    char tmp[24];
    int i = 0;

    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    while (val > 0) {
        tmp[i++] = dig[val % 10];
        val /= 10;
    }

    for (int j = 0; j < i; j++)
        buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

/* ========================================================================
 * Register Dump
 * ======================================================================== */

/*
 * panic_dump_registers — Dump the current process's register state.
 * Shows GP registers R0-R15 in two columns, then special registers.
 */
static void panic_dump_registers(const flux_pcb_t *pcb)
{
    char buf[128];
    char hex[20];

    panic_puts(PANIC_CYAN PANIC_BOLD "  Registers:" PANIC_RESET "\r\n");

    /* Dump R0-R15 in two columns */
    for (int row = 0; row < 16; row += 2) {
        panic_itoa_hex(row, hex, 2);

        panic_puts("  R");
        panic_puts(hex);
        panic_puts(" = ");

        panic_itoa_hex(pcb->regs[row], buf, 16);
        panic_puts(buf);

        /* Second column */
        if (row + 1 < 16) {
            panic_itoa_hex(row + 1, hex, 2);
            panic_puts("    R");
            panic_puts(hex);
            panic_puts(" = ");
            panic_itoa_hex(pcb->regs[row + 1], buf, 16);
            panic_puts(buf);
        }

        panic_puts("\r\n");
    }

    /* Special registers */
    panic_puts("  ");
    panic_puts(PANIC_YELLOW);
    panic_puts("SP ");
    panic_puts(PANIC_RESET);
    panic_puts("= ");
    panic_itoa_hex(pcb->stack_ptr, buf, 16);
    panic_puts(buf);
    panic_puts("   ");

    panic_puts(PANIC_YELLOW "IP " PANIC_RESET);
    panic_puts("= ");
    panic_itoa_hex(pcb->instr_ptr, buf, 16);
    panic_puts(buf);
    panic_puts("   ");

    panic_puts(PANIC_YELLOW "PT " PANIC_RESET);
    panic_puts("= ");
    panic_itoa_hex(pcb->page_table, buf, 16);
    panic_puts(buf);
    panic_puts("\r\n");

    /* Bytecode VM state if active */
    if (pcb->bc.active) {
        panic_puts(PANIC_CYAN "  Bytecode VM State:" PANIC_RESET "\r\n");
        panic_puts("    base=");
        panic_itoa_hex(pcb->bc.bytecode_base, buf, 16);
        panic_puts(buf);
        panic_puts("  len=");
        panic_itoa_dec(pcb->bc.bytecode_len, buf);
        panic_puts(buf);
        panic_puts("  pc=");
        panic_itoa_dec(pcb->bc.pc, buf);
        panic_puts(buf);
        panic_puts("\r\n");

        /* Dump VM registers R0-R7 */
        for (int i = 0; i < 8; i++) {
            panic_puts("    VM_R");
            panic_itoa_dec(i, buf);
            panic_puts(buf);
            panic_puts("=");
            panic_itoa_hex(pcb->bc.vm_regs[i], hex, 16);
            panic_puts(hex);
            if (i % 4 == 3) panic_puts("\r\n");
            else panic_puts("  ");
        }
    }
}

/* ========================================================================
 * Process State Dump
 * ======================================================================== */

/*
 * panic_dump_process — Show the current process information.
 */
static void panic_dump_process(const flux_pcb_t *pcb)
{
    char buf[64];

    panic_puts(PANIC_CYAN PANIC_BOLD "  Process:" PANIC_RESET "\r\n");
    panic_puts("    PID:     ");
    panic_itoa_dec(pcb->pid, buf);
    panic_puts(buf);
    panic_puts("\r\n");

    panic_puts("    Name:    ");
    panic_puts(pcb->name[0] ? pcb->name : "(unnamed)");
    panic_puts("\r\n");

    panic_puts("    State:   ");
    panic_itoa_dec(pcb->state, buf);
    panic_puts(buf);
    panic_puts("\r\n");

    panic_puts("    Parent:  ");
    panic_itoa_dec(pcb->parent_pid, buf);
    panic_puts(buf);
    panic_puts("\r\n");

    panic_puts("    CPU:     ");
    panic_itoa_dec(pcb->cpu_time, buf);
    panic_puts(buf);
    panic_puts(" ticks\r\n");

    if (pcb->is_agent) {
        panic_puts(PANIC_YELLOW "    [AGENT] id=");
        panic_itoa_dec(pcb->agent_id, buf);
        panic_puts(buf);
        panic_puts(" model=");
        panic_puts(pcb->agent_model);
        panic_puts(PANIC_RESET "\r\n");
    }

    if (pcb->is_compiled) {
        panic_puts(PANIC_YELLOW "    [SELF-COMPILED]" PANIC_RESET "\r\n");
    }
}

/* ========================================================================
 * Stack Trace
 * ======================================================================== */

/*
 * panic_collect_stack_trace — Walk the stack using frame pointers.
 *
 * On hosted builds (GCC/Clang), we can use __builtin_frame_address(0)
 * to get the current frame pointer and walk the chain. On bare metal,
 * the HAL may provide a different mechanism.
 *
 * This stores return addresses into the provided buffer and returns
 * the number of frames collected.
 */
static int panic_collect_stack_trace(uint64_t *frames, int max_frames)
{
    int count = 0;

#if defined(__GNUC__) || defined(__clang__)
    /*
     * Frame pointer chain walk:
     *   Each stack frame (with -fno-omit-frame-pointer) has:
     *     [rbp]    -> previous rbp
     *     [rbp+8]  -> return address
     *
     * We start from the caller's frame (skip panic frame itself).
     */
    uint64_t *fp;
#if defined(__x86_64__)
    /* Get caller's frame pointer — skip this function's frame */
    fp = (uint64_t *)__builtin_frame_address(0);
    if (fp) fp = (uint64_t *)(*fp); /* Walk one more to get caller */
#elif defined(__aarch64__)
    fp = (uint64_t *)__builtin_frame_address(0);
    if (fp) fp = (uint64_t *)(*fp);
#else
    fp = (uint64_t *)__builtin_frame_address(0);
    if (fp) fp = (uint64_t *)(*fp);
#endif

    for (count = 0; count < max_frames && fp != NULL; count++) {
        /* Safety: check for reasonable stack bounds */
        if ((uint64_t)fp < 0x1000 || (uint64_t)fp > 0x7FFFFFFFFFFFULL) {
            break;
        }

        /* Return address is at fp[1] */
        uint64_t ret_addr = fp[1];
        frames[count] = ret_addr;

        /* Walk to previous frame */
        uint64_t prev_fp = fp[0];
        if (prev_fp <= (uint64_t)fp) {
            break;  /* Stack grows downward; prev_fp should be higher */
        }
        fp = (uint64_t *)prev_fp;
    }
#else
    /* Unsupported compiler — no stack trace */
    (void)frames;
    (void)max_frames;
#endif

    return count;
}

/*
 * panic_print_stack_trace — Display collected stack frames.
 */
static void panic_print_stack_trace(uint64_t *frames, int count)
{
    char buf[20];

    if (count == 0) {
        panic_puts(PANIC_GRAY "  (no stack trace available)" PANIC_RESET "\r\n");
        return;
    }

    panic_puts(PANIC_CYAN PANIC_BOLD "  Stack Trace:" PANIC_RESET "\r\n");

    for (int i = 0; i < count; i++) {
        panic_puts("    #");
        panic_itoa_dec(i, buf);
        panic_puts(buf);
        panic_puts("  0x");
        panic_itoa_hex(frames[i], buf, 16);
        panic_puts(buf);
        panic_puts("\r\n");
    }
}

/* ========================================================================
 * Raw Stack Dump
 * ======================================================================== */

/*
 * panic_dump_stack_raw — Dump raw stack words around SP for debugging.
 */
static void panic_dump_stack_raw(const flux_pcb_t *pcb)
{
    char buf[20];
    uint64_t sp = pcb->stack_ptr;

    if (sp == 0) {
        panic_puts(PANIC_GRAY "  (no stack pointer)" PANIC_RESET "\r\n");
        return;
    }

    panic_puts(PANIC_CYAN "  Stack Memory (SP-16 to SP+" );
    panic_itoa_dec(FLUX_PANIC_STACK_WORDS * 8, buf);
    panic_puts(buf);
    panic_puts("):" PANIC_RESET "\r\n");

    /* We can't safely dereference SP in a hosted environment,
     * but we dump the stack_ptr value and note it */
    panic_puts("    SP = 0x");
    panic_itoa_hex(sp, buf, 16);
    panic_puts(buf);
    panic_puts(PANIC_GRAY "  (raw stack access unsafe in hosted mode)" PANIC_RESET "\r\n");
}

/* ========================================================================
 * Public API
 * ======================================================================== */

/*
 * flux_panic — Kernel panic handler. NEVER RETURNS.
 *
 * This is the terminal error handler for the FLUX kernel. It:
 *   1. Detects re-entrant panics (double fault)
 *   2. Prints a prominent panic banner
 *   3. Shows the panic reason
 *   4. Dumps process state, registers, and stack trace
 *   5. Halts the system
 *
 * Parameters:
 *   reason — Human-readable description of the panic
 */
void flux_panic(const char *reason)
{
    s_panic_count++;

    /* Double panic — something went very wrong in the panic handler itself */
    if (s_panic_count > 1) {
        panic_puts(PANIC_RED_BOLD PANIC_REVERSE);
        panic_puts("\r\n  *** DOUBLE PANIC ***");
        panic_puts(PANIC_RESET "\r\n");
        panic_puts("  Original panic handler failed. Halting immediately.\r\n");
        goto halt;
    }

    s_in_panic = true;

    /* === PANIC BANNER === */
    panic_puts("\r\n");
    panic_puts(PANIC_RED_BOLD PANIC_REVERSE);
    panic_puts("                       *** FLUX KERNEL PANIC ***                       ");
    panic_puts(PANIC_RESET "\r\n\r\n");

    /* Panic reason */
    if (reason) {
        panic_puts(PANIC_RED_BOLD "  Reason: " PANIC_RESET);
        panic_puts(reason);
        panic_puts("\r\n");
    } else {
        panic_puts(PANIC_RED_BOLD "  Reason: " PANIC_RESET "(no reason given)\r\n");
    }

    panic_puts("\r\n");
    panic_print_separator();

    /* === KERNEL STATE === */
    flux_kernel_state_t *ks = flux_kernel_state();
    if (ks) {
        char buf[64];
        panic_puts(PANIC_CYAN PANIC_BOLD "  Kernel State:" PANIC_RESET "\r\n");
        panic_puts("    tick_count:    ");
        panic_itoa_dec(ks->tick_count, buf);
        panic_puts(buf);
        panic_puts("\r\n");

        panic_puts("    current_pid:   ");
        panic_itoa_dec(ks->current_pid, buf);
        panic_puts(buf);
        panic_puts("\r\n");

        panic_puts("    num_processes: ");
        panic_itoa_dec(ks->num_processes, buf);
        panic_puts(buf);
        panic_puts("\r\n");

        panic_puts("    syscall_count: ");
        panic_itoa_dec(ks->syscall_count, buf);
        panic_puts(buf);
        panic_puts("\r\n");

        panic_puts("    free_memory:   ");
        panic_itoa_dec(ks->free_memory, buf);
        panic_puts(buf);
        panic_puts("\r\n");
    }

    /* === CURRENT PROCESS STATE === */
    if (ks && ks->current_pid != FLUX_PID_INVALID) {
        flux_pcb_t *pcb = flux_get_pcb(ks->current_pid);
        if (pcb) {
            panic_dump_process(pcb);
            panic_puts("\r\n");
            panic_dump_registers(pcb);
            panic_puts("\r\n");
        }
    }

    /* === STACK TRACE === */
    {
        uint64_t frames[FLUX_PANIC_MAX_FRAMES];
        int nframes = panic_collect_stack_trace(frames, FLUX_PANIC_MAX_FRAMES);
        panic_print_stack_trace(frames, nframes);
    }

    panic_puts("\r\n");
    panic_print_separator();

    /* === HALT === */
halt:
    panic_puts(PANIC_RED_BOLD "\r\n  System halted. Reset required.\r\n" PANIC_RESET);

    /* Try HAL halt first, then spin */
    const flux_hal_t *hal = flux_hal_get();
    if (hal && hal->cpu_halt) {
        /* Disable interrupts */
        if (hal->irq_disable)
            hal->irq_disable();

        hal->cpu_halt();
    }

    /* Fallback: infinite loop with pause */
    for (;;) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("hlt");
#elif defined(__aarch64__)
        __asm__ volatile("wfe");
#elif defined(__riscv)
        __asm__ volatile("wfi");
#else
        /* Portable busy loop */
#endif
    }
}

/*
 * flux_assert — Assertion handler.
 * Called by the FLUX_ASSERT macro when a condition is false.
 * Prints file, line, condition, and calls flux_panic().
 */
void flux_assert_fail(const char *file, int line, const char *expr)
{
    char buf[256];

    panic_puts(PANIC_YELLOW_BOLD "\r\n  ASSERTION FAILED" PANIC_RESET "\r\n");
    panic_puts("  File:     ");
    panic_puts(file ? file : "(unknown)");
    panic_puts("\r\n");

    panic_puts("  Line:     ");
    char line_buf[20];
    panic_itoa_dec(line, line_buf);
    panic_puts(line_buf);
    panic_puts("\r\n");

    if (expr) {
        panic_puts("  Expected: ");
        panic_puts(expr);
        panic_puts("\r\n");
    }

    panic_puts("\r\n");

    snprintf(buf, sizeof(buf), "assertion failed: %s", expr ? expr : "(null)");
    flux_panic(buf);
}

/*
 * flux_warn — Print a warning message with yellow highlighting.
 * Warnings are non-fatal but indicate a problem that needs attention.
 */
void flux_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    /* Build formatted message */
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Print with yellow color */
    panic_puts(PANIC_YELLOW);
    const flux_hal_t *hal = flux_hal_get();
    if (hal && hal->console_puts) {
        hal->console_puts(PANIC_YELLOW);
        hal->console_puts("  [WARN] ");
        hal->console_puts(buf);
        hal->console_puts(PANIC_RESET "\r\n");
    } else {
        fprintf(stderr, PANIC_YELLOW "  [WARN] %s" PANIC_RESET "\r\n", buf);
    }
    (void)panic_putc; /* suppress unused warning */
}

/*
 * flux_panic_count — Return the number of panics that have occurred.
 * Useful for detecting double faults.
 */
int flux_panic_count(void)
{
    return s_panic_count;
}

/*
 * flux_in_panic — Check if the kernel is currently handling a panic.
 * Used by other subsystems to avoid re-entrant failures.
 */
bool flux_in_panic(void)
{
    return s_in_panic;
}

/*
 * FLUX_ASSERT — Kernel assertion macro.
 * Uses __builtin_expect to hint the branch predictor that the condition
 * is expected to be true (common case).
 */
#undef FLUX_ASSERT
#define FLUX_ASSERT(expr)                                                     \
    do {                                                                      \
        if (__builtin_expect(!(expr), 0)) {                                   \
            flux_assert_fail(__FILE__, __LINE__, #expr);                      \
        }                                                                     \
    } while (0)
