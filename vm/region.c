/*
 * FLUX OS — Memory Region Management
 *
 * Memory regions provide isolated, sandboxed memory spaces for the FLUX VM.
 * Each region has:
 *   - A unique ID (0 to FLUX_REGION_MAX-1)
 *   - A name (for debugging and identification)
 *   - A size in bytes
 *   - A data buffer (allocated on the heap)
 *   - Read-only flag (writes to readonly regions trap)
 *   - Owner PID (for capability-based access control)
 *   - Active flag (inactive regions cannot be accessed)
 *
 * The active region is used by LOAD/STORE instructions. Regions can be
 * switched at runtime using flux_vm_region_set_active().
 *
 * Bounds checking is performed on all memory operations. Out-of-bounds
 * accesses are silently ignored (return 0 for reads, no-op for writes).
 * This prevents VM crashes from malicious or buggy bytecode.
 *
 * Copyright (c) 2025 SuperInstance
 */

#include <flux/vm.h>
#include <flux/opcodes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/* Find a free region slot */
static int find_free_region(flux_vm_t *vm) {
    for (int i = 0; i < FLUX_REGION_MAX; i++) {
        if (!vm->regions[i].active) {
            return i;
        }
    }
    return -1; /* No free slots */
}

/* Validate region ID is within bounds and active */
static bool validate_region_id(flux_vm_t *vm, int region_id) {
    if (region_id < 0 || region_id >= FLUX_REGION_MAX) {
        return false;
    }
    return vm->regions[region_id].active;
}

/* ========================================================================
 * Region Creation
 *
 * Allocates a new memory region with the given name, size, and readonly flag.
 * Returns the region ID (0 to FLUX_REGION_MAX-1) on success, or -1 on failure.
 *
 * The region data buffer is zero-initialized. Region ID 0 is typically used
 * as the default/stack region.
 * ======================================================================== */

int flux_vm_region_create(flux_vm_t *vm, const char *name,
                          flux_size_t size, bool readonly) {
    if (vm == NULL) return -1;
    if (name == NULL) name = "unnamed";
    if (size == 0) size = 256; /* Minimum region size */

    /* Find a free slot */
    int rid = find_free_region(vm);
    if (rid < 0) {
        /* No free region slots */
        return -1;
    }

    /* Allocate data buffer */
    uint8_t *data = (uint8_t *)calloc(size, 1);
    if (data == NULL) {
        return -1; /* Out of memory */
    }

    /* Initialize region */
    flux_vm_region_t *reg = &vm->regions[rid];
    memset(reg, 0, sizeof(flux_vm_region_t));

    reg->data = data;
    reg->size = size;
    reg->base = 0; /* Will be set by REGION_MAP if needed */
    reg->active = true;
    reg->readonly = readonly;
    reg->owner = vm->owner_pid;

    /* Copy name (truncate if necessary) */
    size_t name_len = strlen(name);
    if (name_len >= sizeof(reg->name)) {
        name_len = sizeof(reg->name) - 1;
    }
    memcpy(reg->name, name, name_len);
    reg->name[name_len] = '\0';

    /* If this is the first region, make it active by default */
    if (vm->active_region < 0) {
        vm->active_region = rid;
    }

    return rid;
}

/* ========================================================================
 * Region Data Access
 *
 * Returns a pointer to the region's data buffer, or NULL if the region
 * ID is invalid or the region is inactive.
 *
 * IMPORTANT: The caller must perform bounds checking. Writing past the
 * region boundary will corrupt heap memory.
 * ======================================================================== */

void *flux_vm_region_data(flux_vm_t *vm, int region_id) {
    if (vm == NULL) return NULL;
    if (!validate_region_id(vm, region_id)) return NULL;
    return vm->regions[region_id].data;
}

/* ========================================================================
 * Region Destruction
 *
 * Frees the region's data buffer and marks the slot as inactive.
 * If the destroyed region was the active region, the active region
 * is set to -1 (no active region).
 * ======================================================================== */

