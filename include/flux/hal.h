/*
 * FLUX OS — Hardware Abstraction Layer
 *
 * The HAL provides a unified interface to all hardware. The FLUX OS kernel
 * never touches hardware directly — everything goes through these function
 * pointers. This is what makes FLUX "intelligently hardware agnostic":
 *
 *   - At boot, the HAL probes the actual hardware and selects the right backend
 *   - The kernel can query hardware capabilities and adapt its behavior
 *   - The self-compiler can generate code targeted to specific hardware
 *   - Backends can be swapped at runtime (hot-plug architecture)
 *
 * Supported architectures:
 *   - x86_64 (PC, servers, cloud VMs)
 *   - ARM64 (mobile, embedded, Apple Silicon)
 *   - RISC-V 64 (emerging embedded and HPC)
 *
 * The HAL also supports the concept of "virtual hardware" — when running
 * under QEMU or other emulators, the HAL can present a simplified hardware
 * model that the kernel's self-compiler can target more easily.
 */

#ifndef FLUX_HAL_H
#define FLUX_HAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "flux/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * HAL Initialization Levels
 * ======================================================================== */

typedef enum {
    FLUX_HAL_NONE     = 0,
    FLUX_HAL_PROBE    = 1,   /* Hardware detected, not configured */
    FLUX_HAL_READY    = 2,   /* Core hardware initialized */
    FLUX_HAL_DRIVERS  = 3,   /* Drivers loaded */
    FLUX_HAL_FULL     = 4,   /* All hardware operational */
} flux_hal_level_t;

/* ========================================================================
 * Architecture Identifier
 * ======================================================================== */

typedef enum {
    FLUX_ARCH_UNKNOWN = 0,
    FLUX_ARCH_X86_64  = 1,
    FLUX_ARCH_ARM64   = 2,
    FLUX_ARCH_RISCV64 = 3,
    FLUX_ARCH_WASM32  = 4,   /* WebAssembly backend */
    FLUX_ARCH_NATIVE  = 5,   /* Hosted mode (Linux/POSIX) */
} flux_arch_t;

/* ========================================================================
 * Memory Map Entry
 * ======================================================================== */

typedef struct {
    flux_addr_t  base;
    flux_size_t  size;
    flux_mem_type_t type;
    flux_perm_t  perms;
    const char  *name;
} flux_mem_map_t;

/* ========================================================================
 * CPU Feature Flags
 * ======================================================================== */

typedef struct {
    bool has_sse;
    bool has_sse2;
    bool has_avx;
    bool has_avx2;
    bool has_avx512;
    bool has_neon;          /* ARM NEON */
    bool has_rvv;           /* RISC-V Vector */
    bool has_mmu;
    bool has_fpu;
    bool has_tsc;           /* Time-stamp counter */
    bool has_apic;          /* Advanced PIC */
    bool has_msi;           /* Message-signaled interrupts */
    uint32_t phys_addr_bits;
    uint32_t linear_addr_bits;
    uint32_t cache_line_size;
    uint32_t l1_size;
    uint32_t l2_size;
    uint32_t l3_size;
} flux_cpu_features_t;

/* ========================================================================
 * HAL Device Interface (VTable Pattern)
 * ======================================================================== */

#define FLUX_DEV_NAME_MAX 32

typedef struct flux_device {
    char              name[FLUX_DEV_NAME_MAX];
    uint32_t          dev_id;
    flux_status_t (*init)(struct flux_device *dev);
    flux_status_t (*read)(struct flux_device *dev, void *buf, flux_size_t *len);
    flux_status_t (*write)(struct flux_device *dev, const void *buf, flux_size_t len);
    flux_status_t (*ioctl)(struct flux_device *dev, uint32_t cmd, void *arg);
    flux_status_t (*deinit)(struct flux_device *dev);
    void            *private_data;
} flux_device_t;

/* ========================================================================
 * HAL Interrupt Descriptor
 * ======================================================================== */

typedef void (*flux_irq_handler_t)(void *ctx, uint32_t irq_num);

typedef struct {
    uint32_t          irq_num;
    flux_irq_handler_t handler;
    void             *ctx;
    bool              enabled;
} flux_irq_t;

/* ========================================================================
 * HAL Interface — Function Table
 *
 * Every backend (x86_64, ARM64, RISC-V, native) must implement ALL of these.
 * The kernel selects the backend at boot and calls through these pointers.
 * ======================================================================== */

