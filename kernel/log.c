/*
 * FLUX OS — Kernel Logging Subsystem
 *
 * Provides color-coded, leveled logging with a ring buffer history.
 * All kernel output goes through this module, which wraps the HAL
 * console interface. In hosted mode (Linux/POSIX), ANSI escape codes
 * provide color. On bare metal, the HAL console_set_color() is used.
 *
 * Features:
 *   - Four log levels: ERROR, WARN, INFO, DEBUG
 *   - Tick-count timestamp prefix
 *   - Color-coded output (ANSI on hosted, HAL on bare metal)
 *   - Ring buffer of last 1024 entries for post-mortem analysis
 *   - Compile-time debug gate via FLUX_DEBUG
 *   - Optional source file:line annotation
 */

#include "flux/kernel.h"
#include "flux/hal.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================
 * Configuration
 * ======================================================================== */

#ifndef FLUX_LOG_RING_SIZE
#define FLUX_LOG_RING_SIZE     1024   /* entries in ring buffer */
#endif

#ifndef FLUX_LOG_MAX_MSG
#define FLUX_LOG_MAX_MSG       512    /* max message length per entry */
#endif

#ifndef FLUX_LOG_BUF_SIZE
#define FLUX_LOG_BUF_SIZE      1024   /* internal formatting buffer */
#endif

/* Minimum log level — DEBUG messages are compiled out unless FLUX_DEBUG=1 */
#ifndef FLUX_DEBUG
#define FLUX_DEBUG             0
#endif

/* ========================================================================
 * ANSI Color Codes (hosted mode)
 *
 * In bare-metal mode, the HAL provides console_set_color() which takes
 * VGA-style color indices. We define ANSI sequences for hosted builds.
 * ======================================================================== */

#define ANSI_RESET     "\033[0m"
#define ANSI_RED       "\033[31m"
#define ANSI_YELLOW    "\033[33m"
#define ANSI_GREEN     "\033[32m"
#define ANSI_CYAN      "\033[36m"
#define ANSI_GRAY      "\033[90m"
#define ANSI_BOLD      "\033[1m"
#define ANSI_DIM       "\033[2m"

/* VGA color indices for bare-metal HAL */
#define VGA_COLOR_WHITE    0x0F
#define VGA_COLOR_RED      0x0C
#define VGA_COLOR_YELLOW   0x0E
#define VGA_COLOR_GREEN    0x0A
#define VGA_COLOR_CYAN     0x0B
#define VGA_COLOR_GRAY     0x08

/* ========================================================================
 * Log Level Definitions
 * ======================================================================== */

typedef enum {
    LOG_LEVEL_ERROR = 0,   /* Critical failures */
    LOG_LEVEL_WARN  = 1,   /* Warning conditions */
    LOG_LEVEL_INFO  = 2,   /* Informational messages */
    LOG_LEVEL_DEBUG = 3,   /* Debug-only (compiled out if !FLUX_DEBUG) */
} log_level_t;

static const char *level_names[] = {
    "ERROR",
    "WARN ",
    "INFO ",
    "DEBUG",
};

static const char *level_ansi[] = {
    ANSI_RED,         /* ERROR */
    ANSI_YELLOW,      /* WARN  */
    ANSI_GREEN,       /* INFO  */
    ANSI_CYAN,        /* DEBUG */
};

static uint8_t level_vga_fg[] = {
    VGA_COLOR_RED,    /* ERROR */
    VGA_COLOR_YELLOW, /* WARN  */
    VGA_COLOR_GREEN,  /* INFO  */
    VGA_COLOR_CYAN,   /* DEBUG */
};

/* ========================================================================
 * Log Entry (Ring Buffer Item)
 * ======================================================================== */

typedef struct {
    uint32_t    tick;           /* Timestamp (kernel tick count) */
    log_level_t level;          /* Log level */
    uint32_t    sequence;       /* Monotonically increasing sequence number */
    char        file[32];       /* Source file name (optional) */
    int         line;           /* Source line number (optional) */
    char        message[FLUX_LOG_MAX_MSG]; /* Formatted message */
} log_entry_t;

/* ========================================================================
 * Static State
 * ======================================================================== */