flux_status_t flux_vm_region_destroy(flux_vm_t *vm, int region_id) {
    if (vm == NULL) return FLUX_ERR_INVALID;
    if (!validate_region_id(vm, region_id)) return FLUX_ERR_NOTFOUND;

    flux_vm_region_t *reg = &vm->regions[region_id];

    /* Free data buffer */
    if (reg->data != NULL) {
        free(reg->data);
        reg->data = NULL;
    }

    /* Mark as inactive */
    reg->active = false;
    reg->size = 0;
    reg->base = 0;
    reg->name[0] = '\0';

    /* Update active region if needed */
    if (vm->active_region == region_id) {
        vm->active_region = -1;

        /* Try to find another active region */
        for (int i = 0; i < FLUX_REGION_MAX; i++) {
            if (vm->regions[i].active) {
                vm->active_region = i;
                break;
            }
        }
    }

    return FLUX_OK;
}

/* ========================================================================
 * Region Switching
 *
 * Set the active memory region used by LOAD/STORE instructions.
 * ======================================================================== */

flux_status_t flux_vm_region_set_active(flux_vm_t *vm, int region_id) {
    if (vm == NULL) return FLUX_ERR_INVALID;
    if (!validate_region_id(vm, region_id)) return FLUX_ERR_NOTFOUND;

    vm->active_region = region_id;

    /* Update special registers */
    vm->regs[FLUX_REG_REGION_BASE] =
        (uint64_t)(uintptr_t)vm->regions[region_id].data;
    vm->regs[FLUX_REG_REGION_SIZE] =
        (uint64_t)vm->regions[region_id].size;

    return FLUX_OK;
}

/* ========================================================================
 * Region Query
 *
 * Get information about a region.
 * ==================================================================== */

flux_status_t flux_vm_region_info(flux_vm_t *vm, int region_id,
                                   char *name_buf, flux_size_t name_buf_len,
                                   flux_size_t *out_size, bool *out_readonly,
                                   bool *out_active) {
    if (vm == NULL) return FLUX_ERR_INVALID;
    if (region_id < 0 || region_id >= FLUX_REGION_MAX) return FLUX_ERR_NOTFOUND;

    flux_vm_region_t *reg = &vm->regions[region_id];

    if (name_buf != NULL && name_buf_len > 0) {
        size_t len = strlen(reg->name);
        if (len >= name_buf_len) len = name_buf_len - 1;
        memcpy(name_buf, reg->name, len);
        name_buf[len] = '\0';
    }

    if (out_size != NULL) *out_size = reg->size;
    if (out_readonly != NULL) *out_readonly = reg->readonly;
    if (out_active != NULL) *out_active = reg->active;

    return FLUX_OK;
}

/* ========================================================================
 * Region Read (bounds-checked)
 *
 * Read nbytes from the region at the given offset.
 * Returns the number of bytes actually read (may be less if near boundary).
 * ======================================================================== */

flux_size_t flux_vm_region_read(flux_vm_t *vm, int region_id,
                                flux_size_t offset, void *buf,
                                flux_size_t nbytes) {
    if (vm == NULL || buf == NULL || nbytes == 0) return 0;
    if (!validate_region_id(vm, region_id)) return 0;

    flux_vm_region_t *reg = &vm->regions[region_id];

    /* Bounds check */
    if (offset >= reg->size) return 0;

    flux_size_t available = reg->size - offset;
    flux_size_t to_read = (nbytes < available) ? nbytes : available;

    memcpy(buf, reg->data + offset, to_read);
    return to_read;
}

/* ========================================================================
 * Region Write (bounds-checked, respects readonly)
 *
 * Write nbytes to the region at the given offset.
 * Returns the number of bytes actually written.
 * ======================================================================== */

flux_size_t flux_vm_region_write(flux_vm_t *vm, int region_id,
                                 flux_size_t offset, const void *buf,
                                 flux_size_t nbytes) {
    if (vm == NULL || buf == NULL || nbytes == 0) return 0;
    if (!validate_region_id(vm, region_id)) return 0;

    flux_vm_region_t *reg = &vm->regions[region_id];

    /* Check readonly */
    if (reg->readonly) return 0;

    /* Bounds check */
    if (offset >= reg->size) return 0;

    flux_size_t available = reg->size - offset;
    flux_size_t to_write = (nbytes < available) ? nbytes : available;

    memcpy(reg->data + offset, buf, to_write);
    return to_write;
}

