/*
 * FLUX OS — Native/Hosted HAL Backend
 *
 * Implements the full flux_hal_t interface for hosted mode (Linux/macOS/POSIX).
 * This is the primary development and testing backend. It provides:
 *
 *   - stdio-based console with ANSI color support
 *   - Simulated 64 MB physical memory using host malloc/free
 *   - POSIX timers (clock_gettime, nanosleep)
 *   - setjmp/longjmp based context save/restore
 *   - Simple linked-list device registry
 *   - Stub port I/O with optional debug logging
 *
 * The native HAL allows the FLUX kernel to run as a regular user-space
 * process, enabling:
 *   - Development without real hardware or emulators
 *   - Unit testing of kernel subsystems
 *   - The self-compiler to operate on the host
 *   - Agent simulation and A2A protocol testing
 *
 * Copyright (c) 2025 SuperInstance
 */

#define _POSIX_C_SOURCE 199309L

#include "flux/hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

/* ========================================================================
 * Configuration Constants
 * ======================================================================== */

#define NATIVE_MEM_SIZE        (64ULL * 1024ULL * 1024ULL)  /* 64 MB simulated */
#define NATIVE_MAX_DEVICES     64
#define NATIVE_MAX_IRQ         256
#define NATIVE_MAX_MEM_MAPS    32

/* ========================================================================
 * Static HAL Instance
 * ======================================================================== */

static flux_hal_t s_native_hal;

/* ========================================================================
 * Simulated Physical Memory
 *
 * We maintain a simple bitmap allocator over a simulated physical
 * address space.  Pages are 4 KB.
 * ======================================================================== */

#define PHYS_PAGE_SIZE       4096
#define PHYS_PAGE_COUNT      (NATIVE_MEM_SIZE / PHYS_PAGE_SIZE)
#define PHYS_BITMAP_SIZE     (PHYS_PAGE_COUNT / 8 + 1)

static uint8_t s_phys_bitmap[PHYS_BITMAP_SIZE];
static uint8_t s_phys_memory[PHYS_PAGE_SIZE * 16]; /* 64 KB backing store */

static void phys_bitmap_set(int page)
{
    s_phys_bitmap[page / 8] |= (1 << (page % 8));
}

static void phys_bitmap_clear(int page)
{
    s_phys_bitmap[page / 8] &= ~(1 << (page % 8));
}

static int phys_bitmap_test(int page)
{
    return (s_phys_bitmap[page / 8] >> (page % 8)) & 1;
}

/* ========================================================================
 * Simulated Memory Map
 * ======================================================================== */

static flux_mem_map_t s_mem_maps[NATIVE_MAX_MEM_MAPS];
static int s_mem_map_count = 0;

/* ========================================================================
 * Device Registry (Linked List)
 * ======================================================================== */

typedef struct native_device_node {
    flux_device_t            device;
    struct native_device_node *next;
} native_device_node_t;

static native_device_node_t *s_device_list_head = NULL;
static native_device_node_t *s_device_list_tail = NULL;
static int s_device_count = 0;

/* ========================================================================
 * Interrupt Simulation
 * ======================================================================== */

static flux_irq_t s_irq_table[NATIVE_MAX_IRQ];
static bool s_irq_enabled = false;

/* Timer alarm state */
static struct {
    bool      armed;
    uint32_t  delay_ms;
    void    (*callback)(void);
    time_t    arm_time;
} s_timer_alarm = { false, 0, NULL, 0 };

/* ========================================================================
 * Context Save State (setjmp/longjmp)
 * ======================================================================== */

#define MAX_PCB_JMP 16
static jmp_buf s_pcb_envs[MAX_PCB_JMP];
static int      s_pcb_env_used[MAX_PCB_JMP];
static int      s_pcb_slot_counter = 0;

/* ========================================================================
 * ANSI Color Codes
 * ======================================================================== */

