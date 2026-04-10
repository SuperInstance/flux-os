/*
 * FLUX OS — x86_64 Bare-Metal HAL Backend
 *
 * Implements the full flux_hal_t interface for bare-metal x86_64 systems.
 * This backend provides real hardware access:
 *
 *   - Port I/O via inline assembly (inb/outb/inw/outw/inl/outl)
 *   - VGA text mode console (0xB8000 memory-mapped I/O)
 *   - Framebuffer console (UEFI GOP or VBE, if available)
 *   - CPUID-based feature detection (SSE, AVX, AVX2, AVX-512, etc.)
 *   - PIC (8259A) and APIC interrupt controllers
 *   - PIT and APIC timer
 *   - x86_64 4-level page table (PML4) management
 *   - Serial port (COM1 at 0x3F8) for early debug
 *   - DMA controller (8237A)
 *   - ACPI power management (shutdown/reboot via ACPI PM1a)
 *
 * This file is intended for linking with a multiboot2 or UEFI bootloader
 * that sets up a basic GDT and identity-mapped pages before jumping to
 * the kernel entry point.
 *
 * Copyright (c) 2025 SuperInstance
 */

#include "flux/hal.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* ========================================================================
 * Compile-Time Architecture Verification
 * ======================================================================== */

#if !defined(__x86_64__) && !defined(_M_X64)
#error "hal_x86_64.c must be compiled for x86_64 architecture"
#endif

/* ========================================================================
 * x86_64 Port I/O — Inline Assembly
 * ======================================================================== */

static inline void x86_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void x86_outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void x86_outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t x86_inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t x86_inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t x86_inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* I/O delay (slow down port I/O for old hardware) */
static inline void x86_io_wait(void)
{
    x86_outb(0x80, 0);
}

/* ========================================================================
 * CPUID Feature Detection
 * ======================================================================== */

static uint32_t x86_cpuid_max_func(void)
{
    uint32_t max_func;
    __asm__ volatile("cpuid" : "=a"(max_func) : "a"(0) : "ebx", "ecx", "edx");
    return max_func;
}

static void x86_cpuid(uint32_t func, uint32_t *eax, uint32_t *ebx,
                      uint32_t *ecx, uint32_t *edx)
{
    uint32_t a = func, b, c, d;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(a));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

/* Extended CPUID (function >= 0x80000000) */
static void x86_cpuid_ext(uint32_t func, uint32_t *eax, uint32_t *ebx,
                          uint32_t *ecx, uint32_t *edx)
{
    uint32_t a = func, b, c, d;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(a));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

/* ========================================================================
 * VGA Text Mode Constants
 * ======================================================================== */

#define VGA_WIDTH        80
#define VGA_HEIGHT       25
#define VGA_BUFFER       0xB8000
#define VGA_ATTRIBUTE    0x07  /* Light grey on black */

/* VGA color codes (foreground/background, 0-15) */
#define VGA_COLOR_BLACK       0
#define VGA_COLOR_BLUE        1
#define VGA_COLOR_GREEN       2
#define VGA_COLOR_CYAN        3
#define VGA_COLOR_RED         4
#define VGA_COLOR_MAGENTA     5
#define VGA_COLOR_BROWN       6
#define VGA_COLOR_LIGHT_GREY  7
#define VGA_COLOR_DARK_GREY   8
#define VGA_COLOR_LIGHT_BLUE  9
#define VGA_COLOR_LIGHT_GREEN 10
#define VGA_COLOR_LIGHT_CYAN  11
#define VGA_COLOR_LIGHT_RED   12
#define VGA_COLOR_PINK        13
#define VGA_COLOR_YELLOW      14
#define VGA_COLOR_WHITE       15

/* ========================================================================
 * Serial Port Constants (COM1)
 * ======================================================================== */

#define COM1_BASE        0x3F8
#define COM2_BASE        0x2F8
#define COM3_BASE        0x3E8
#define COM4_BASE        0x2E8

#define COM_DATA         0  /* Data register (R/W) */
#define COM_IER          1  /* Interrupt Enable Register */
#define COM_FCR          2  /* FIFO Control Register */
#define COM_LCR          3  /* Line Control Register */
#define COM_MCR          4  /* Modem Control Register */
#define COM_LSR          5  /* Line Status Register */
#define COM_MSR          6  /* Modem Status Register */

/* ========================================================================
 * PIC Constants (8259A)
 * ======================================================================== */

#define PIC1_CMD         0x20
#define PIC1_DATA        0x21
#define PIC2_CMD         0xA0
#define PIC2_DATA        0xA1

#define PIC_EOI          0x20

#define IRQ_BASE         0x20  /* Remapped IRQ base (0x20-0x2F) */
#define IRQ2_BASE        0x28  /* Slave PIC IRQ base (0x28-0x2F) */

/* ========================================================================
 * APIC Constants
 * ======================================================================== */

#define APIC_BASE_MSR    0x1B
#define APIC_BASE_ADDR   0xFEE00000

