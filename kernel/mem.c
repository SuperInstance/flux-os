/*
 * FLUX OS — Memory Management Subsystem
 *
 * Implements a free-list memory allocator with type tagging. This is the
 * kernel's physical memory manager, designed to work without any external
 * heap allocator (no dependency on malloc/free).
 *
 * Design:
 *   - The kernel manages a contiguous heap region (obtained from HAL at boot)
 *   - Allocations are tracked via metadata headers prepended to each block
 *   - A free-list maintains available blocks, sorted by address
 *   - Coalescing: adjacent free blocks are merged on free()
 *   - Type tagging: each allocation records its flux_mem_type_t for auditing
 *   - Memory map tracking: every allocation is recorded in a flat map
 *   - Memory permissions are tracked per-region for capability enforcement
 *
 * In hosted mode (Linux), we use a static buffer as the simulated heap.
 * On bare metal, the HAL provides physical memory pages.
 */

#include "flux/kernel.h"
#include "flux/hal.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================
 * Configuration
 * ======================================================================== */

#ifndef FLUX_HEAP_SIZE
#define FLUX_HEAP_SIZE          (16 * 1024 * 1024)  /* 16 MB default heap */
#endif

#ifndef FLUX_MEM_ALIGN
#define FLUX_MEM_ALIGN          16     /* Minimum alignment for allocations */
#endif

#ifndef FLUX_MEM_MAX_MAP_ENTRIES
#define FLUX_MEM_MAX_MAP_ENTRIES  2048  /* Max tracked memory regions */
#endif

#ifndef FLUX_MEM_MAX_PERMISSIONS
#define FLUX_MEM_MAX_PERMISSIONS  256   /* Max permission entries */
#endif

/* ========================================================================
 * Allocation Metadata Header
 *
 * Every allocated block has this header prepended. The pointer returned
 * to the caller points to the data immediately after this header.
 * ======================================================================== */

typedef struct mem_block_s {
    uint32_t    magic;          /* Magic number for validation */
    uint32_t    flags;          /* Flags (e.g., ALLOC_USED) */
    flux_size_t size;           /* Usable size (excluding header) */
    flux_mem_type_t type;       /* Memory type tag */
    uint32_t    owner_pid;      /* PID of allocating process */
    struct mem_block_s *next;   /* Next block in free list (if free) */
    struct mem_block_s *prev;   /* Previous block in free list (if free) */
} mem_block_t;

#define MEM_BLOCK_MAGIC     0x464C5558  /* "FLUX" in ASCII */
#define MEM_BLOCK_USED      0x00000001  /* Block is in use */
#define MEM_BLOCK_FREE      0x00000002  /* Block is free */
#define MEM_BLOCK_GUARD     0x00000004  /* Guard bytes enabled */

/* ========================================================================
 * Memory Map Entry
 *
 * Tracks all allocations for auditing, debugging, and memory protection.
 * ======================================================================== */

typedef struct {
    flux_addr_t        base;           /* Start address */
    flux_size_t        size;           /* Size in bytes */
    flux_mem_type_t    type;           /* Memory type */
    flux_perm_t        perms;          /* Current permissions */
    uint32_t           owner_pid;      /* Owning process */
    bool               active;         /* Is this entry in use? */
    char               tag[32];        /* Optional tag for debugging */
} mem_map_entry_t;

/* ========================================================================
 * Memory Permission Entry
 *
 * Records explicit permission changes for regions.
 * ======================================================================== */

typedef struct {
    flux_addr_t        base;
    flux_size_t        size;
    flux_perm_t        perms;
    bool               active;
} mem_perm_entry_t;

/* ========================================================================
 * Static State — The Heap
 *
 * In a real kernel, this would be the physical memory region obtained
 * from the HAL. For the hosted/simulation build, we use a static buffer.
 * ======================================================================== */

/* The simulated heap */
static uint8_t s_heap[FLUX_HEAP_SIZE]
    __attribute__((aligned(FLUX_MEM_ALIGN)));