static const char *ansi_fg_colors[16] = {
    "\033[30m", /* 0: Black   */
    "\033[34m", /* 1: Blue    */
    "\033[32m", /* 2: Green   */
    "\033[36m", /* 3: Cyan    */
    "\033[31m", /* 4: Red     */
    "\033[35m", /* 5: Magenta */
    "\033[33m", /* 6: Yellow  */
    "\033[37m", /* 7: White   */
    "\033[90m", /* 8: Bright Black   */
    "\033[94m", /* 9: Bright Blue    */
    "\033[92m", /* 10: Bright Green  */
    "\033[96m", /* 11: Bright Cyan   */
    "\033[91m", /* 12: Bright Red    */
    "\033[95m", /* 13: Bright Magenta */
    "\033[93m", /* 14: Bright Yellow  */
    "\033[97m", /* 15: Bright White  */
};

static const char *ansi_bg_colors[8] = {
    "\033[40m", /* 0: Black */
    "\033[44m", /* 1: Blue  */
    "\033[42m", /* 2: Green */
    "\033[46m", /* 3: Cyan  */
    "\033[41m", /* 4: Red   */
    "\033[45m", /* 5: Magenta */
    "\033[43m", /* 6: Yellow  */
    "\033[47m", /* 7: White   */
};

/* ========================================================================
 * Identification
 * ======================================================================== */

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
    return "native-hosted";
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
 * Console (stdio with ANSI support)
 * ======================================================================== */