/* ========================================================================
 * Region Resize
 *
 * Resize a region's data buffer. If the new size is larger, new memory
 * is zero-initialized. If smaller, data is truncated.
 * ======================================================================== */

flux_status_t flux_vm_region_resize(flux_vm_t *vm, int region_id,
                                    flux_size_t new_size) {
    if (vm == NULL) return FLUX_ERR_INVALID;
    if (!validate_region_id(vm, region_id)) return FLUX_ERR_NOTFOUND;
    if (new_size == 0) new_size = 1;

    flux_vm_region_t *reg = &vm->regions[region_id];

    uint8_t *new_data = (uint8_t *)realloc(reg->data, new_size);
    if (new_data == NULL) return FLUX_ERR_NOMEM;

    /* Zero-initialize any new memory */
    if (new_size > reg->size) {
        memset(new_data + reg->size, 0, new_size - reg->size);
    }

    reg->data = new_data;
    reg->size = new_size;

    /* Update special registers if this is the active region */
    if (vm->active_region == region_id) {
        vm->regs[FLUX_REG_REGION_SIZE] = (uint64_t)new_size;
    }

    return FLUX_OK;
}

/* ========================================================================
 * Region Set Owner
 *
 * Change the owner PID of a region (for capability-based access control).
 * ======================================================================== */

flux_status_t flux_vm_region_set_owner(flux_vm_t *vm, int region_id,
                                       flux_pid_t owner) {
    if (vm == NULL) return FLUX_ERR_INVALID;
    if (!validate_region_id(vm, region_id)) return FLUX_ERR_NOTFOUND;

    vm->regions[region_id].owner = owner;
    return FLUX_OK;
}

/* ========================================================================
 * Region Set Readonly
 *
 * Change the readonly flag of an existing region.
 * ======================================================================== */

flux_status_t flux_vm_region_set_readonly(flux_vm_t *vm, int region_id,
                                          bool readonly) {
    if (vm == NULL) return FLUX_ERR_INVALID;
    if (!validate_region_id(vm, region_id)) return FLUX_ERR_NOTFOUND;

    vm->regions[region_id].readonly = readonly;
    return FLUX_OK;
}

/* ========================================================================
 * Region Copy
 *
 * Copy data from one region to another. Handles overlapping regions safely.
 * ======================================================================== */

flux_status_t flux_vm_region_copy(flux_vm_t *vm,
                                  int src_region_id, flux_size_t src_offset,
                                  int dst_region_id, flux_size_t dst_offset,
                                  flux_size_t nbytes) {
    if (vm == NULL) return FLUX_ERR_INVALID;
    if (!validate_region_id(vm, src_region_id)) return FLUX_ERR_NOTFOUND;
    if (!validate_region_id(vm, dst_region_id)) return FLUX_ERR_NOTFOUND;

    flux_vm_region_t *src_reg = &vm->regions[src_region_id];
    flux_vm_region_t *dst_reg = &vm->regions[dst_region_id];

    /* Check readonly on destination */
    if (dst_reg->readonly) return FLUX_ERR_DENIED;

    /* Bounds check source */
    if (src_offset >= src_reg->size) return FLUX_ERR_INVALID;
    flux_size_t src_avail = src_reg->size - src_offset;
    if (nbytes > src_avail) nbytes = src_avail;

    /* Bounds check destination */
    if (dst_offset >= dst_reg->size) return FLUX_ERR_INVALID;
    flux_size_t dst_avail = dst_reg->size - dst_offset;
    if (nbytes > dst_avail) nbytes = dst_avail;

    if (nbytes == 0) return FLUX_OK;

    /* Use memmove for overlapping safety within same region */
    memmove(dst_reg->data + dst_offset, src_reg->data + src_offset, nbytes);

    return FLUX_OK;
}

/* ========================================================================
 * Region Fill
 *
 * Fill a region with a byte value (like memset).
 * ======================================================================== */