/* Free list head */
static mem_block_t *s_free_list = NULL;

/* Total heap statistics */
static flux_size_t s_total_heap = FLUX_HEAP_SIZE;
static flux_size_t s_used_heap = 0;
static flux_size_t s_free_heap = FLUX_HEAP_SIZE;
static uint32_t    s_alloc_count = 0;     /* Total allocations made */
static uint32_t    s_free_count = 0;      /* Total frees performed */
static uint32_t    s_alloc_failures = 0;  /* Out-of-memory count */

/* Memory map */
static mem_map_entry_t s_mem_map[FLUX_MEM_MAX_MAP_ENTRIES];
static uint32_t s_mem_map_count = 0;

/* Permission tracking */
static mem_perm_entry_t s_perm_table[FLUX_MEM_MAX_PERMISSIONS];
static uint32_t s_perm_count = 0;

/* Allocation sequence number for debugging */
static uint32_t s_alloc_seq = 0;

/* Mutex for thread safety (no-op in single-threaded kernel) */
static volatile int s_mem_lock = 0;

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/*
 * mem_lock / mem_unlock — Simple spinlock for memory operations.
 * In a single-threaded kernel these are no-ops, but they're included
 * for correctness when the kernel runs on SMP systems.
 */
static inline void mem_lock(void)
{
    while (__atomic_exchange_n(&s_mem_lock, 1, __ATOMIC_ACQUIRE)) {
        /* Spin wait */
        __asm__ volatile("pause" ::: "memory");
    }
}

static inline void mem_unlock(void)
{
    __atomic_store_n(&s_mem_lock, 0, __ATOMIC_RELEASE);
}

/*
 * mem_align_up — Align a value up to the nearest alignment boundary.
 */
static inline flux_size_t mem_align_up(flux_size_t val, flux_size_t align)
{
    return (val + align - 1) & ~(align - 1);
}

/*
 * mem_ptr_from_block — Get the user-visible pointer from a block header.
 */
static inline void *mem_ptr_from_block(mem_block_t *block)
{
    return (void *)((uint8_t *)block + sizeof(mem_block_t));
}

/*
 * mem_block_from_ptr — Get the block header from a user-visible pointer.
 */
static inline mem_block_t *mem_block_from_ptr(void *ptr)
{
    return (mem_block_t *)((uint8_t *)ptr - sizeof(mem_block_t));
}

/*
 * mem_block_total_size — Total size including the header.
 */
static inline flux_size_t mem_block_total_size(flux_size_t data_size)
{
    return mem_align_up(data_size + sizeof(mem_block_t), FLUX_MEM_ALIGN);
}

/*
 * mem_find_free_block — Search the free list for a block of at least
 * the requested size. Uses first-fit strategy.
 */
static mem_block_t *mem_find_free_block(flux_size_t size)
{
    mem_block_t *best = NULL;

    /* First-fit with slight preference for smaller blocks to reduce fragmentation */
    for (mem_block_t *block = s_free_list; block != NULL; block = block->next) {
        if ((block->flags & MEM_BLOCK_FREE) && block->size >= size) {
            if (!best || block->size < best->size) {
                best = block;
            }
        }
    }

    return best;
}

/*
 * mem_remove_from_free_list — Remove a block from the free list.
 */
static void mem_remove_from_free_list(mem_block_t *block)
{
    if (block->prev)
        block->prev->next = block->next;
    else
        s_free_list = block->next;

    if (block->next)
        block->next->prev = block->prev;

    block->next = NULL;
    block->prev = NULL;
}

/*
 * mem_add_to_free_list — Add a block to the free list, sorted by address.
 */