static void native_console_init(void)
{
    /* Disable stdout buffering for immediate output */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

static void native_console_putc(char c)
{
    putchar(c);
    fflush(stdout);
}

static void native_console_puts(const char *s)
{
    if (s)
        fputs(s, stdout);
}

static char native_console_getc(void)
{
    return (char)getchar();
}

static void native_console_clear(void)
{
    /* ANSI escape: clear screen and move cursor to top-left */
    printf("\033[2J\033[H");
    fflush(stdout);
}

static void native_console_set_color(uint8_t fg, uint8_t bg)
{
    /* Set foreground color */
    if (fg < 16)
        fputs(ansi_fg_colors[fg], stdout);

    /* Set background color */
    if (bg < 8)
        fputs(ansi_bg_colors[bg], stdout);

    fflush(stdout);
}

/* ========================================================================
 * Physical Memory (Bitmap Allocator)
 * ======================================================================== */

static void native_mem_init(void)
{
    memset(s_phys_bitmap, 0, sizeof(s_phys_bitmap));
    /* Reserve page 0 (null page) */
    phys_bitmap_set(0);
}

static flux_size_t native_mem_total(void)
{
    return NATIVE_MEM_SIZE;
}

static flux_size_t native_mem_free(void)
{
    int free_pages = 0;
    for (int i = 0; i < PHYS_PAGE_COUNT; i++) {
        if (!phys_bitmap_test(i))
            free_pages++;
    }
    return (flux_size_t)free_pages * PHYS_PAGE_SIZE;
}

static flux_addr_t native_mem_alloc_phys(flux_size_t size, flux_size_t align)
{
    (void)align; /* Pages are already aligned to 4 KB */

    int pages_needed = (int)((size + PHYS_PAGE_SIZE - 1) / PHYS_PAGE_SIZE);

    /* Linear search for contiguous free pages */
    int consecutive = 0;
    int start_page = -1;

    for (int i = 1; i < PHYS_PAGE_COUNT && consecutive < pages_needed; i++) {
        if (!phys_bitmap_test(i)) {
            if (consecutive == 0)
                start_page = i;
            consecutive++;
        } else {
            consecutive = 0;
            start_page = -1;
        }
    }

    if (consecutive < pages_needed)
        return 0; /* Out of memory */

    /* Mark pages as allocated */
    for (int i = 0; i < pages_needed; i++)
        phys_bitmap_set(start_page + i);

    return (flux_addr_t)(start_page * PHYS_PAGE_SIZE);
}

static void native_mem_free_phys(flux_addr_t addr, flux_size_t size)
{
    int start_page = (int)(addr / PHYS_PAGE_SIZE);
    int pages = (int)((size + PHYS_PAGE_SIZE - 1) / PHYS_PAGE_SIZE);

    for (int i = 0; i < pages; i++) {
        int page = start_page + i;
        if (page >= 0 && page < PHYS_PAGE_COUNT)
            phys_bitmap_clear(page);
    }
}

static int native_mem_map_count_fn(void)
{
    return s_mem_map_count;
}

static const flux_mem_map_t *native_mem_map_get_fn(int index)
{
    if (index < 0 || index >= s_mem_map_count)
        return NULL;
    return &s_mem_maps[index];
}

/* ========================================================================
 * Virtual Memory (Stubs — hosted mode uses host MMU)
 * ======================================================================== */

static flux_status_t native_vm_init(void)
{
    native_mem_init();
    return FLUX_OK;
}

static flux_status_t native_vm_map(flux_addr_t virt, flux_addr_t phys,
                                   flux_size_t size, flux_perm_t perm)
{
    (void)virt; (void)phys; (void)size; (void)perm;

    /* Add to memory map for bookkeeping */
    if (s_mem_map_count < NATIVE_MAX_MEM_MAPS) {
        s_mem_maps[s_mem_map_count].base = virt;
        s_mem_maps[s_mem_map_count].size = size;
        s_mem_maps[s_mem_map_count].type = FLUX_MEM_KERNEL;
        s_mem_maps[s_mem_map_count].perms = perm;
        s_mem_maps[s_mem_map_count].name = "native_map";
        s_mem_map_count++;
    }

    return FLUX_OK;
}

static flux_status_t native_vm_unmap(flux_addr_t virt, flux_size_t size)
{
    (void)virt; (void)size;
    return FLUX_OK;
}

static flux_status_t native_vm_protect(flux_addr_t virt, flux_size_t size,
                                       flux_perm_t perm)
{
    (void)virt; (void)size; (void)perm;
    return FLUX_OK;
}

static flux_addr_t native_vm_current_pml4(void)
{
    return 0;
}

static void native_vm_switch(flux_addr_t pml4)
{
    (void)pml4;
}

static void native_vm_flush_tlb(void)
{
    /* No-op in hosted mode */
}

static void native_vm_invalidate(flux_addr_t addr)
{
    (void)addr;
}

/* ========================================================================
 * CPU Features (Detect Host Capabilities)
 * ======================================================================== */

static void native_cpu_init(void)
{
    /* CPU is already initialized by the host OS */
}

static void native_cpu_features(flux_cpu_features_t *feat)
{
    if (!feat) return;
    memset(feat, 0, sizeof(*feat));

    /* Detect host CPU features from preprocessor defines */
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
#if defined(__AVX512F__)
    feat->has_avx512 = true;
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
    feat->has_neon = true;
#endif
#if defined(__riscv_vector)
    feat->has_rvv = true;
#endif

    feat->has_mmu  = true;
    feat->has_fpu  = true;
    feat->has_tsc  = true;
    feat->has_apic = false; /* No APIC in hosted mode */
    feat->has_msi  = false;

    feat->phys_addr_bits   = 48;
    feat->linear_addr_bits = 64;
    feat->cache_line_size  = 64;
    feat->l1_size = 32768;      /* 32 KB (typical) */
    feat->l2_size = 262144;     /* 256 KB (typical) */
    feat->l3_size = 8388608;    /* 8 MB (typical) */
}

static void native_cpu_halt(void)
{
    /* In hosted mode, halt just yields */
    sleep(1);
}

static void native_cpu_pause(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield" ::: "memory");
#else
    /* Generic: compiler barrier */
    __asm__ volatile("" ::: "memory");
#endif
}

static void native_cpu_wbinvd(void)
{
    /* No-op in hosted mode (host OS manages cache) */
    __asm__ volatile("" ::: "memory");
}

static uint64_t native_cpu_rdtsc(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
#else
    /* Fallback to POSIX monotonic clock */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static void native_cpu_set_gs_base(uint64_t addr)
{
    (void)addr;
    /* No-op in hosted mode */
}

/* ========================================================================
 * Interrupts (Simulated)
 * ======================================================================== */

static void native_irq_init(void)
{
    memset(s_irq_table, 0, sizeof(s_irq_table));
    s_irq_enabled = false;
}

static void native_irq_enable(void)
{
    s_irq_enabled = true;
}

static void native_irq_disable(void)
{
    s_irq_enabled = false;
}

static bool native_irq_enabled_fn(void)
{
    return s_irq_enabled;
}

static flux_status_t native_irq_register(uint32_t irq, flux_irq_handler_t handler,
                                         void *ctx)
{
    if (irq >= NATIVE_MAX_IRQ)
        return FLUX_ERR_INVALID;

    s_irq_table[irq].irq_num  = irq;
    s_irq_table[irq].handler  = handler;
    s_irq_table[irq].ctx      = ctx;
    s_irq_table[irq].enabled  = true;
    return FLUX_OK;
}

static void native_irq_unregister(uint32_t irq)
{
    if (irq >= NATIVE_MAX_IRQ) return;
    s_irq_table[irq].handler = NULL;
    s_irq_table[irq].ctx     = NULL;
    s_irq_table[irq].enabled = false;
}

static void native_irq_eoi(uint32_t irq)
{
    (void)irq;
    /* No-op in hosted mode */
}

/**
 * native_irq_fire — Simulate firing an interrupt (for testing).
 */
void native_irq_fire(uint32_t irq)
{
    if (irq >= NATIVE_MAX_IRQ) return;
    if (!s_irq_table[irq].handler || !s_irq_table[irq].enabled) return;

    s_irq_table[irq].handler(s_irq_table[irq].ctx, irq);
}

/* ========================================================================
 * Timer (POSIX)
 * ======================================================================== */

static uint32_t s_timer_freq_hz = 1000;

static void native_timer_init(uint32_t freq_hz)
{
    s_timer_freq_hz = freq_hz > 0 ? freq_hz : 1000;
}

static uint64_t native_timer_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    /* Convert to ticks at the configured frequency */
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    return ns / (1000000000ULL / s_timer_freq_hz);
}

static void native_timer_sleep_ms(uint32_t ms)
{
    usleep((useconds_t)ms * 1000);
}

static void native_timer_set_alarm(uint32_t ms, void (*callback)(void))
{
    s_timer_alarm.armed     = true;
    s_timer_alarm.delay_ms  = ms;
    s_timer_alarm.callback  = callback;
    time(&s_timer_alarm.arm_time);
}

/**
 * native_timer_poll_alarm — Check and fire alarm if expired.
 * Must be called periodically (e.g., from a main loop or signal handler).
 */
void native_timer_poll_alarm(void)
{
    if (!s_timer_alarm.armed || !s_timer_alarm.callback)
        return;

    time_t now;
    time(&now);
    double elapsed = difftime(now, s_timer_alarm.arm_time) * 1000.0;

    if (elapsed >= s_timer_alarm.delay_ms) {
        s_timer_alarm.armed = false;
        s_timer_alarm.callback();
    }
}

/* ========================================================================
 * Context Save/Restore (setjmp/longjmp)
 * ======================================================================== */

static void native_context_save(flux_pcb_t *pcb)
{
    if (!pcb) return;

    /* Find or allocate a slot */
    int slot = -1;
    for (int i = 0; i < MAX_PCB_JMP; i++) {
        if (s_pcb_env_used[i] && s_pcb_envs[i] == pcb->regs[0]) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* Allocate a new slot */
        for (int i = 0; i < MAX_PCB_JMP; i++) {
            if (!s_pcb_env_used[i]) {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0) {
        /* No free slots — overwrite the oldest */
        slot = s_pcb_slot_counter % MAX_PCB_JMP;
    }

    /* Store slot index in pcb->regs[0] as our handle */
    pcb->regs[0] = (uint64_t)slot;
    s_pcb_env_used[slot] = 1;
    setjmp(s_pcb_envs[slot]);
}

static void native_context_restore(const flux_pcb_t *pcb)
{
    if (!pcb) return;

    int slot = (int)pcb->regs[0];
    if (slot < 0 || slot >= MAX_PCB_JMP || !s_pcb_env_used[slot])
        return;

    longjmp(s_pcb_envs[slot], 1);
}

/* ========================================================================
 * Port I/O (Stubs with Optional Logging)
 * ======================================================================== */

#ifndef NATIVE_PORT_IO_DEBUG
#define NATIVE_PORT_IO_DEBUG 0
#endif

static void native_outb(uint16_t port, uint8_t val)
{
#if NATIVE_PORT_IO_DEBUG
    fprintf(stderr, "[HAL:IO] outb(0x%04x, 0x%02x)\n", port, val);
#else
    (void)port; (void)val;
#endif
}

static void native_outw(uint16_t port, uint16_t val)
{
#if NATIVE_PORT_IO_DEBUG
    fprintf(stderr, "[HAL:IO] outw(0x%04x, 0x%04x)\n", port, val);
#else
    (void)port; (void)val;
#endif
}

static void native_outl(uint16_t port, uint32_t val)
{
#if NATIVE_PORT_IO_DEBUG
    fprintf(stderr, "[HAL:IO] outl(0x%04x, 0x%08x)\n", port, val);
#else
    (void)port; (void)val;
#endif
}

static uint8_t native_inb(uint16_t port)
{
#if NATIVE_PORT_IO_DEBUG
    fprintf(stderr, "[HAL:IO] inb(0x%04x) -> 0x00\n", port);
#else
    (void)port;
#endif
    return 0;
}

static uint16_t native_inw(uint16_t port)
{
#if NATIVE_PORT_IO_DEBUG
    fprintf(stderr, "[HAL:IO] inw(0x%04x) -> 0x0000\n", port);
#else
    (void)port;
#endif
    return 0;
}

static uint32_t native_inl(uint16_t port)
{
#if NATIVE_PORT_IO_DEBUG
    fprintf(stderr, "[HAL:IO] inl(0x%04x) -> 0x00000000\n", port);
#else
    (void)port;
#endif
    return 0;
}

/* ========================================================================
 * DMA (Host malloc-based)
 * ======================================================================== */

static flux_status_t native_dma_alloc(flux_size_t size, flux_addr_t *phys,
                                      void **virt)
{
    if (!virt || size == 0)
        return FLUX_ERR_INVALID;

    *virt = calloc(1, (size_t)size);
    if (!*virt)
        return FLUX_ERR_NOMEM;

    if (phys)
        *phys = (flux_addr_t)(uintptr_t)*virt;

    return FLUX_OK;
}

static void native_dma_free(flux_addr_t phys, void *virt)
{
    (void)phys;
    free(virt);
}

static flux_status_t native_dma_transfer(flux_addr_t src, flux_addr_t dst,
                                         flux_size_t len, bool to_device)
{
    (void)to_device;
    if (len == 0) return FLUX_OK;

    /* Simulate DMA by memcpy between addresses */
    void *src_ptr = (void *)(uintptr_t)src;
    void *dst_ptr = (void *)(uintptr_t)dst;

    if (!src_ptr || !dst_ptr)
        return FLUX_ERR_INVALID;

    memcpy(dst_ptr, src_ptr, (size_t)len);
    return FLUX_OK;
}

/* ========================================================================
 * Device Management (Linked List)
 * ======================================================================== */

static flux_status_t native_device_register(flux_device_t *dev)
{
    if (!dev) return FLUX_ERR_INVALID;
    if (s_device_count >= NATIVE_MAX_DEVICES)
        return FLUX_ERR_OVERFLOW;

    /* Allocate a new node */
    native_device_node_t *node = (native_device_node_t *)calloc(
        1, sizeof(native_device_node_t));
    if (!node)
        return FLUX_ERR_NOMEM;

    /* Copy device data into the node */
    memcpy(&node->device, dev, sizeof(flux_device_t));
    node->next = NULL;

    /* Append to linked list */
    if (s_device_list_tail) {
        s_device_list_tail->next = node;
        s_device_list_tail = node;
    } else {
        s_device_list_head = node;
        s_device_list_tail = node;
    }

    s_device_count++;

    /* Call device init if available */
    if (node->device.init)
        node->device.init(&node->device);

    return FLUX_OK;
}

static flux_status_t native_device_unregister(flux_device_t *dev)
{
    if (!dev) return FLUX_ERR_INVALID;

    native_device_node_t **pp = &s_device_list_head;
    while (*pp) {
        native_device_node_t *node = *pp;
        if (&node->device == dev || strcmp(node->device.name, dev->name) == 0) {
            /* Call device deinit if available */
            if (node->device.deinit)
                node->device.deinit(&node->device);

            *pp = node->next;
            if (s_device_list_tail == node)
                s_device_list_tail = *pp;
            free(node);
            s_device_count--;
            return FLUX_OK;
        }
        pp = &node->next;
    }

    return FLUX_ERR_NOTFOUND;
}

static flux_device_t *native_device_find(const char *name)
{
    if (!name) return NULL;

    native_device_node_t *node = s_device_list_head;
    while (node) {
        if (strcmp(node->device.name, name) == 0)
            return &node->device;
        node = node->next;
    }

    return NULL;
}

static void native_device_list(void)
{
    printf("=== FLUX Device Registry (%d devices) ===\n", s_device_count);

    native_device_node_t *node = s_device_list_head;
    while (node) {
        printf("  [%03u] %-24s  private=%p\n",
               node->device.dev_id, node->device.name,
               node->device.private_data);
        node = node->next;
    }

    printf("==========================================\n");
}

/* ========================================================================
 * Power Management
 * ======================================================================== */

static void native_shutdown(void)
{
    printf("\n[HAL] FLUX OS shutting down.\n");
    fflush(stdout);
    exit(0);
}

static void native_reboot(void)
{
    printf("\n[HAL] FLUX OS rebooting (re-exiting in hosted mode).\n");
    fflush(stdout);
    exit(0);
}

/* ========================================================================
 * Hardware Info (for self-compiler)
 * ======================================================================== */

static flux_status_t native_hw_info_dump(char *buf, flux_size_t len)
{
    if (!buf || len == 0)
        return FLUX_ERR_INVALID;

    flux_cpu_features_t feat;
    native_cpu_features(&feat);

    int offset = 0;
    offset += snprintf(buf + offset, len - offset,
        "FLUX Hardware Information\n"
        "==========================\n"
        "  HAL Version:   %s\n"
        "  Architecture:  %s\n"
        "  Platform:      native-hosted (POSIX)\n"
        "  Simulated RAM: %llu MB\n"
        "  Devices:       %d registered\n"
        "\n"
        "CPU Features:\n"
        "  MMU:           %s\n"
        "  FPU:           %s\n"
        "  SSE:           %s\n"
        "  SSE2:          %s\n"
        "  AVX:           %s\n"
        "  AVX2:          %s\n"
        "  AVX-512:       %s\n"
        "  NEON:          %s\n"
        "  RISC-V VEC:    %s\n"
        "  TSC:           %s\n"
        "  Cache Line:    %u bytes\n"
        "  L1 Cache:      %u KB\n"
        "  L2 Cache:      %u KB\n"
        "  L3 Cache:      %u MB\n"
        "  Phys Addr:     %u bits\n"
        "  Linear Addr:   %u bits\n",
        FLUX_OS_VERSION_STRING,
        native_arch_name(),
        (unsigned long long)(NATIVE_MEM_SIZE / (1024 * 1024)),
        s_device_count,
        feat.has_mmu    ? "yes" : "no",
        feat.has_fpu    ? "yes" : "no",
        feat.has_sse    ? "yes" : "no",
        feat.has_sse2   ? "yes" : "no",
        feat.has_avx    ? "yes" : "no",
        feat.has_avx2   ? "yes" : "no",
        feat.has_avx512 ? "yes" : "no",
        feat.has_neon   ? "yes" : "no",
        feat.has_rvv    ? "yes" : "no",
        feat.has_tsc    ? "yes" : "no",
        feat.cache_line_size,
        feat.l1_size / 1024,
        feat.l2_size / 1024,
        feat.l3_size / (1024 * 1024),
        feat.phys_addr_bits,
        feat.linear_addr_bits);

    (void)offset;
    return FLUX_OK;
}

static flux_status_t native_hw_optimal_config(char *buf, flux_size_t len)
{
    if (!buf || len == 0)
        return FLUX_ERR_INVALID;

    flux_cpu_features_t feat;
    native_cpu_features(&feat);

    /* Determine best SIMD strategy */
    const char *simd = "none";
    if (feat.has_avx512)     simd = "avx512";
    else if (feat.has_avx2)  simd = "avx2";
    else if (feat.has_avx)   simd = "avx";
    else if (feat.has_sse2)  simd = "sse2";
    else if (feat.has_sse)   simd = "sse";
    else if (feat.has_neon)  simd = "neon";
    else if (feat.has_rvv)   simd = "rvv";

    snprintf(buf, len,
        "optimal_config {\n"
        "  target:       native\n"
        "  optimize:     speed\n"
        "  simd:         %s\n"
        "  cache_line:   %u\n"
        "  page_size:    4096\n"
        "  stack_size:   65536\n"
        "  vm_enabled:   false\n"
        "  compiler:     flux-bytecode\n"
        "  jit:          enabled\n"
        "}\n",
        simd,
        feat.cache_line_size);

    return FLUX_OK;
}

/* ========================================================================
 * Registration — Populate HAL Function Table
 * ======================================================================== */

void flux_hal_register_native_impl(void)
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
    s_native_hal.mem_total      = native_mem_total;
    s_native_hal.mem_free       = native_mem_free;
    s_native_hal.mem_alloc_phys = native_mem_alloc_phys;
    s_native_hal.mem_free_phys  = native_mem_free_phys;
    s_native_hal.mem_map_count  = native_mem_map_count_fn;
    s_native_hal.mem_map_get    = native_mem_map_get_fn;

    /* Virtual Memory */
    s_native_hal.vm_init         = native_vm_init;
    s_native_hal.vm_map          = native_vm_map;
    s_native_hal.vm_unmap        = native_vm_unmap;
    s_native_hal.vm_protect      = native_vm_protect;
    s_native_hal.vm_current_pml4 = native_vm_current_pml4;
    s_native_hal.vm_switch       = native_vm_switch;
    s_native_hal.vm_flush_tlb    = native_vm_flush_tlb;
    s_native_hal.vm_invalidate   = native_vm_invalidate;

    /* CPU */
    s_native_hal.cpu_init        = native_cpu_init;
    s_native_hal.cpu_features    = native_cpu_features;
    s_native_hal.cpu_halt        = native_cpu_halt;
    s_native_hal.cpu_pause       = native_cpu_pause;
    s_native_hal.cpu_wbinvd      = native_cpu_wbinvd;
    s_native_hal.cpu_rdtsc       = native_cpu_rdtsc;
    s_native_hal.cpu_set_gs_base = native_cpu_set_gs_base;

    /* Interrupts */
    s_native_hal.irq_init      = native_irq_init;
    s_native_hal.irq_enable    = native_irq_enable;
    s_native_hal.irq_disable   = native_irq_disable;
    s_native_hal.irq_enabled   = native_irq_enabled_fn;
    s_native_hal.irq_register  = native_irq_register;
    s_native_hal.irq_unregister = native_irq_unregister;
    s_native_hal.irq_eoi       = native_irq_eoi;

    /* Timer */
    s_native_hal.timer_init      = native_timer_init;
    s_native_hal.timer_ticks     = native_timer_ticks;
    s_native_hal.timer_sleep_ms  = native_timer_sleep_ms;
    s_native_hal.timer_set_alarm = native_timer_set_alarm;

    /* Context */
    s_native_hal.context_save    = native_context_save;
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
    s_native_hal.hw_info_dump      = native_hw_info_dump;
    s_native_hal.hw_optimal_config = native_hw_optimal_config;

    /* Set as active HAL */
    flux_hal_set(&s_native_hal);
}