flux_status_t flux_vm_region_fill(flux_vm_t *vm, int region_id,
                                  flux_size_t offset, flux_size_t nbytes,
                                  uint8_t value) {
    if (vm == NULL) return FLUX_ERR_INVALID;
    if (!validate_region_id(vm, region_id)) return FLUX_ERR_NOTFOUND;

    flux_vm_region_t *reg = &vm->regions[region_id];

    if (reg->readonly) return FLUX_ERR_DENIED;

    if (offset >= reg->size) return FLUX_ERR_INVALID;
    flux_size_t avail = reg->size - offset;
    if (nbytes > avail) nbytes = avail;

    memset(reg->data + offset, value, nbytes);
    return FLUX_OK;
}

/* ========================================================================
 * Region Count
 *
 * Returns the number of active regions.
 * ======================================================================== */

int flux_vm_region_count(flux_vm_t *vm) {
    if (vm == NULL) return 0;

    int count = 0;
    for (int i = 0; i < FLUX_REGION_MAX; i++) {
        if (vm->regions[i].active) {
            count++;
        }
    }
    return count;
}

/* ========================================================================
 * Region Dump
 *
 * Print region information for debugging.
 * ======================================================================== */

void flux_vm_region_dump(flux_vm_t *vm) {
    if (vm == NULL) return;

    printf("=== FLUX VM Memory Regions ===\n");
    printf("Active region: %d\n", vm->active_region);
    printf("Total slots: %d\n\n", FLUX_REGION_MAX);

    for (int i = 0; i < FLUX_REGION_MAX; i++) {
        flux_vm_region_t *reg = &vm->regions[i];
        if (!reg->active) continue;

        printf("  [%2d] %-16s  size=%8lu  %s  owner=%u  base=0x%016lX\n",
               i, reg->name,
               (unsigned long)reg->size,
               reg->readonly ? "RO" : "RW",
               reg->owner,
               (unsigned long)reg->base);

        /* Hex dump first 64 bytes */
        flux_size_t dump_len = (reg->size < 64) ? reg->size : 64;
        if (dump_len > 0 && reg->data != NULL) {
            printf("       Data: ");
            for (flux_size_t j = 0; j < dump_len; j++) {
                printf("%02X ", reg->data[j]);
                if ((j + 1) % 16 == 0 && j + 1 < dump_len) {
                    printf("\n             ");
                }
            }
            if (dump_len < reg->size) {
                printf("... (%lu more bytes)", (unsigned long)(reg->size - dump_len));
            }
            printf("\n");
        }
    }
}

/* ========================================================================
 * Region Get Active
 *
 * Returns the currently active region ID, or -1 if none.
 * ======================================================================== */

int flux_vm_region_get_active(flux_vm_t *vm) {
    if (vm == NULL) return -1;
    return vm->active_region;
}

/* ========================================================================
 * Region Size
 *
 * Returns the size of a specific region, or 0 if invalid.
 * ==================================================================== */

flux_size_t flux_vm_region_get_size(flux_vm_t *vm, int region_id) {
    if (vm == NULL) return 0;
    if (!validate_region_id(vm, region_id)) return 0;
    return vm->regions[region_id].size;
}

/* ========================================================================
 * Region Find by Name
 *
 * Search for a region by name. Returns region ID or -1 if not found.
 * ==================================================================== */

int flux_vm_region_find(flux_vm_t *vm, const char *name) {
    if (vm == NULL || name == NULL) return -1;

    for (int i = 0; i < FLUX_REGION_MAX; i++) {
        if (vm->regions[i].active && strcmp(vm->regions[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ========================================================================
 * Region Destroy All
 *
 * Destroy all active regions and reset the active region.
 * ==================================================================== */

flux_status_t flux_vm_region_destroy_all(flux_vm_t *vm) {
    if (vm == NULL) return FLUX_ERR_INVALID;

    for (int i = 0; i < FLUX_REGION_MAX; i++) {
        if (vm->regions[i].active) {
            if (vm->regions[i].data != NULL) {
                free(vm->regions[i].data);
                vm->regions[i].data = NULL;
            }
            vm->regions[i].active = false;
            vm->regions[i].size = 0;
            vm->regions[i].name[0] = '\0';
        }
    }

    vm->active_region = -1;
    return FLUX_OK;
}