static void mem_add_to_free_list(mem_block_t *block)
{
    block->flags = MEM_BLOCK_FREE;
    block->next = NULL;
    block->prev = NULL;

    if (s_free_list == NULL) {
        s_free_list = block;
        return;
    }

    /* Insert sorted by address */
    mem_block_t *curr = s_free_list;
    mem_block_t *prev = NULL;

    while (curr && (uint8_t *)curr < (uint8_t *)block) {
        prev = curr;
        curr = curr->next;
    }

    if (prev) {
        prev->next = block;
        block->prev = prev;
    } else {
        s_free_list = block;
    }

    block->next = curr;
    if (curr)
        curr->prev = block;
}

/*
 * mem_coalesce — Merge adjacent free blocks.
 * Called after freeing a block to reduce fragmentation.
 */
static void mem_coalesce(void)
{
    bool merged = true;

    while (merged) {
        merged = false;

        for (mem_block_t *block = s_free_list; block != NULL; block = block->next) {
            if (!(block->flags & MEM_BLOCK_FREE))
                continue;

            /* Check if next block is adjacent and free */
            uint8_t *block_end = (uint8_t *)block + sizeof(mem_block_t) + block->size;
            mem_block_t *next_block = block->next;

            if (next_block && (uint8_t *)next_block == block_end &&
                (next_block->flags & MEM_BLOCK_FREE)) {
                /* Merge: expand current block to encompass next */
                block->size += sizeof(mem_block_t) + next_block->size;

                /* Remove next from list */
                next_block->magic = 0;  /* Invalidate */
                block->next = next_block->next;
                if (next_block->next)
                    next_block->next->prev = block;

                merged = true;
                break;  /* Restart scan */
            }
        }
    }
}

/*
 * mem_split_block — Split a large free block if it's significantly
 * larger than needed. The remainder becomes a new free block.
 */
static void mem_split_block(mem_block_t *block, flux_size_t needed)
{
    /* Only split if remainder is large enough to be useful */
    flux_size_t remainder = block->size - needed;
    flux_size_t min_block = sizeof(mem_block_t) + FLUX_MEM_ALIGN;

    if (remainder >= min_block) {
        /* Create new free block in the remainder space */
        uint8_t *new_block_addr = (uint8_t *)block + sizeof(mem_block_t) + needed;
        mem_block_t *new_block = (mem_block_t *)new_block_addr;

        new_block->magic = MEM_BLOCK_MAGIC;
        new_block->flags = MEM_BLOCK_FREE;
        new_block->size = remainder - sizeof(mem_block_t);
        new_block->type = FLUX_MEM_ANY;
        new_block->owner_pid = FLUX_PID_INVALID;
        new_block->next = NULL;
        new_block->prev = NULL;

        /* Shrink original block */
        block->size = needed;

        /* Add remainder to free list */
        mem_add_to_free_list(new_block);
    }
}

/*
 * mem_map_add — Record an allocation in the memory map.
 */
static flux_status_t mem_map_add(flux_addr_t base, flux_size_t size,
                                 flux_mem_type_t type, uint32_t owner_pid,
                                 const char *tag)
{
    /* Find a free entry */
    for (uint32_t i = 0; i < FLUX_MEM_MAX_MAP_ENTRIES; i++) {
        if (!s_mem_map[i].active) {
            s_mem_map[i].base = base;
            s_mem_map[i].size = size;
            s_mem_map[i].type = type;
            s_mem_map[i].perms = FLUX_PERM_RWX;  /* Default: full access */
            s_mem_map[i].owner_pid = owner_pid;
            s_mem_map[i].active = true;

            if (tag) {
                int j;
                for (j = 0; j < 31 && tag[j]; j++)
                    s_mem_map[i].tag[j] = tag[j];
                s_mem_map[i].tag[j] = '\0';
            } else {
                s_mem_map[i].tag[0] = '\0';
            }

            if (i >= s_mem_map_count)
                s_mem_map_count = i + 1;

            return FLUX_OK;
        }
    }

    return FLUX_ERR_OVERFLOW;
}

/*
 * mem_map_remove — Remove an entry from the memory map.
 */