typedef struct {
    /* --- Identification --- */
    flux_arch_t   (*detect_arch)(void);
    const char   *(*arch_name)(void);
    const char   *(*hal_version)(void);
    flux_hal_level_t (*init_level)(void);

    /* --- Console / Display --- */
    void          (*console_init)(void);
    void          (*console_putc)(char c);
    void          (*console_puts)(const char *s);
    char          (*console_getc)(void);
    void          (*console_clear)(void);
    void          (*console_set_color)(uint8_t fg, uint8_t bg);

    /* --- Physical Memory --- */
    flux_size_t   (*mem_total)(void);
    flux_size_t   (*mem_free)(void);
    flux_addr_t   (*mem_alloc_phys)(flux_size_t size, flux_size_t align);
    void          (*mem_free_phys)(flux_addr_t addr, flux_size_t size);
    int           (*mem_map_count)(void);
    const flux_mem_map_t *(*mem_map_get)(int index);

    /* --- Virtual Memory --- */
    flux_status_t (*vm_init)(void);
    flux_status_t (*vm_map)(flux_addr_t virt, flux_addr_t phys, flux_size_t size, flux_perm_t perm);
    flux_status_t (*vm_unmap)(flux_addr_t virt, flux_size_t size);
    flux_status_t (*vm_protect)(flux_addr_t virt, flux_size_t size, flux_perm_t perm);
    flux_addr_t   (*vm_current_pml4)(void);
    void          (*vm_switch)(flux_addr_t pml4);
    void          (*vm_flush_tlb)(void);
    void          (*vm_invalidate)(flux_addr_t addr);

    /* --- CPU Features --- */
    void          (*cpu_init)(void);
    void          (*cpu_features)(flux_cpu_features_t *feat);
    void          (*cpu_halt)(void);
    void          (*cpu_pause)(void);
    void          (*cpu_wbinvd)(void);
    uint64_t      (*cpu_rdtsc)(void);
    void          (*cpu_set_gs_base)(uint64_t addr);

    /* --- Interrupts --- */
    void          (*irq_init)(void);
    void          (*irq_enable)(void);
    void          (*irq_disable)(void);
    bool          (*irq_enabled)(void);
    flux_status_t (*irq_register)(uint32_t irq, flux_irq_handler_t handler, void *ctx);
    void          (*irq_unregister)(uint32_t irq);
    void          (*irq_eoi)(uint32_t irq);

    /* --- Timer --- */
    void          (*timer_init)(uint32_t freq_hz);
    uint64_t      (*timer_ticks)(void);
    void          (*timer_sleep_ms)(uint32_t ms);
    void          (*timer_set_alarm)(uint32_t ms, void (*callback)(void));

    /* --- Interrupt Context Save/Restore --- */
    void          (*context_save)(flux_pcb_t *pcb);
    void          (*context_restore)(const flux_pcb_t *pcb);

    /* --- Port I/O (for x86) --- */
    void          (*outb)(uint16_t port, uint8_t val);
    void          (*outw)(uint16_t port, uint16_t val);
    void          (*outl)(uint16_t port, uint32_t val);
    uint8_t       (*inb)(uint16_t port);
    uint16_t      (*inw)(uint16_t port);
    uint32_t      (*inl)(uint16_t port);

    /* --- DMA --- */
    flux_status_t (*dma_alloc)(flux_size_t size, flux_addr_t *phys, void **virt);
    void          (*dma_free)(flux_addr_t phys, void *virt);
    flux_status_t (*dma_transfer)(flux_addr_t src, flux_addr_t dst, flux_size_t len, bool to_device);

    /* --- Device Management --- */
    flux_status_t (*device_register)(flux_device_t *dev);
    flux_status_t (*device_unregister)(flux_device_t *dev);
    flux_device_t *(*device_find)(const char *name);
    void          (*device_list)(void);

    /* --- Power Management --- */
    void          (*shutdown)(void);
    void          (*reboot)(void);

    /* --- Hardware Info for Self-Compiler --- */
    flux_status_t (*hw_info_dump)(char *buf, flux_size_t len);
    flux_status_t (*hw_optimal_config)(char *buf, flux_size_t len);
} flux_hal_t;

/* ========================================================================
 * HAL API
 * ======================================================================== */

/* Set the active HAL backend (called by arch-specific boot code) */
void              flux_hal_set(const flux_hal_t *hal);
const flux_hal_t *flux_hal_get(void);

/* Convenience wrappers */
flux_arch_t       flux_hal_detect_arch(void);
const char       *flux_hal_arch_name(void);
void              flux_hal_console_init(void);
void              flux_hal_putc(char c);
void              flux_hal_puts(const char *s);
void              flux_hal_clear(void);

/* Boot sequence */
flux_hal_level_t  flux_hal_boot(void);

/* Memory helpers */
flux_size_t       flux_hal_mem_total(void);
flux_addr_t       flux_hal_alloc_phys(flux_size_t size, flux_size_t align);

/* HAL backend registration */
void flux_hal_register_x86_64(void);
void flux_hal_register_arm64(void);
void flux_hal_register_riscv64(void);
void flux_hal_register_native(void);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_HAL_H */