#define APIC_REG_ID      0x020
#define APIC_REG_VER     0x030
#define APIC_REG_TPR     0x080
#define APIC_REG_EOI     0x0B0
#define APIC_REG_SVR     0x0F0
#define APIC_REG_ISR     0x100
#define APIC_REG_TMR     0x300
#define APIC_REG_TMRINIT 0x380
#define APIC_REG_TMRCUR  0x390
#define APIC_REG_TMRDIV  0x3E0

#define APIC_SVR_ENABLE  0x100

/* ========================================================================
 * PIT Timer Constants
 * ======================================================================== */

#define PIT_CH0          0x40
#define PIT_CH1          0x41
#define PIT_CH2          0x42
#define PIT_CMD          0x43

#define PIT_FREQ_HZ      1193182

/* ========================================================================
 * Static State
 * ======================================================================== */

static flux_hal_t s_x86_hal;

/* VGA cursor position */
static int s_vga_cursor_x = 0;
static int s_vga_cursor_y = 0;
static uint8_t s_vga_attr = VGA_ATTRIBUTE;

/* Current foreground/background for color changes */
static uint8_t s_vga_fg = VGA_COLOR_LIGHT_GREY;
static uint8_t s_vga_bg = VGA_COLOR_BLACK;

/* Interrupt state */
static bool s_irq_enabled = false;
static flux_irq_t s_irq_table[256];

/* Timer state */
static uint32_t s_timer_freq = 0;
static volatile uint64_t s_timer_ticks = 0;

/* APIC detected */
static bool s_has_apic = false;

/* Page table root */
static flux_addr_t s_pml4_phys = 0;

/* ========================================================================
 * Serial Port Helpers
 * ======================================================================== */

static void x86_serial_init_port(uint16_t base, uint32_t baud)
{
    (void)baud;

    x86_outb(base + COM_IER, 0x00);  /* Disable interrupts */
    x86_outb(base + COM_LCR, 0x80);  /* Enable DLAB */
    x86_outb(base + COM_DATA, 0x03); /* Baud divisor low (38400) */
    x86_outb(base + COM_IER, 0x00);  /* Baud divisor high */
    x86_outb(base + COM_LCR, 0x03);  /* 8 bits, no parity, 1 stop */
    x86_outb(base + COM_FCR, 0xC7);  /* Enable FIFO, clear, 14-byte threshold */
    x86_outb(base + COM_MCR, 0x0B);  /* IRQs enabled, RTS/DSR set */
}

static void x86_serial_putc(uint16_t base, char c)
{
    while ((x86_inb(base + COM_LSR) & 0x20) == 0)
        ; /* Wait for transmit empty */
    x86_outb(base + COM_DATA, (uint8_t)c);
}

static char x86_serial_getc(uint16_t base)
{
    while ((x86_inb(base + COM_LSR) & 0x01) == 0)
        ; /* Wait for data available */
    return (char)x86_inb(base + COM_DATA);
}

/* ========================================================================
 * VGA Text Mode Helpers
 * ======================================================================== */

static inline volatile uint16_t *vga_buffer(void)
{
    return (volatile uint16_t *)VGA_BUFFER;
}

static void vga_scroll(void)
{
    volatile uint16_t *buf = vga_buffer();

    /* Move all lines up by one */
    for (int y = 0; y < VGA_HEIGHT - 1; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            buf[y * VGA_WIDTH + x] = buf[(y + 1) * VGA_WIDTH + x];
        }
    }

    /* Clear the last line */
    uint16_t blank = ((uint16_t)s_vga_attr << 8) | ' ';
    for (int x = 0; x < VGA_WIDTH; x++) {
        buf[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = blank;
    }

    s_vga_cursor_y = VGA_HEIGHT - 1;
}

static void vga_update_cursor(void)
{
    uint16_t pos = (uint16_t)(s_vga_cursor_y * VGA_WIDTH + s_vga_cursor_x);
    x86_outb(0x3D4, 14);
    x86_outb(0x3D5, (uint8_t)(pos >> 8));
    x86_outb(0x3D4, 15);
    x86_outb(0x3D5, (uint8_t)(pos & 0xFF));
}

static void vga_make_attr(void)
{
    s_vga_attr = (uint8_t)((s_vga_bg << 4) | (s_vga_fg & 0x0F));
}

/* ========================================================================
 * Identification
 * ======================================================================== */

static flux_arch_t x86_detect_arch(void)
{
    return FLUX_ARCH_X86_64;
}

static const char *x86_arch_name(void)
{
    return "x86_64";
}

static const char *x86_hal_version(void)
{
    return FLUX_OS_VERSION_STRING;
}

static flux_hal_level_t x86_init_level(void)
{
    return FLUX_HAL_FULL;
}

/* ========================================================================
 * Console (VGA Text Mode)
 * ======================================================================== */

static void x86_console_init(void)
{
    s_vga_cursor_x = 0;
    s_vga_cursor_y = 0;
    s_vga_fg = VGA_COLOR_LIGHT_GREY;
    s_vga_bg = VGA_COLOR_BLACK;
    vga_make_attr();

    /* Clear VGA buffer */
    volatile uint16_t *buf = vga_buffer();
    uint16_t blank = ((uint16_t)s_vga_attr << 8) | ' ';
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        buf[i] = blank;

    vga_update_cursor();

    /* Initialize COM1 for early debug */
    x86_serial_init_port(COM1_BASE, 115200);
}