static flux_status_t mem_map_remove(flux_addr_t base)
{
    for (uint32_t i = 0; i < s_mem_map_count; i++) {
        if (s_mem_map[i].active && s_mem_map[i].base == base) {
            s_mem_map[i].active = false;
            return FLUX_OK;
        }
    }
    return FLUX_ERR_NOTFOUND;
}

/*
 * mem_find_perm — Find a permission entry matching the given address range.
 */
static mem_perm_entry_t *mem_find_perm(flux_addr_t addr, flux_size_t size)
{
    for (uint32_t i = 0; i < s_perm_count; i++) {
        if (!s_perm_table[i].active)
            continue;

        mem_perm_entry_t *pe = &s_perm_table[i];
        flux_addr_t pe_end = pe->base + pe->size;
        flux_addr_t req_end = addr + size;

        /* Check for overlap */
        if (addr < pe_end && req_end > pe->base) {
            return pe;
        }
    }
    return NULL;
}

/* ========================================================================
 * Memory Subsystem Initialization
 * ======================================================================== */

/*
 * flux_mem_init — Initialize the memory manager.
 *
 * Sets up the heap region as a single large free block. In a real kernel,
 * this would walk the HAL's physical memory map to find available RAM.
 */
flux_status_t flux_mem_init(void)
{
    /* Clear all state */
    memset(s_heap, 0, FLUX_HEAP_SIZE);
    s_free_list = NULL;
    s_used_heap = 0;
    s_free_heap = FLUX_HEAP_SIZE;
    s_alloc_count = 0;
    s_free_count = 0;
    s_alloc_failures = 0;
    s_alloc_seq = 0;
    s_mem_map_count = 0;
    s_perm_count = 0;
    s_mem_lock = 0;

    /* Initialize the entire heap as one free block */
    mem_block_t *initial = (mem_block_t *)s_heap;
    initial->magic = MEM_BLOCK_MAGIC;
    initial->flags = MEM_BLOCK_FREE;
    initial->size = FLUX_HEAP_SIZE - sizeof(mem_block_t);
    initial->type = FLUX_MEM_ANY;
    initial->owner_pid = FLUX_PID_INVALID;
    initial->next = NULL;
    initial->prev = NULL;

    s_free_list = initial;

    /* Record in memory map */
    mem_map_add((flux_addr_t)s_heap, FLUX_HEAP_SIZE, FLUX_MEM_KERNEL,
                FLUX_PID_KERNEL, "kernel-heap");

    return FLUX_OK;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

/*
 * flux_alloc — Allocate memory with type tagging.
 *
 * Parameters:
 *   size — Number of bytes to allocate
 *   type — Memory type tag (KERNEL, USER, BYTECODE, AGENT, COMPILED, etc.)
 *
 * Returns:
 *   Pointer to allocated memory, or NULL on failure.
 *   The returned pointer is guaranteed to be aligned to FLUX_MEM_ALIGN.
 */
void *flux_alloc(flux_size_t size, flux_mem_type_t type)
{
    if (size == 0)
        return NULL;

    /* Align size */
    flux_size_t aligned_size = mem_align_up(size, FLUX_MEM_ALIGN);

    mem_lock();

    /* Find a suitable free block */
    mem_block_t *block = mem_find_free_block(aligned_size);
    if (!block) {
        s_alloc_failures++;
        mem_unlock();
        return NULL;
    }

    /* Remove from free list */
    mem_remove_from_free_list(block);

    /* Split if block is much larger than needed */
    mem_split_block(block, aligned_size);

    /* Mark as used */
    block->magic = MEM_BLOCK_MAGIC;
    block->flags = MEM_BLOCK_USED;
    block->type = type;
    block->owner_pid = flux_kernel_state()
                        ? flux_kernel_state()->current_pid
                        : FLUX_PID_KERNEL;

    /* Update statistics */
    flux_size_t total = sizeof(mem_block_t) + aligned_size;
    s_used_heap += total;
    s_free_heap -= total;
    s_alloc_count++;
    s_alloc_seq++;

    /* Record in memory map */
    void *ptr = mem_ptr_from_block(block);
    char tag[32];
    const char *type_names[] = {
        "any", "kernel", "user", "device", "bytecode", "agent", "compiled"
    };
    snprintf(tag, sizeof(tag), "alloc-%s-%u",
             type < 7 ? type_names[type] : "unknown", s_alloc_seq);
    mem_map_add((flux_addr_t)ptr, aligned_size, type, block->owner_pid, tag);

    mem_unlock();

    /* Zero the allocated memory */
    memset(ptr, 0, aligned_size);

    return ptr;
}

/*
 * flux_free — Free previously allocated memory.
 *
 * Parameters:
 *   ptr — Pointer previously returned by flux_alloc()
 *
 * Returns:
 *   FLUX_OK on success, FLUX_ERR_INVALID if ptr is NULL or corrupt.
 */
void flux_free(void *ptr)
{
    if (!ptr)
        return;

    mem_lock();

    mem_block_t *block = mem_block_from_ptr(ptr);

    /* Validate the block */
    if (block->magic != MEM_BLOCK_MAGIC) {
        mem_unlock();
        return;
    }

    if (!(block->flags & MEM_BLOCK_USED)) {
        /* Double free detected */
        mem_unlock();
        return;
    }

    flux_size_t total = sizeof(mem_block_t) + block->size;

    /* Remove from memory map */
    mem_map_remove((flux_addr_t)ptr);

    /* Update statistics */
    s_used_heap -= total;
    s_free_heap += total;
    s_free_count++;

    /* Clear owner */
    block->owner_pid = FLUX_PID_INVALID;

    /* Add back to free list */
    mem_add_to_free_list(block);

    /* Coalesce adjacent free blocks */
    mem_coalesce();

    mem_unlock();
}

/*
 * flux_mem_protect — Set memory permissions for a region.
 *
 * In hosted mode, this records the permission change but cannot enforce it
 * (POSIX mprotect could be used). On bare metal, this calls through the
 * HAL's vm_protect().
 *
 * Parameters:
 *   addr — Start address of the region
 *   size — Size in bytes
 *   perm — Permission flags (FLUX_PERM_READ, FLUX_PERM_WRITE, FLUX_PERM_EXEC)
 *
 * Returns:
 *   FLUX_OK on success, FLUX_ERR_INVALID for bad parameters.
 */
flux_status_t flux_mem_protect(flux_addr_t addr, flux_size_t size, flux_perm_t perm)
{
    if (size == 0 || addr == 0)
        return FLUX_ERR_INVALID;

    mem_lock();

    /* Record permission in local table */
    mem_perm_entry_t *pe = NULL;

    /* Try to find existing entry for this exact range */
    for (uint32_t i = 0; i < s_perm_count; i++) {
        if (s_perm_table[i].active &&
            s_perm_table[i].base == addr &&
            s_perm_table[i].size == size) {
            pe = &s_perm_table[i];
            break;
        }
    }

    if (pe) {
        /* Update existing */
        pe->perms = perm;
    } else {
        /* Create new entry */
        if (s_perm_count < FLUX_MEM_MAX_PERMISSIONS) {
            pe = &s_perm_table[s_perm_count++];
            pe->base = addr;
            pe->size = size;
            pe->perms = perm;
            pe->active = true;
        } else {
            mem_unlock();
            return FLUX_ERR_OVERFLOW;
        }
    }

    /* Also update the memory map entry if one exists */
    for (uint32_t i = 0; i < s_mem_map_count; i++) {
        if (s_mem_map[i].active && s_mem_map[i].base == addr) {
            s_mem_map[i].perms = perm;
            break;
        }
    }

    /* Call through to HAL if available */
    const flux_hal_t *hal = flux_hal_get();
    if (hal && hal->vm_protect) {
        hal->vm_protect(addr, size, perm);
    }

    mem_unlock();

    return FLUX_OK;
}

/*
 * flux_mem_stats — Get memory usage statistics.
 * Returns a formatted string with current memory state.
 */
const char *flux_mem_stats(void)
{
    static char stats_buf[256];

    snprintf(stats_buf, sizeof(stats_buf),
             "Memory: total=%llu used=%llu free=%llu allocs=%u frees=%u fails=%u map_entries=%u",
             (unsigned long long)s_total_heap,
             (unsigned long long)s_used_heap,
             (unsigned long long)s_free_heap,
             s_alloc_count, s_free_count, s_alloc_failures,
             s_mem_map_count);

    return stats_buf;
}

/*
 * flux_mem_check — Validate a memory region.
 * Checks that a pointer/size range falls within the heap and
 * has a valid allocation header.
 */
bool flux_mem_check(const void *ptr, flux_size_t size)
{
    if (!ptr)
        return false;

    uint8_t *addr = (uint8_t *)ptr;
    uint8_t *heap_start = s_heap;
    uint8_t *heap_end = s_heap + FLUX_HEAP_SIZE;

    /* Check bounds */
    if (addr < heap_start || addr >= heap_end)
        return false;

    if (addr + size > heap_end)
        return false;

    /* Validate header */
    mem_block_t *block = mem_block_from_ptr((void *)ptr);
    if (block->magic != MEM_BLOCK_MAGIC)
        return false;

    if (!(block->flags & MEM_BLOCK_USED))
        return false;

    if (block->size < size)
        return false;

    return true;
}

/*
 * flux_mem_map_dump — Dump the memory map for debugging.
 */
void flux_mem_map_dump(void)
{
    static const char *type_names[] = {
        "ANY", "KERNEL", "USER", "DEVICE", "BYTECODE", "AGENT", "COMPILED"
    };

    char buf[256];

    for (uint32_t i = 0; i < s_mem_map_count; i++) {
        if (!s_mem_map[i].active)
            continue;

        snprintf(buf, sizeof(buf),
                 "  [%3u] 0x%016llX %8llu  %-10s PID=%-4u %s\r\n",
                 i,
                 (unsigned long long)s_mem_map[i].base,
                 (unsigned long long)s_mem_map[i].size,
                 s_mem_map[i].type < 7 ? type_names[s_mem_map[i].type] : "???",
                 s_mem_map[i].owner_pid,
                 s_mem_map[i].tag);

        /* Use HAL if available */
        const flux_hal_t *hal = flux_hal_get();
        if (hal && hal->console_puts)
            hal->console_puts(buf);
        else
            fputs(buf, stderr);
    }
}

/*
 * flux_realloc — Reallocate memory to a new size.
 * If the existing block is large enough, returns the same pointer.
 * Otherwise, allocates new memory, copies data, and frees the old block.
 */
void *flux_realloc(void *ptr, flux_size_t new_size, flux_mem_type_t type)
{
    if (!ptr)
        return flux_alloc(new_size, type);

    if (new_size == 0) {
        flux_free(ptr);
        return NULL;
    }

    mem_block_t *block = mem_block_from_ptr(ptr);

    /* Validate */
    if (block->magic != MEM_BLOCK_MAGIC || !(block->flags & MEM_BLOCK_USED))
        return NULL;

    /* If current block is large enough */
    flux_size_t aligned_new = mem_align_up(new_size, FLUX_MEM_ALIGN);
    if (block->size >= aligned_new) {
        block->type = type;  /* Update type */
        return ptr;
    }

    /* Allocate new block */
    void *new_ptr = flux_alloc(new_size, type);
    if (!new_ptr)
        return NULL;

    /* Copy old data */
    mem_block_t *new_block = mem_block_from_ptr(new_ptr);
    flux_size_t copy_size = block->size < new_block->size ? block->size : new_block->size;
    memcpy(new_ptr, ptr, copy_size);

    /* Free old block */
    flux_free(ptr);

    return new_ptr;
}
