/*
 * FLUX OS — Native HAL Backend (Hosted Mode)
 *
 * Implements the HAL interface for hosted mode (Linux/macOS/POSIX).
 * This provides stdio-based console, simulated memory, and stub
 * implementations for all hardware operations.
 *
 * The native HAL allows the FLUX kernel to run as a regular process
 * on the host OS, which is essential for:
 *   - Development and testing
 *   - The self-compiler (kernel can compile itself on host)
 *   - Agent simulation
 *   - Bytecode VM testing
 */

#define _POSIX_C_SOURCE 199309L

#include "flux/hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* ========================================================================
 * Static HAL Instance
 * ======================================================================== */

static flux_hal_t s_native_hal;

/* Global HAL pointer (defined in hal.h, set by this file) */
static const flux_hal_t *s_active_hal = NULL;

/* ========================================================================
 * Console Implementation (stdio)
 * ======================================================================== */

static void native_console_init(void)
{
    /* stdio is already initialized by the C runtime */
}

static void native_console_putc(char c)
{
    fputc(c, stderr);
}

static void native_console_puts(const char *s)
{
    if (s)
        fputs(s, stderr);
}

static char native_console_getc(void)
{
    return (char)fgetc(stdin);
}

static void native_console_clear(void)
{
    /* ANSI clear screen */
    fputs("\033[2J\033[H", stderr);
}

static void native_console_set_color(uint8_t fg, uint8_t bg)
{
    /* ANSI color mapping */
    static const char *ansi_fg[] = {
        "\033[30m", "\033[34m", "\033[32m", "\033[36m",
        "\033[31m", "\033[35m", "\033[33m", "\033[37m",
        "\033[90m", "\033[94m", "\033[92m", "\033[96m",
        "\033[91m", "\033[95m", "\033[93m", "\033[97m",
    };
    (void)bg;

    if (fg < 16)
        fputs(ansi_fg[fg], stderr);
}

/* ========================================================================
 * Memory (Simulated)
 * ======================================================================== */

static flux_size_t native_mem_total(void)
{
    return 16 * 1024 * 1024;  /* 16 MB simulated */
}

static flux_size_t native_mem_free(void)
{
    /* Return a fixed value — the kernel's memory manager tracks actual usage */
    flux_kernel_state_t *ks = flux_kernel_state();
    if (ks)
        return ks->free_memory;
    return 16 * 1024 * 1024;
}

static flux_addr_t native_mem_alloc_phys(flux_size_t size, flux_size_t align)
{
    (void)size;
    (void)align;
    return 0;  /* Not used — kernel uses its own allocator */
}

static void native_mem_free_phys(flux_addr_t addr, flux_size_t size)
{
    (void)addr;
    (void)size;
}

static int native_mem_map_count(void)
{
    return 0;
}

static const flux_mem_map_t *native_mem_map_get(int index)
{
    (void)index;
    return NULL;
}

/* ========================================================================
 * Virtual Memory (Stubs)
 * ======================================================================== */

static flux_status_t native_vm_init(void)
{
    return FLUX_OK;  /* No-op in hosted mode */
}

static flux_status_t native_vm_map(flux_addr_t virt, flux_addr_t phys,
                                   flux_size_t size, flux_perm_t perm)
{
    (void)virt; (void)phys; (void)size; (void)perm;
    return FLUX_OK;
}

static flux_status_t native_vm_unmap(flux_addr_t virt, flux_size_t size)
{
    (void)virt; (void)size;
    return FLUX_OK;
}

static flux_status_t native_vm_protect(flux_addr_t virt, flux_size_t size, flux_perm_t perm)
{
    (void)virt; (void)size; (void)perm;
    return FLUX_OK;
}

static flux_addr_t native_vm_current_pml4(void) { return 0; }
static void native_vm_switch(flux_addr_t pml4) { (void)pml4; }
static void native_vm_flush_tlb(void) {}
static void native_vm_invalidate(flux_addr_t addr) { (void)addr; }

/* ========================================================================
 * CPU (Simulated)
 * ======================================================================== */

static void native_cpu_init(void) {}
static void native_cpu_halt(void) {}
static void native_cpu_pause(void) { __asm__ volatile("pause" ::: "memory"); }
static void native_cpu_wbinvd(void) {}