/* Ring buffer */
static log_entry_t  s_log_ring[FLUX_LOG_RING_SIZE];
static uint32_t     s_log_head = 0;    /* Next write position */
static uint32_t     s_log_count = 0;   /* Total entries written (for seq#) */
static uint32_t     s_log_dropped = 0; /* Number of dropped messages */

/* Minimum log level filter */
static volatile log_level_t s_log_min_level =
#if FLUX_DEBUG
    LOG_LEVEL_DEBUG;
#else
    LOG_LEVEL_INFO;
#endif

/* Whether to use ANSI colors (hosted mode) vs HAL colors (bare metal) */
static bool s_log_ansi_enabled = true;

/* Internal formatting buffer (static to avoid heap) */
static char s_log_buf[FLUX_LOG_BUF_SIZE];

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/*
 * flux_log_itoa — Minimal integer-to-string conversion.
 * Avoids stdio dependency. Writes into caller-provided buffer.
 * Returns pointer to the start of the number string.
 */
static char *flux_log_itoa(uint64_t value, char *buf, int base)
{
    static const char digits[] = "0123456789ABCDEF";
    char tmp[24];
    int i = 0;

    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }

    while (value > 0 && i < 24) {
        tmp[i++] = digits[value % base];
        value /= base;
    }

    /* Reverse into output buffer */
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
    return buf;
}

/*
 * flux_log_puts — Write a string to the console.
 * Uses HAL if available, falls back to stdio for hosted mode.
 */
static void flux_log_puts(const char *s)
{
    const flux_hal_t *hal = flux_hal_get();
    if (hal && hal->console_puts) {
        hal->console_puts(s);
    } else {
        /* Hosted fallback — use stdio */
        fputs(s, stderr);
    }
}

/*
 * flux_log_set_color — Set console text color.
 * Uses ANSI codes in hosted mode, HAL in bare-metal mode.
 */
static void flux_log_set_color(uint8_t ansi_level)
{
    if (s_log_ansi_enabled) {
        /* In hosted mode, the ANSI codes are embedded in format strings */
        /* This function is a no-op; colors come from the prefix */
        (void)ansi_level;
    } else {
        const flux_hal_t *hal = flux_hal_get();
        if (hal && hal->console_set_color) {
            hal->console_set_color(level_vga_fg[ansi_level], 0);
        }
    }
}

/*
 * flux_log_reset_color — Reset console to default color.
 */
static void flux_log_reset_color(void)
{
    if (s_log_ansi_enabled) {
        /* ANSI reset is embedded in format strings */
    } else {
        const flux_hal_t *hal = flux_hal_get();
        if (hal && hal->console_set_color) {
            hal->console_set_color(VGA_COLOR_WHITE, 0);
        }
    }
}

/*
 * flux_log_record — Add an entry to the ring buffer.
 * This stores the message for later retrieval, even if the console
 * is not yet initialized (early boot messages are preserved).
 */
static void flux_log_record(log_level_t level, uint32_t tick,
                            const char *file, int line,
                            const char *message)
{
    log_entry_t *entry = &s_log_ring[s_log_head];

    entry->tick = tick;
    entry->level = level;
    entry->sequence = s_log_count++;
    entry->line = line;

    /* Store file name (just the basename) */
    if (file) {
        const char *base = file;
        const char *p;
        for (p = file; *p; p++) {
            if (*p == '/' || *p == '\\')
                base = p + 1;
        }
        /* Truncate to fit */
        int i;
        for (i = 0; i < 31 && base[i]; i++)
            entry->file[i] = base[i];
        entry->file[i] = '\0';
    } else {
        entry->file[0] = '\0';
    }

    /* Copy message, truncating if necessary */
    int i;
    for (i = 0; i < FLUX_LOG_MAX_MSG - 1 && message[i]; i++)
        entry->message[i] = message[i];
    entry->message[i] = '\0';

    /* Advance ring buffer head */
    s_log_head = (s_log_head + 1) % FLUX_LOG_RING_SIZE;
}

/*
 * flux_log_format — Core logging routine.
 * Formats the message with timestamp, level prefix, and color codes,
 * outputs to console, and records in ring buffer.
 */