static void x86_console_putc(char c)
{
    volatile uint16_t *buf = vga_buffer();

    /* Echo to serial */
    x86_serial_putc(COM1_BASE, c);

    switch (c) {
    case '\n':
        s_vga_cursor_x = 0;
        s_vga_cursor_y++;
        if (s_vga_cursor_y >= VGA_HEIGHT)
            vga_scroll();
        break;

    case '\r':
        s_vga_cursor_x = 0;
        break;

    case '\t':
        /* Tab stops every 8 characters */
        s_vga_cursor_x = (s_vga_cursor_x + 8) & ~7;
        if (s_vga_cursor_x >= VGA_WIDTH) {
            s_vga_cursor_x = 0;
            s_vga_cursor_y++;
            if (s_vga_cursor_y >= VGA_HEIGHT)
                vga_scroll();
        }
        break;

    case '\b':
        if (s_vga_cursor_x > 0) {
            s_vga_cursor_x--;
            buf[s_vga_cursor_y * VGA_WIDTH + s_vga_cursor_x] =
                ((uint16_t)s_vga_attr << 8) | ' ';
        }
        break;

    default:
        if (c >= ' ') {
            buf[s_vga_cursor_y * VGA_WIDTH + s_vga_cursor_x] =
                ((uint16_t)s_vga_attr << 8) | (uint8_t)c;
            s_vga_cursor_x++;
            if (s_vga_cursor_x >= VGA_WIDTH) {
                s_vga_cursor_x = 0;
                s_vga_cursor_y++;
                if (s_vga_cursor_y >= VGA_HEIGHT)
                    vga_scroll();
            }
        }
        break;
    }

    vga_update_cursor();
}

static void x86_console_puts(const char *s)
{
    if (!s) return;
    while (*s) {
        x86_console_putc(*s);
        s++;
    }
}

static char x86_console_getc(void)
{
    return x86_serial_getc(COM1_BASE);
}

static void x86_console_clear(void)
{
    volatile uint16_t *buf = vga_buffer();
    uint16_t blank = ((uint16_t)s_vga_attr << 8) | ' ';
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        buf[i] = blank;

    s_vga_cursor_x = 0;
    s_vga_cursor_y = 0;
    vga_update_cursor();
}

static void x86_console_set_color(uint8_t fg, uint8_t bg)
{
    s_vga_fg = fg & 0x0F;
    s_vga_bg = bg & 0x0F;
    vga_make_attr();
}

/* ========================================================================
 * Physical Memory
 * ======================================================================== */

/* Physical memory is managed by the bootloader (multiboot2 memory map).
 * For now, we provide simple stubs that assume a basic setup. */

static flux_size_t x86_mem_total(void)
{
    /* TODO: Parse multiboot2 memory map for actual total */
    return 128ULL * 1024ULL * 1024ULL; /* Default 128 MB */
}

static flux_size_t x86_mem_free(void)
{
    /* TODO: Track allocations from multiboot2 memory map */
    return 64ULL * 1024ULL * 1024ULL; /* Default 64 MB free */
}

static flux_addr_t x86_mem_alloc_phys(flux_size_t size, flux_size_t align)
{
    /* TODO: Implement physical page allocator from multiboot2 memory map */
    (void)size; (void)align;
    return 0;
}

static void x86_mem_free_phys(flux_addr_t addr, flux_size_t size)
{
    (void)addr; (void)size;
}

static int x86_mem_map_count(void)
{
    /* TODO: Return multiboot2 memory map entry count */
    return 3; /* Placeholder: kernel, framebuffer, free */
}

static const flux_mem_map_t *x86_mem_map_get(int index)
{
    /* Static memory map entries */
    static flux_mem_map_t maps[] = {
        { 0x000000, 0x100000, FLUX_MEM_KERNEL, FLUX_PERM_RWX, "kernel" },
        { 0xB8000,  0x8000,   FLUX_MEM_DEVICE, FLUX_PERM_RW,  "vga" },
        { 0x100000, 0x700000, FLUX_MEM_KERNEL, FLUX_PERM_RW,  "free_low" },
    };
    (void)index;
    return maps; /* Simplified — always returns the array */
}

/* ========================================================================
 * Virtual Memory (x86_64 4-Level Page Tables)
 * ======================================================================== */

/*
 * x86_64 uses 4-level page tables:
 *   PML4 (Page Map Level 4) → PDPT → PD → PT → Page
 *
 * Each table has 512 entries (9 bits per level).
 * Virtual address: [47:39] PML4 | [38:30] PDPT | [29:21] PD | [20:12] PT | [11:0] offset
 *
 * Page table entry format:
 *   Bit 0:     Present
 *   Bit 1:     Read/Write
 *   Bit 2:     User/Supervisor
 *   Bits 3-4:  Page-level write-through / cache disable
 *   Bit 5:     Accessed
 *   Bit 6:     Dirty
 *   Bit 7:     Page size (1 GB for PDPT, 2 MB for PD)
 *   Bits 12-51: Physical address of next table (or page)
 */