static uint64_t native_cpu_rdtsc(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void native_cpu_set_gs_base(uint64_t addr) { (void)addr; }

static void native_cpu_features(flux_cpu_features_t *feat)
{
    if (!feat) return;
    memset(feat, 0, sizeof(*feat));

    /* Report host features based on preprocessor defines */
#if defined(__SSE__)
    feat->has_sse = true;
#endif
#if defined(__SSE2__)
    feat->has_sse2 = true;
#endif
#if defined(__AVX__)
    feat->has_avx = true;
#endif
#if defined(__AVX2__)
    feat->has_avx2 = true;
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
    feat->has_neon = true;
#endif

    feat->has_mmu = true;
    feat->has_fpu = true;
    feat->has_tsc = true;
    feat->phys_addr_bits = 48;
    feat->linear_addr_bits = 64;
    feat->cache_line_size = 64;
    feat->l1_size = 32768;
    feat->l2_size = 262144;
    feat->l3_size = 8388608;
}

/* ========================================================================
 * Interrupts (Stubs)
 * ======================================================================== */

static void native_irq_init(void) {}
static void native_irq_enable(void) {}
static void native_irq_disable(void) {}
static bool native_irq_enabled(void) { return false; }
static flux_status_t native_irq_register(uint32_t irq, flux_irq_handler_t h, void *ctx)
{ (void)irq; (void)h; (void)ctx; return FLUX_OK; }
static void native_irq_unregister(uint32_t irq) { (void)irq; }
static void native_irq_eoi(uint32_t irq) { (void)irq; }

/* ========================================================================
 * Timer (Simulated)
 * ======================================================================== */

static void native_timer_init(uint32_t freq_hz) { (void)freq_hz; }

static uint64_t native_timer_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void native_timer_sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void native_timer_set_alarm(uint32_t ms, void (*callback)(void))
{
    (void)ms;
    (void)callback;
    /* Would use timer_create/settimer in a full implementation */
}

/* ========================================================================
 * Context (Stubs)
 * ======================================================================== */

static void native_context_save(flux_pcb_t *pcb) { (void)pcb; }
static void native_context_restore(const flux_pcb_t *pcb) { (void)pcb; }

/* ========================================================================
 * Port I/O (Stubs)
 * ======================================================================== */

static void native_outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static void native_outw(uint16_t port, uint16_t val) { (void)port; (void)val; }
static void native_outl(uint16_t port, uint32_t val) { (void)port; (void)val; }
static uint8_t native_inb(uint16_t port) { (void)port; return 0; }
static uint16_t native_inw(uint16_t port) { (void)port; return 0; }
static uint32_t native_inl(uint16_t port) { (void)port; return 0; }

/* ========================================================================
 * DMA (Stubs)
 * ======================================================================== */

static flux_status_t native_dma_alloc(flux_size_t size, flux_addr_t *phys, void **virt)
{
    if (!virt || size == 0) return FLUX_ERR_INVALID;
    *virt = calloc(1, size);
    if (phys) *phys = (flux_addr_t)(uintptr_t)*virt;
    return *virt ? FLUX_OK : FLUX_ERR_NOMEM;
}

static void native_dma_free(flux_addr_t phys, void *virt)
{
    (void)phys;
    free(virt);
}

static flux_status_t native_dma_transfer(flux_addr_t src, flux_addr_t dst,
                                         flux_size_t len, bool to_device)
{
    (void)src; (void)dst; (void)len; (void)to_device;
    return FLUX_OK;
}

/* ========================================================================
 * Device Management (Stubs)
 * ======================================================================== */

static flux_status_t native_device_register(flux_device_t *dev)
{ (void)dev; return FLUX_OK; }
static flux_status_t native_device_unregister(flux_device_t *dev)
{ (void)dev; return FLUX_OK; }
static flux_device_t *native_device_find(const char *name)
{ (void)name; return NULL; }
static void native_device_list(void) {}

/* ========================================================================
 * Power Management
 * ======================================================================== */

static void native_shutdown(void)
{
    fputs("\nFLUX OS shutting down.\n", stderr);
    exit(0);
}

static void native_reboot(void)
{
    fputs("\nFLUX OS rebooting.\n", stderr);
    exit(0);
}

/* ========================================================================
 * Identification
 * ======================================================================== */

static flux_arch_t native_detect_arch(void); /* forward declaration */

static flux_status_t native_hw_info_dump(char *buf, flux_size_t len)
{
    if (!buf || len == 0) return FLUX_ERR_INVALID;

    flux_arch_t arch = native_detect_arch();
    const char *arch_str = arch == FLUX_ARCH_X86_64 ? "x86_64" :
                       arch == FLUX_ARCH_ARM64 ? "ARM64" :
                       arch == FLUX_ARCH_RISCV64 ? "RISC-V64" :
                       arch == FLUX_ARCH_WASM32 ? "WASM32" : "native";

    snprintf(buf, len,
             "FLUX Native HAL\n"
             "  arch: %s\n"
             "  hal_version: " FLUX_OS_VERSION_STRING "\n"
             "  platform: hosted (Linux/macOS/POSIX)\n"
             "  features: mmu fpu sse sse2%s%s\n"
             "  memory: simulated 16MB\n",
             arch_str,
#if defined(__AVX__)
             " avx",
#else
             "",
#endif
             "");
    return FLUX_OK;
}

static flux_status_t native_hw_optimal_config(char *buf, flux_size_t len)
{
    if (!buf || len == 0) return FLUX_ERR_INVALID;

    snprintf(buf, len,
             "optimal_config {\n"
             "  target: native\n"
             "  optimize: speed\n"
             "  simd: auto\n"
             "}\n");
    return FLUX_OK;
}

static flux_arch_t native_detect_arch(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return FLUX_ARCH_X86_64;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return FLUX_ARCH_ARM64;
#elif defined(__riscv) && (__riscv_xlen == 64)
    return FLUX_ARCH_RISCV64;
#elif defined(__wasm32__) || defined(__wasm64__)
    return FLUX_ARCH_WASM32;
#else
    return FLUX_ARCH_NATIVE;
#endif
}

static const char *native_arch_name(void)
{
    return native_detect_arch() == FLUX_ARCH_X86_64 ? "x86_64" :
           native_detect_arch() == FLUX_ARCH_ARM64 ? "ARM64" :
           native_detect_arch() == FLUX_ARCH_RISCV64 ? "RISC-V64" :
           native_detect_arch() == FLUX_ARCH_WASM32 ? "WASM32" : "native";
}

static const char *native_hal_version(void)
{
    return FLUX_OS_VERSION_STRING;
}

static flux_hal_level_t native_init_level(void)
{
    return FLUX_HAL_FULL;
}

/* ========================================================================
 * HAL API Implementation
 * ======================================================================== */

void flux_hal_set(const flux_hal_t *hal)
{
    s_active_hal = hal;
}

const flux_hal_t *flux_hal_get(void)
{
    return s_active_hal;
}

flux_arch_t flux_hal_detect_arch(void)
{
    if (s_active_hal && s_active_hal->detect_arch)
        return s_active_hal->detect_arch();
    return FLUX_ARCH_UNKNOWN;
}

const char *flux_hal_arch_name(void)
{
    if (s_active_hal && s_active_hal->arch_name)
        return s_active_hal->arch_name();
    return "unknown";
}

void flux_hal_console_init(void)
{
    if (s_active_hal && s_active_hal->console_init)
        s_active_hal->console_init();
}

void flux_hal_putc(char c)
{
    if (s_active_hal && s_active_hal->console_putc)
        s_active_hal->console_putc(c);
}

void flux_hal_puts(const char *s)
{
    if (s_active_hal && s_active_hal->console_puts)
        s_active_hal->console_puts(s);
}

void flux_hal_clear(void)
{
    if (s_active_hal && s_active_hal->console_clear)
        s_active_hal->console_clear();
}

flux_hal_level_t flux_hal_boot(void)
{
    return FLUX_HAL_FULL;
}

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
 * Backend Registration
 * ======================================================================== */

/*
 * flux_hal_register_native — Register the native (hosted) HAL backend.
 * This populates the HAL function table with stdio-based implementations.
 */
void flux_hal_register_native(void)
{
    memset(&s_native_hal, 0, sizeof(s_native_hal));

    /* Identification */
    s_native_hal.detect_arch  = native_detect_arch;
    s_native_hal.arch_name    = native_arch_name;
    s_native_hal.hal_version  = native_hal_version;
    s_native_hal.init_level   = native_init_level;

    /* Console */
    s_native_hal.console_init     = native_console_init;
    s_native_hal.console_putc     = native_console_putc;
    s_native_hal.console_puts     = native_console_puts;
    s_native_hal.console_getc     = native_console_getc;
    s_native_hal.console_clear    = native_console_clear;
    s_native_hal.console_set_color = native_console_set_color;

    /* Physical Memory */
    s_native_hal.mem_total     = native_mem_total;
    s_native_hal.mem_free      = native_mem_free;
    s_native_hal.mem_alloc_phys = native_mem_alloc_phys;
    s_native_hal.mem_free_phys = native_mem_free_phys;
    s_native_hal.mem_map_count = native_mem_map_count;
    s_native_hal.mem_map_get   = native_mem_map_get;

    /* Virtual Memory */
    s_native_hal.vm_init       = native_vm_init;
    s_native_hal.vm_map        = native_vm_map;
    s_native_hal.vm_unmap      = native_vm_unmap;
    s_native_hal.vm_protect    = native_vm_protect;
    s_native_hal.vm_current_pml4 = native_vm_current_pml4;
    s_native_hal.vm_switch     = native_vm_switch;
    s_native_hal.vm_flush_tlb  = native_vm_flush_tlb;
    s_native_hal.vm_invalidate = native_vm_invalidate;

    /* CPU */
    s_native_hal.cpu_init      = native_cpu_init;
    s_native_hal.cpu_features  = native_cpu_features;
    s_native_hal.cpu_halt      = native_cpu_halt;
    s_native_hal.cpu_pause     = native_cpu_pause;
    s_native_hal.cpu_wbinvd    = native_cpu_wbinvd;
    s_native_hal.cpu_rdtsc     = native_cpu_rdtsc;
    s_native_hal.cpu_set_gs_base = native_cpu_set_gs_base;

    /* Interrupts */
    s_native_hal.irq_init      = native_irq_init;
    s_native_hal.irq_enable    = native_irq_enable;
    s_native_hal.irq_disable   = native_irq_disable;
    s_native_hal.irq_enabled   = native_irq_enabled;
    s_native_hal.irq_register  = native_irq_register;
    s_native_hal.irq_unregister = native_irq_unregister;
    s_native_hal.irq_eoi       = native_irq_eoi;

    /* Timer */
    s_native_hal.timer_init    = native_timer_init;
    s_native_hal.timer_ticks   = native_timer_ticks;
    s_native_hal.timer_sleep_ms = native_timer_sleep_ms;
    s_native_hal.timer_set_alarm = native_timer_set_alarm;

    /* Context */
    s_native_hal.context_save  = native_context_save;
    s_native_hal.context_restore = native_context_restore;

    /* Port I/O */
    s_native_hal.outb = native_outb;
    s_native_hal.outw = native_outw;
    s_native_hal.outl = native_outl;
    s_native_hal.inb  = native_inb;
    s_native_hal.inw  = native_inw;
    s_native_hal.inl  = native_inl;

    /* DMA */
    s_native_hal.dma_alloc    = native_dma_alloc;
    s_native_hal.dma_free     = native_dma_free;
    s_native_hal.dma_transfer = native_dma_transfer;

    /* Devices */
    s_native_hal.device_register   = native_device_register;
    s_native_hal.device_unregister = native_device_unregister;
    s_native_hal.device_find       = native_device_find;
    s_native_hal.device_list       = native_device_list;

    /* Power */
    s_native_hal.shutdown = native_shutdown;
    s_native_hal.reboot   = native_reboot;

    /* Hardware Info */
    s_native_hal.hw_info_dump       = native_hw_info_dump;
    s_native_hal.hw_optimal_config  = native_hw_optimal_config;

    /* Set as active HAL */
    flux_hal_set(&s_native_hal);
}

/* Other arch backends — stubs */
void flux_hal_register_x86_64(void) { flux_hal_register_native(); }
void flux_hal_register_arm64(void)  { flux_hal_register_native(); }
void flux_hal_register_riscv64(void) { flux_hal_register_native(); }