static void flux_log_format(log_level_t level, const char *file, int line,
                            const char *fmt, va_list ap)
{
    /* Check level filter */
    if ((int)level > (int)s_log_min_level)
        return;

    /* Get current tick count */
    uint32_t tick = 0;
    flux_kernel_state_t *ks = flux_kernel_state();
    if (ks)
        tick = ks->tick_count;

    /* Format the user message into s_log_buf */
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int msg_len = 0;
    if (fmt) {
        /* Minimal vsnprintf-like formatting using vsnprintf from stdio */
        msg_len = vsnprintf(s_log_buf, FLUX_LOG_BUF_SIZE - 1, fmt, ap_copy);
        if (msg_len < 0) msg_len = 0;
        if (msg_len >= FLUX_LOG_BUF_SIZE) msg_len = FLUX_LOG_BUF_SIZE - 1;
        s_log_buf[msg_len] = '\0';
    } else {
        s_log_buf[0] = '\0';
        msg_len = 0;
    }
    va_end(ap_copy);

    /* Record in ring buffer (always, even if console not ready) */
    flux_log_record(level, tick, file, line, s_log_buf);

    /* Output to console */
    char prefix_buf[128];
    char tick_str[20];
    flux_log_itoa(tick, tick_str, 10);

    if (s_log_ansi_enabled) {
        /* ANSI color-coded output: [TICK] LEVEL: message */
        snprintf(prefix_buf, sizeof(prefix_buf),
                 "%s[%s] %s%s:%s %s",
                 ANSI_DIM, tick_str,
                 level_ansi[level], level_names[level], ANSI_RESET,
                 ANSI_BOLD);
        flux_log_puts(prefix_buf);
        flux_log_puts(s_log_buf);
        flux_log_puts(ANSI_RESET);
        flux_log_puts("\r\n");
    } else {
        /* Bare-metal: use HAL color switching */
        flux_log_set_color((uint8_t)level);
        snprintf(prefix_buf, sizeof(prefix_buf),
                 "[%s] %s: ", tick_str, level_names[level]);
        flux_log_puts(prefix_buf);
        flux_log_puts(s_log_buf);
        flux_log_puts("\r\n");
        flux_log_reset_color();
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

/*
 * flux_log — Main kernel log function (INFO level).
 * All kernel subsystems use this for informational output.
 */
void flux_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    flux_log_format(LOG_LEVEL_INFO, NULL, 0, fmt, ap);
    va_end(ap);
}

/*
 * flux_log_debug — Debug-only log function.
 * Compiled to a no-op unless FLUX_DEBUG=1 is defined.
 */
#if FLUX_DEBUG
void flux_log_debug(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    flux_log_format(LOG_LEVEL_DEBUG, NULL, 0, fmt, ap);
    va_end(ap);
}
#else
void flux_log_debug(const char *fmt, ...)
{
    /* Completely compiled out — zero overhead */
    (void)fmt;
}
#endif

/*
 * flux_log_error — Log an ERROR level message.
 * Used for critical failures that don't necessarily halt the system.
 */
void flux_log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    flux_log_format(LOG_LEVEL_ERROR, NULL, 0, fmt, ap);
    va_end(ap);
}

/*
 * flux_log_warn — Log a WARNING level message.
 * Used for recoverable issues and degraded operation.
 */
void flux_log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    flux_log_format(LOG_LEVEL_WARN, NULL, 0, fmt, ap);
    va_end(ap);
}

/*
 * flux_log_source — Log with source file:line annotation.
 * Useful for tracing through kernel code. Intended for debug builds.
 */
void flux_log_source(log_level_t level, const char *file, int line,
                     const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    flux_log_format(level, file, line, fmt, ap);
    va_end(ap);
}

/*
 * flux_log_dump_ring — Dump the ring buffer contents for post-mortem.
 * Outputs the most recent N entries (or all if N >= ring size).
 */