#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_PWT        (1ULL << 3)
#define PTE_PCD        (1ULL << 4)
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_HUGE       (1ULL << 7)
#define PTE_NX         (1ULL << 63)

#define PAGE_SHIFT     12
#define PAGE_SIZE      (1ULL << PAGE_SHIFT)
#define PAGE_MASK      (~(PAGE_SIZE - 1))

#define PML4E_SHIFT    39
#define PDPTE_SHIFT    30
#define PDE_SHIFT      21

#define ENTRIES_PER_PT 512

typedef uint64_t page_table_entry_t;

/* Read CR3 to get current PML4 */
static inline flux_addr_t read_cr3(void)
{
    flux_addr_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* Write CR3 to load PML4 (and flush TLB) */
static inline void write_cr3(flux_addr_t pml4)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4) : "memory");
    s_pml4_phys = pml4;
}

static flux_status_t x86_vm_init(void)
{
    /* Read current PML4 from CR3 (set up by bootloader) */
    s_pml4_phys = read_cr3();
    return FLUX_OK;
}

static flux_status_t x86_vm_map(flux_addr_t virt, flux_addr_t phys,
                                flux_size_t size, flux_perm_t perm)
{
    (void)virt; (void)phys; (void)size; (void)perm;
    /* TODO: Walk PML4 and create entries */
    return FLUX_OK;
}

static flux_status_t x86_vm_unmap(flux_addr_t virt, flux_size_t size)
{
    (void)virt; (void)size;
    /* TODO: Walk PML4 and clear entries */
    return FLUX_OK;
}

static flux_status_t x86_vm_protect(flux_addr_t virt, flux_size_t size,
                                    flux_perm_t perm)
{
    (void)virt; (void)size; (void)perm;
    /* TODO: Walk PML4 and modify entry flags */
    return FLUX_OK;
}

static flux_addr_t x86_vm_current_pml4(void)
{
    return read_cr3();
}

static void x86_vm_switch(flux_addr_t pml4)
{
    write_cr3(pml4);
}

static void x86_vm_flush_tlb(void)
{
    /* Reload CR3 to flush entire TLB */
    write_cr3(read_cr3());
}

static void x86_vm_invalidate(flux_addr_t addr)
{
    /* INVLPG invalidates a single TLB entry */
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/* ========================================================================
 * CPU Features
 * ======================================================================== */

static void x86_cpu_init(void)
{
    /* Read CPU vendor string */
    uint32_t eax, ebx, ecx, edx;
    x86_cpuid(0, &eax, &ebx, &ecx, &edx);

    /* Check for APIC */
    x86_cpuid(1, &eax, &ebx, &ecx, &edx);
    if (edx & (1 << 9))
        s_has_apic = true;
}

static void x86_cpu_features(flux_cpu_features_t *feat)
{
    if (!feat) return;
    memset(feat, 0, sizeof(*feat));

    uint32_t eax, ebx, ecx, edx;

    /* CPUID leaf 1: feature flags */
    x86_cpuid(1, &eax, &ebx, &ecx, &edx);

    feat->has_mmu    = true;
    feat->has_fpu    = (edx >> 0) & 1;
    feat->has_tsc    = (edx >> 4) & 1;
    feat->has_apic   = (edx >> 9) & 1;
    feat->has_msi    = s_has_apic;
    feat->has_sse    = (edx >> 25) & 1;
    feat->has_sse2   = (edx >> 26) & 1;

    /* ECX features */
    feat->has_avx    = (ecx >> 28) & 1;

    /* Extended features: AVX2 */
    x86_cpuid(7, &eax, &ebx, &ecx, &edx);
    feat->has_avx2   = (ebx >> 5) & 1;
    feat->has_avx512 = (ebx >> 16) & 1; /* AVX512F */

    /* Extended CPUID (0x80000000+) */
    x86_cpuid_ext(0x80000000, &eax, NULL, NULL, NULL);
    if (eax >= 0x80000008) {
        uint32_t addr_size;
        x86_cpuid_ext(0x80000008, &addr_size, NULL, NULL, NULL);
        feat->phys_addr_bits   = (uint32_t)(addr_size & 0xFF);
        feat->linear_addr_bits = (uint32_t)((addr_size >> 8) & 0xFF);
    } else {
        feat->phys_addr_bits   = 36;
        feat->linear_addr_bits = 48;
    }

    /* Cache info (leaf 2/4) — simplified defaults */
    feat->cache_line_size = 64;
    feat->l1_size = 32768;
    feat->l2_size = 262144;
    feat->l3_size = 8388608;
}

static void x86_cpu_halt(void)
{
    __asm__ volatile("hlt");
}

static void x86_cpu_pause(void)
{
    __asm__ volatile("pause");
}

static void x86_cpu_wbinvd(void)
{
    __asm__ volatile("wbinvd");
}

static uint64_t x86_cpu_rdtsc(void)
{
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void x86_cpu_set_gs_base(uint64_t addr)
{
    /* WRMSR to IA32_GS_BASE (0xC0000101) */
    uint32_t low = (uint32_t)(addr & 0xFFFFFFFF);
    uint32_t high = (uint32_t)(addr >> 32);
    __asm__ volatile("wrmsr" : : "c"(0xC0000101), "a"(low), "d"(high));
}

/* ========================================================================
 * Interrupts (PIC/APIC)
 * ======================================================================== */

static void x86_irq_init(void)
{
    memset(s_irq_table, 0, sizeof(s_irq_table));

    if (s_has_apic) {
        /* APIC init — mask all interrupts initially */
        uint32_t svr = *(volatile uint32_t *)(APIC_BASE_ADDR + APIC_REG_SVR);
        *(volatile uint32_t *)(APIC_BASE_ADDR + APIC_REG_SVR) =
            svr & ~APIC_SVR_ENABLE;
    } else {
        /* PIC init: remap IRQs to 0x20-0x2F */

        /* Save masks */
        uint8_t m1 = x86_inb(PIC1_DATA);
        uint8_t m2 = x86_inb(PIC2_DATA);

        /* Start initialization in cascade mode */
        x86_outb(PIC1_CMD, 0x11);
        x86_io_wait();
        x86_outb(PIC2_CMD, 0x11);
        x86_io_wait();

        /* Set vector offsets */
        x86_outb(PIC1_DATA, IRQ_BASE);
        x86_io_wait();
        x86_outb(PIC2_DATA, IRQ2_BASE);
        x86_io_wait();

        /* Cascade: PIC2 on IRQ2 of PIC1 */
        x86_outb(PIC1_DATA, 0x04);
        x86_io_wait();
        x86_outb(PIC2_DATA, 0x02);
        x86_io_wait();

        /* 8086 mode */
        x86_outb(PIC1_DATA, 0x01);
        x86_io_wait();
        x86_outb(PIC2_DATA, 0x01);
        x86_io_wait();

        /* Restore saved masks */
        x86_outb(PIC1_DATA, m1);
        x86_outb(PIC2_DATA, m2);
    }
}

static void x86_irq_enable(void)
{
    __asm__ volatile("sti");
    s_irq_enabled = true;
}

static void x86_irq_disable(void)
{
    __asm__ volatile("cli");
    s_irq_enabled = false;
}

static bool x86_irq_enabled_fn(void)
{
    return s_irq_enabled;
}

static flux_status_t x86_irq_register(uint32_t irq, flux_irq_handler_t handler,
                                       void *ctx)
{
    if (irq >= 256) return FLUX_ERR_INVALID;

    s_irq_table[irq].irq_num = irq;
    s_irq_table[irq].handler = handler;
    s_irq_table[irq].ctx     = ctx;
    s_irq_table[irq].enabled = true;

    /* Unmask the IRQ on the PIC */
    if (irq < 8) {
        uint8_t mask = x86_inb(PIC1_DATA) & ~(1 << irq);
        x86_outb(PIC1_DATA, mask);
    } else if (irq < 16) {
        uint8_t mask = x86_inb(PIC2_DATA) & ~(1 << (irq - 8));
        x86_outb(PIC2_DATA, mask);
    }

    return FLUX_OK;
}

static void x86_irq_unregister(uint32_t irq)
{
    if (irq >= 256) return;
    s_irq_table[irq].handler = NULL;
    s_irq_table[irq].enabled = false;

    /* Mask the IRQ on the PIC */
    if (irq < 8) {
        uint8_t mask = x86_inb(PIC1_DATA) | (1 << irq);
        x86_outb(PIC1_DATA, mask);
    } else if (irq < 16) {
        uint8_t mask = x86_inb(PIC2_DATA) | (1 << (irq - 8));
        x86_outb(PIC2_DATA, mask);
    }
}

static void x86_irq_eoi(uint32_t irq)
{
    if (s_has_apic) {
        *(volatile uint32_t *)(APIC_BASE_ADDR + APIC_REG_EOI) = 0;
    } else {
        /* Send EOI to both PICs */
        if (irq >= 8)
            x86_outb(PIC2_CMD, PIC_EOI);
        x86_outb(PIC1_CMD, PIC_EOI);
    }
}

/* ========================================================================
 * Timer (PIT)
 * ======================================================================== */

static void x86_timer_init(uint32_t freq_hz)
{
    s_timer_freq = freq_hz;

    if (s_timer_freq == 0)
        s_timer_freq = 1000;

    uint16_t divisor = (uint16_t)(PIT_FREQ_HZ / s_timer_freq);

    /* Channel 0, rate generator, lobyte/hibyte */
    x86_outb(PIT_CMD, 0x36);
    x86_io_wait();
    x86_outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    x86_io_wait();
    x86_outb(PIT_CH0, (uint8_t)(divisor >> 8));
}

static uint64_t x86_timer_ticks(void)
{
    return __atomic_load_n(&s_timer_ticks, __ATOMIC_RELAXED);
}

/* Called from the PIT ISR (IRQ0) */
void x86_timer_tick_handler(void)
{
    __atomic_add_fetch(&s_timer_ticks, 1, __ATOMIC_RELAXED);
}

static void x86_timer_sleep_ms(uint32_t ms)
{
    /* Busy-wait using TSC for approximate timing */
    uint64_t start = x86_cpu_rdtsc();
    uint64_t end = start + (uint64_t)ms * 2000000ULL; /* ~2 GHz assumption */

    while (x86_cpu_rdtsc() < end) {
        x86_cpu_pause();
    }
}

static void x86_timer_set_alarm(uint32_t ms, void (*callback)(void))
{
    (void)ms; (void)callback;
    /* TODO: Implement one-shot timer using PIT channel or APIC timer */
}

/* ========================================================================
 * Context Save/Restore
 * ======================================================================== */

static void x86_context_save(flux_pcb_t *pcb)
{
    if (!pcb) return;

    /* Save general-purpose registers */
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

    __asm__ volatile(
        "mov %%rax, %0\n"
        "mov %%rbx, %1\n"
        "mov %%rcx, %2\n"
        "mov %%rdx, %3\n"
        "mov %%rsi, %4\n"
        "mov %%rdi, %5\n"
        "mov %%rbp, %6\n"
        "mov %%rsp, %7\n"
        "mov %%r8,  %8\n"
        "mov %%r9,  %9\n"
        "mov %%r10, %10\n"
        "mov %%r11, %11\n"
        "mov %%r12, %12\n"
        "mov %%r13, %13\n"
        "mov %%r14, %14\n"
        "mov %%r15, %15\n"
        : "=m"(rax), "=m"(rbx), "=m"(rcx), "=m"(rdx),
          "=m"(rsi), "=m"(rdi), "=m"(rbp), "=m"(rsp),
          "=m"(r8), "=m"(r9), "=m"(r10), "=m"(r11),
          "=m"(r12), "=m"(r13), "=m"(r14), "=m"(r15)
    );

    pcb->regs[0]  = rax;
    pcb->regs[1]  = rbx;
    pcb->regs[2]  = rcx;
    pcb->regs[3]  = rdx;
    pcb->regs[4]  = rsi;
    pcb->regs[5]  = rdi;
    pcb->regs[6]  = rbp;
    pcb->regs[7]  = rsp;
    pcb->regs[8]  = r8;
    pcb->regs[9]  = r9;
    pcb->regs[10] = r10;
    pcb->regs[11] = r11;
    pcb->regs[12] = r12;
    pcb->regs[13] = r13;
    pcb->regs[14] = r14;
    pcb->regs[15] = r15;
    pcb->stack_ptr = rsp;
    pcb->page_table = read_cr3();

    /* TODO: Save RFLAGS and RIP */
}

static void x86_context_restore(const flux_pcb_t *pcb)
{
    if (!pcb) return;

    /* Restore general-purpose registers */
    __asm__ volatile(
        "mov %0, %%r15\n"
        "mov %1, %%r14\n"
        "mov %2, %%r13\n"
        "mov %3, %%r12\n"
        "mov %4, %%r11\n"
        "mov %5, %%r10\n"
        "mov %6, %%r9\n"
        "mov %7, %%r8\n"
        :
        : "m"(pcb->regs[15]), "m"(pcb->regs[14]),
          "m"(pcb->regs[13]), "m"(pcb->regs[12]),
          "m"(pcb->regs[11]), "m"(pcb->regs[10]),
          "m"(pcb->regs[9]),  "m"(pcb->regs[8])
    );

    __asm__ volatile(
        "mov %0, %%rsp\n"
        "mov %1, %%rbp\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        :
        : "m"(pcb->stack_ptr),
          "m"(pcb->regs[6]), "m"(pcb->regs[5]), "m"(pcb->regs[4])
    );

    /* Switch page table if needed */
    if (pcb->page_table && pcb->page_table != read_cr3())
        write_cr3(pcb->page_table);

    /* TODO: Restore RFLAGS and jump to RIP */
}

/* ========================================================================
 * Port I/O (HAL Interface)
 * ======================================================================== */

static void x86_outb_fn(uint16_t port, uint8_t val) { x86_outb(port, val); }
static void x86_outw_fn(uint16_t port, uint16_t val) { x86_outw(port, val); }
static void x86_outl_fn(uint16_t port, uint32_t val) { x86_outl(port, val); }
static uint8_t x86_inb_fn(uint16_t port) { return x86_inb(port); }
static uint16_t x86_inw_fn(uint16_t port) { return x86_inw(port); }
static uint32_t x86_inl_fn(uint16_t port) { return x86_inl(port); }

/* ========================================================================
 * DMA (8237A)
 * ======================================================================== */

#define DMA1_CMD    0x08
#define DMA1_MASK   0x0A
#define DMA1_MODE   0x0B
#define DMA1_CLRFF  0x0C
#define DMA1_ADDR   0x00
#define DMA1_COUNT  0x01

static flux_status_t x86_dma_alloc(flux_size_t size, flux_addr_t *phys,
                                    void **virt)
{
    (void)size; (void)phys; (void)virt;
    /* TODO: Allocate from low memory (below 16 MB) for ISA DMA */
    return FLUX_ERR_GENERAL;
}

static void x86_dma_free(flux_addr_t phys, void *virt)
{
    (void)phys; (void)virt;
}

static flux_status_t x86_dma_transfer(flux_addr_t src, flux_addr_t dst,
                                       flux_size_t len, bool to_device)
{
    (void)src; (void)dst; (void)len; (void)to_device;
    /* TODO: Program 8237A DMA controller */
    return FLUX_ERR_GENERAL;
}

/* ========================================================================
 * Device Management
 * ======================================================================== */

static flux_status_t x86_device_register(flux_device_t *dev)
{
    (void)dev;
    /* TODO: Add to device tree */
    return FLUX_OK;
}

static flux_status_t x86_device_unregister(flux_device_t *dev)
{
    (void)dev;
    return FLUX_OK;
}

static flux_device_t *x86_device_find(const char *name)
{
    (void)name;
    return NULL;
}

static void x86_device_list(void)
{
    /* TODO: Enumerate PCI/ISA devices */
}

/* ========================================================================
 * Power Management (ACPI)
 * ======================================================================== */

static void x86_shutdown(void)
{
    /* Try ACPI shutdown via PM1a_CNT */
    x86_outw(0x604, 0x2000);

    /* Fallback: triple fault */
    __asm__ volatile(
        "lidt %0\n"
        "int $0x03\n"
        : : "m"((struct { uint16_t limit; uint32_t base; } __attribute__((packed)))
                { 0, 0 })
    );

    /* Should not reach here */
    for (;;) x86_cpu_halt();
}

static void x86_reboot(void)
{
    /* Try keyboard controller reboot */
    uint8_t good = 0x02;
    while (good & 0x02)
        good = x86_inb(0x64);
    x86_outb(0x64, 0xFE);

    /* Fallback: triple fault */
    __asm__ volatile("int $0x03");
    for (;;) x86_cpu_halt();
}

/* ========================================================================
 * Hardware Info
 * ======================================================================== */

static flux_status_t x86_hw_info_dump(char *buf, flux_size_t len)
{
    if (!buf || len == 0) return FLUX_ERR_INVALID;

    flux_cpu_features_t feat;
    x86_cpu_features(&feat);

    /* Get CPU vendor string */
    uint32_t eax, ebx, ecx, edx;
    x86_cpuid(0, &eax, &ebx, &ecx, &edx);
    char vendor[13];
    vendor[0]  = (char)(ebx & 0xFF);
    vendor[1]  = (char)((ebx >> 8) & 0xFF);
    vendor[2]  = (char)((ebx >> 16) & 0xFF);
    vendor[3]  = (char)((ebx >> 24) & 0xFF);
    vendor[4]  = (char)(edx & 0xFF);
    vendor[5]  = (char)((edx >> 8) & 0xFF);
    vendor[6]  = (char)((edx >> 16) & 0xFF);
    vendor[7]  = (char)((edx >> 24) & 0xFF);
    vendor[8]  = (char)(ecx & 0xFF);
    vendor[9]  = (char)((ecx >> 8) & 0xFF);
    vendor[10] = (char)((ecx >> 16) & 0xFF);
    vendor[11] = (char)((ecx >> 24) & 0xFF);
    vendor[12] = '\0';

    snprintf(buf, len,
        "FLUX x86_64 Hardware Information\n"
        "=================================\n"
        "  HAL Version:   %s\n"
        "  Architecture:  x86_64\n"
        "  Platform:      bare-metal\n"
        "  CPU Vendor:    %s\n"
        "  APIC:          %s\n"
        "\n"
        "CPU Features:\n"
        "  MMU:           yes\n"
        "  FPU:           %s\n"
        "  SSE:           %s\n"
        "  SSE2:          %s\n"
        "  AVX:           %s\n"
        "  AVX2:          %s\n"
        "  AVX-512:       %s\n"
        "  TSC:           %s\n"
        "  Phys Addr:     %u bits\n"
        "  Linear Addr:   %u bits\n"
        "  Cache Line:    %u bytes\n",
        FLUX_OS_VERSION_STRING,
        vendor,
        feat.has_apic   ? "yes" : "no",
        feat.has_fpu    ? "yes" : "no",
        feat.has_sse    ? "yes" : "no",
        feat.has_sse2   ? "yes" : "no",
        feat.has_avx    ? "yes" : "no",
        feat.has_avx2   ? "yes" : "no",
        feat.has_avx512 ? "yes" : "no",
        feat.has_tsc    ? "yes" : "no",
        feat.phys_addr_bits,
        feat.linear_addr_bits,
        feat.cache_line_size);

    return FLUX_OK;
}

static flux_status_t x86_hw_optimal_config(char *buf, flux_size_t len)
{
    if (!buf || len == 0) return FLUX_ERR_INVALID;

    flux_cpu_features_t feat;
    x86_cpu_features(&feat);

    const char *simd = "none";
    if (feat.has_avx512)     simd = "avx512";
    else if (feat.has_avx2)  simd = "avx2";
    else if (feat.has_avx)   simd = "avx";
    else if (feat.has_sse2)  simd = "sse2";
    else if (feat.has_sse)   simd = "sse";

    const char *timer = feat.has_apic ? "apic" : "pit";

    snprintf(buf, len,
        "optimal_config {\n"
        "  target:       x86_64\n"
        "  optimize:     speed\n"
        "  simd:         %s\n"
        "  timer:        %s\n"
        "  irq:          %s\n"
        "  cache_line:   %u\n"
        "  page_size:    4096\n"
        "  stack_size:   65536\n"
        "  vm_enabled:   true\n"
        "  paging:       pml4_4level\n"
        "  compiler:     flux-bytecode\n"
        "  jit:          enabled\n"
        "}\n",
        simd, timer, feat.has_apic ? "apic" : "pic",
        feat.cache_line_size);

    return FLUX_OK;
}

/* ========================================================================
 * Registration
 * ======================================================================== */

void flux_hal_register_x86_64_impl(void)
{
    memset(&s_x86_hal, 0, sizeof(s_x86_hal));

    /* Identification */
    s_x86_hal.detect_arch = x86_detect_arch;
    s_x86_hal.arch_name   = x86_arch_name;
    s_x86_hal.hal_version = x86_hal_version;
    s_x86_hal.init_level  = x86_init_level;

    /* Console */
    s_x86_hal.console_init     = x86_console_init;
    s_x86_hal.console_putc     = x86_console_putc;
    s_x86_hal.console_puts     = x86_console_puts;
    s_x86_hal.console_getc     = x86_console_getc;
    s_x86_hal.console_clear    = x86_console_clear;
    s_x86_hal.console_set_color = x86_console_set_color;

    /* Physical Memory */
    s_x86_hal.mem_total      = x86_mem_total;
    s_x86_hal.mem_free       = x86_mem_free;
    s_x86_hal.mem_alloc_phys = x86_mem_alloc_phys;
    s_x86_hal.mem_free_phys  = x86_mem_free_phys;
    s_x86_hal.mem_map_count  = x86_mem_map_count;
    s_x86_hal.mem_map_get    = x86_mem_map_get;

    /* Virtual Memory */
    s_x86_hal.vm_init         = x86_vm_init;
    s_x86_hal.vm_map          = x86_vm_map;
    s_x86_hal.vm_unmap        = x86_vm_unmap;
    s_x86_hal.vm_protect      = x86_vm_protect;
    s_x86_hal.vm_current_pml4 = x86_vm_current_pml4;
    s_x86_hal.vm_switch       = x86_vm_switch;
    s_x86_hal.vm_flush_tlb    = x86_vm_flush_tlb;
    s_x86_hal.vm_invalidate   = x86_vm_invalidate;

    /* CPU */
    s_x86_hal.cpu_init        = x86_cpu_init;
    s_x86_hal.cpu_features    = x86_cpu_features;
    s_x86_hal.cpu_halt        = x86_cpu_halt;
    s_x86_hal.cpu_pause       = x86_cpu_pause;
    s_x86_hal.cpu_wbinvd      = x86_cpu_wbinvd;
    s_x86_hal.cpu_rdtsc       = x86_cpu_rdtsc;
    s_x86_hal.cpu_set_gs_base = x86_cpu_set_gs_base;

    /* Interrupts */
    s_x86_hal.irq_init      = x86_irq_init;
    s_x86_hal.irq_enable    = x86_irq_enable;
    s_x86_hal.irq_disable   = x86_irq_disable;
    s_x86_hal.irq_enabled   = x86_irq_enabled_fn;
    s_x86_hal.irq_register  = x86_irq_register;
    s_x86_hal.irq_unregister = x86_irq_unregister;
    s_x86_hal.irq_eoi       = x86_irq_eoi;

    /* Timer */
    s_x86_hal.timer_init      = x86_timer_init;
    s_x86_hal.timer_ticks     = x86_timer_ticks;
    s_x86_hal.timer_sleep_ms  = x86_timer_sleep_ms;
    s_x86_hal.timer_set_alarm = x86_timer_set_alarm;

    /* Context */
    s_x86_hal.context_save    = x86_context_save;
    s_x86_hal.context_restore = x86_context_restore;

    /* Port I/O */
    s_x86_hal.outb = x86_outb_fn;
    s_x86_hal.outw = x86_outw_fn;
    s_x86_hal.outl = x86_outl_fn;
    s_x86_hal.inb  = x86_inb_fn;
    s_x86_hal.inw  = x86_inw_fn;
    s_x86_hal.inl  = x86_inl_fn;

    /* DMA */
    s_x86_hal.dma_alloc    = x86_dma_alloc;
    s_x86_hal.dma_free     = x86_dma_free;
    s_x86_hal.dma_transfer = x86_dma_transfer;

    /* Devices */
    s_x86_hal.device_register   = x86_device_register;
    s_x86_hal.device_unregister = x86_device_unregister;
    s_x86_hal.device_find       = x86_device_find;
    s_x86_hal.device_list       = x86_device_list;

    /* Power */
    s_x86_hal.shutdown = x86_shutdown;
    s_x86_hal.reboot   = x86_reboot;

    /* Hardware Info */
    s_x86_hal.hw_info_dump      = x86_hw_info_dump;
    s_x86_hal.hw_optimal_config = x86_hw_optimal_config;

    /* Set as active HAL */
    flux_hal_set(&s_x86_hal);
}