void flux_log_dump_ring(int count)
{
    flux_log_puts(ANSI_BOLD "=== FLUX Kernel Log Ring Buffer ===" ANSI_RESET "\r\n");

    if (count <= 0 || count > FLUX_LOG_RING_SIZE)
        count = FLUX_LOG_RING_SIZE;

    /* Calculate starting position in the ring */
    uint32_t start;
    if (s_log_count < FLUX_LOG_RING_SIZE) {
        start = 0;
        count = (int)s_log_count;
    } else {
        start = s_log_head; /* s_log_head points to oldest entry when full */
    }

    for (int i = 0; i < count; i++) {
        uint32_t idx = (start + i) % FLUX_LOG_RING_SIZE;
        log_entry_t *e = &s_log_ring[idx];

        char tick_str[20];
        flux_log_itoa(e->tick, tick_str, 10);
        char seq_str[20];
        flux_log_itoa(e->sequence, seq_str, 10);

        if (s_log_ansi_enabled) {
            flux_log_puts(level_ansi[e->level]);
        }

        char line_buf[512];
        int written = snprintf(line_buf, sizeof(line_buf),
                 "  [%s] #%s %s: %s%s%s\r\n",
                 tick_str, seq_str, level_names[e->level],
                 e->file[0] ? e->file : "",
                 e->file[0] ? ": " : "",
                 e->message);
        /* Truncate message if it overflows — just print the first portion */
        (void)written;
        flux_log_puts(line_buf);

        if (s_log_ansi_enabled) {
            flux_log_puts(ANSI_RESET);
        }
    }

    flux_log_puts(ANSI_DIM "=== End of Log ===" ANSI_RESET "\r\n");
}

/*
 * flux_log_set_level — Set the minimum log level filter.
 * Messages below this level are silently discarded.
 */
void flux_log_set_level(int level)
{
    if (level >= LOG_LEVEL_ERROR && level <= LOG_LEVEL_DEBUG)
        s_log_min_level = (log_level_t)level;
}

/*
 * flux_log_get_level — Get the current minimum log level.
 */
int flux_log_get_level(void)
{
    return (int)s_log_min_level;
}

/*
 * flux_log_get_entry — Retrieve a specific entry from the ring buffer.
 * index 0 = most recent entry.
 * Returns FLUX_OK on success, FLUX_ERR_INVALID if index out of range.
 */
flux_status_t flux_log_get_entry(int index, log_entry_t *out)
{
    if (!out || index < 0)
        return FLUX_ERR_INVALID;

    if ((uint32_t)index >= s_log_count)
        return FLUX_ERR_INVALID;

    /* Convert from "0 = most recent" to ring buffer index */
    uint32_t actual;
    if (s_log_count < FLUX_LOG_RING_SIZE) {
        actual = (uint32_t)index;
    } else {
        actual = (s_log_head + FLUX_LOG_RING_SIZE - 1 - (uint32_t)index) % FLUX_LOG_RING_SIZE;
    }

    *out = s_log_ring[actual];
    return FLUX_OK;
}

/*
 * flux_log_entry_count — Total number of log entries written since boot.
 */
uint32_t flux_log_entry_count(void)
{
    return s_log_count;
}

/*
 * flux_log_init — Initialize the logging subsystem.
 * Detects whether to use ANSI or HAL colors based on HAL state.
 */
void flux_log_init(void)
{
    const flux_hal_t *hal = flux_hal_get();

    /* If HAL is available and we're on bare metal, disable ANSI */
    if (hal && hal->console_set_color) {
        s_log_ansi_enabled = false;
    }

    /* Clear ring buffer */
    for (uint32_t i = 0; i < FLUX_LOG_RING_SIZE; i++) {
        s_log_ring[i].tick = 0;
        s_log_ring[i].level = LOG_LEVEL_INFO;
        s_log_ring[i].sequence = 0;
        s_log_ring[i].line = 0;
        s_log_ring[i].file[0] = '\0';
        s_log_ring[i].message[0] = '\0';
    }

    s_log_head = 0;
    s_log_count = 0;
    s_log_dropped = 0;

    flux_log("kernel log subsystem initialized (ring=%d entries)", FLUX_LOG_RING_SIZE);
}

/*
 * flux_log_shutdown — Clean up the logging subsystem.
 */
void flux_log_shutdown(void)
{
    flux_log("kernel log subsystem shutting down (total entries: %u, dropped: %u)",
             s_log_count, s_log_dropped);
}
