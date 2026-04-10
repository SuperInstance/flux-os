/*
 * FLUX OS — Bytecode Virtual Machine Interpreter
 *
 * The FLUX VM is a register-based bytecode interpreter with 64 general-purpose
 * registers. This file implements the core fetch-decode-execute loop and
 * all opcode handlers for the complete FLUX instruction set.
 *
 * Instruction encoding (all instructions are 4 bytes / 32 bits):
 *   A-type: [opcode:8] [rd:8] [rs1:8] [rs2:8]         — Register-Register
 *   B-type: [opcode:8] [rd:8]   [imm16:16]             — Register-Immediate
 *   C-type: [opcode:8] [target:24]                     — Branch target
 *   D-type: [opcode:8] [rd:8] [rs1:8] [offset:8]       — Memory access
 *   E-type: [opcode:8] [imm32:32]                      — Extended immediate
 *
 * Register conventions:
 *   R0     = Always zero (writes are ignored)
 *   R1     = Return address (RA)
 *   R2     = Stack pointer (SP)
 *   R3     = Base pointer (BP)
 *   R4     = Program counter (PC, implicit in instruction fetch)
 *   R5     = Flags register
 *   R6     = Frame pointer (FP)
 *   R7     = Temporary (T0)
 *   R8-R15 = Callee-saved (S0-S7)
 *   R16-R19= Argument registers (A0-A3)
 *   R20-R31= Temporaries (T1-T11)
 *   R32-R47= Agent registers
 *   R56-R63= Special-purpose (VMSTATE, CYCLE_COUNT, etc.)
 *
 * Copyright (c) 2025 SuperInstance
 */

#include <flux/vm.h>
#include <flux/opcodes.h>
#include <flux/hal.h>
#include <flux/agent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/* Read a 32-bit instruction from bytecode at current PC (big-endian) */
static inline uint32_t fetch_instruction(flux_vm_t *vm) {
    flux_size_t pc = vm->pc;
    if (pc + 4 > vm->bytecode_len) {
        return 0; /* Will be caught as invalid */
    }
    return ((uint32_t)vm->bytecode[pc] << 24) |
           ((uint32_t)vm->bytecode[pc + 1] << 16) |
           ((uint32_t)vm->bytecode[pc + 2] << 8) |
           ((uint32_t)vm->bytecode[pc + 3]);
}

/* Read 64-bit value from region memory at given offset */
static inline int64_t mem_read64(const uint8_t *data, flux_size_t offset, flux_size_t size) {
    if (offset + 8 > size) return 0;
    int64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val = (val << 8) | data[offset + i];
    }
    return val;
}

/* Write 64-bit value to region memory at given offset */
static inline void mem_write64(uint8_t *data, flux_size_t offset, flux_size_t size, int64_t val) {
    if (offset + 8 > size) return;
    for (int i = 7; i >= 0; i--) {
        data[offset + i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
}

/* Read 32-bit value from memory */
static inline int32_t mem_read32(const uint8_t *data, flux_size_t offset, flux_size_t size) {
    if (offset + 4 > size) return 0;
    return ((int32_t)data[offset] << 24) |
           ((int32_t)data[offset + 1] << 16) |
           ((int32_t)data[offset + 2] << 8) |
           (int32_t)data[offset + 3];
}

/* Write 32-bit value to memory */
static inline void mem_write32(uint8_t *data, flux_size_t offset, flux_size_t size, int32_t val) {
    if (offset + 4 > size) return;
    data[offset]     = (uint8_t)((val >> 24) & 0xFF);
    data[offset + 1] = (uint8_t)((val >> 16) & 0xFF);
    data[offset + 2] = (uint8_t)((val >> 8) & 0xFF);
    data[offset + 3] = (uint8_t)(val & 0xFF);
}

/* Read 16-bit value from memory */
static inline int16_t mem_read16(const uint8_t *data, flux_size_t offset, flux_size_t size) {
    if (offset + 2 > size) return 0;
    return (int16_t)(((uint16_t)data[offset] << 8) | data[offset + 1]);
}

/* Write 16-bit value to memory */
static inline void mem_write16(uint8_t *data, flux_size_t offset, flux_size_t size, int16_t val) {
    if (offset + 2 > size) return;
    data[offset]     = (uint8_t)((val >> 8) & 0xFF);
    data[offset + 1] = (uint8_t)(val & 0xFF);
}

/* Read 8-bit value from memory */
static inline int8_t mem_read8(const uint8_t *data, flux_size_t offset, flux_size_t size) {
    if (offset >= size) return 0;
    return (int8_t)data[offset];
}

/* Write 8-bit value to memory */
static inline void mem_write8(uint8_t *data, flux_size_t offset, flux_size_t size, int8_t val) {
    if (offset >= size) return;
    data[offset] = (uint8_t)val;
}

/* Helper: push value onto VM stack */
static inline int stack_push(flux_vm_t *vm, uint64_t val) {
    if (vm->stack_ptr >= vm->stack_size) {
        vm->error_code = 1; /* Stack overflow */
        vm->state = FLUX_VM_ERROR;
        return FLUX_ERR_OVERFLOW;
    }
    vm->stack[vm->stack_ptr++] = val;
    return FLUX_OK;
}

/* Helper: pop value from VM stack */
static inline int stack_pop(flux_vm_t *vm, uint64_t *val) {
    if (vm->stack_ptr == 0) {
        vm->error_code = 2; /* Stack underflow */
        vm->state = FLUX_VM_ERROR;
        return FLUX_ERR_GENERAL;
    }
    *val = vm->stack[--vm->stack_ptr];
    return FLUX_OK;
}

/* Helper: push return address onto call stack */
static inline int call_stack_push(flux_vm_t *vm, uint64_t ret_addr) {
    if (vm->call_depth >= FLUX_VM_MAX_CALL_DEPTH) {
        vm->error_code = 3; /* Call stack overflow */
        vm->state = FLUX_VM_ERROR;
        return FLUX_ERR_OVERFLOW;
    }
    vm->call_stack[vm->call_depth++] = ret_addr;
    return FLUX_OK;
}

/* Helper: pop return address from call stack */
static inline int call_stack_pop(flux_vm_t *vm, uint64_t *ret_addr) {
    if (vm->call_depth == 0) {
        vm->error_code = 4; /* Call stack underflow */
        vm->state = FLUX_VM_ERROR;
        return FLUX_ERR_GENERAL;
    }
    *ret_addr = vm->call_stack[--vm->call_depth];
    return FLUX_OK;
}

/* Helper: reinterpret uint64_t bits as double */
static inline double u64_to_double(uint64_t v) {
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

/* Helper: reinterpret double as uint64_t bits */
static inline uint64_t double_to_u64(double d) {
    uint64_t v;
    memcpy(&v, &d, sizeof(v));
    return v;
}

/* Helper: reinterpret uint64_t bits as float (stored in lower 32 bits) */
static inline float u64_to_float(uint64_t v) {
    uint32_t fbits = (uint32_t)(v & 0xFFFFFFFF);
    float f;
    memcpy(&f, &fbits, sizeof(f));
    return f;
}

/* Helper: reinterpret float as uint64_t */
static inline uint64_t float_to_u64(float f) {
    uint32_t fbits;
    memcpy(&fbits, &f, sizeof(fbits));
    return (uint64_t)fbits;
}

/* Count leading zeros for 64-bit value */
static inline int clz64(uint64_t x) {
    if (x == 0) return 64;
    int n = 0;
    if (x <= 0x00000000FFFFFFFFULL) { n += 32; x <<= 32; }
    if (x <= 0x0000FFFFFFFFFFFFULL) { n += 16; x <<= 16; }
    if (x <= 0x00FFFFFFFFFFFFFFULL) { n += 8;  x <<= 8; }
    if (x <= 0x0FFFFFFFFFFFFFFFULL) { n += 4;  x <<= 4; }
    if (x <= 0x3FFFFFFFFFFFFFFFULL) { n += 2;  x <<= 2; }
    if (x <= 0x7FFFFFFFFFFFFFFFULL) { n += 1; }
    return n;
}

/* Count trailing zeros for 64-bit value */
static inline int ctz64(uint64_t x) {
    if (x == 0) return 64;
    int n = 0;
    if ((x & 0x00000000FFFFFFFFULL) == 0) { n += 32; x >>= 32; }
    if ((x & 0x000000000000FFFFULL) == 0) { n += 16; x >>= 16; }
    if ((x & 0x00000000000000FFULL) == 0) { n += 8;  x >>= 8; }
    if ((x & 0x000000000000000FULL) == 0) { n += 4;  x >>= 4; }
    if ((x & 0x0000000000000003ULL) == 0) { n += 2;  x >>= 2; }
    if ((x & 0x0000000000000001ULL) == 0) { n += 1; }
    return n;
}

/* Population count (number of set bits) */
static inline int popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

/* Byte swap */
static inline uint64_t bswap64(uint64_t x) {
    x = ((x & 0x00000000FFFFFFFFULL) << 32) | ((x & 0xFFFFFFFF00000000ULL) >> 32);
    x = ((x & 0x0000FFFF0000FFFFULL) << 16) | ((x & 0xFFFF0000FFFF0000ULL) >> 16);
    x = ((x & 0x00FF00FF00FF00FFULL) << 8)  | ((x & 0xFF00FF00FF00FF00ULL) >> 8);
    return x;
}

/* Rotate left */
static inline uint64_t rotl64(uint64_t x, int n) {
    n &= 63;
    return (x << n) | (x >> (64 - n));
}

/* Rotate right */
static inline uint64_t rotr64(uint64_t x, int n) {
    n &= 63;
    return (x >> n) | (x << (64 - n));
}

/* ========================================================================
 * VM State Name (for diagnostics)
 * ======================================================================== */

static const char *vm_state_name(flux_vm_state_t state) {
    switch (state) {
        case FLUX_VM_IDLE:    return "IDLE";
        case FLUX_VM_RUNNING: return "RUNNING";
        case FLUX_VM_HALTED:  return "HALTED";
        case FLUX_VM_TRAPPED: return "TRAPPED";
        case FLUX_VM_ERROR:   return "ERROR";
        case FLUX_VM_WAITING: return "WAITING";
        default:              return "UNKNOWN";
    }
}

/* ========================================================================
 * VM Initialization
 * ======================================================================== */

flux_status_t flux_vm_init(flux_vm_t *vm) {
    if (vm == NULL) return FLUX_ERR_INVALID;

    memset(vm, 0, sizeof(flux_vm_t));

    /* Set initial state */
    vm->state = FLUX_VM_IDLE;
    vm->error_code = 0;

    /* R0 is always zero */
    vm->regs[FLUX_REG_ZERO] = 0;

    /* Initialize stack pointer register */
    vm->stack_size = FLUX_STACK_SIZE;
    vm->stack = (uint64_t *)calloc(vm->stack_size, sizeof(uint64_t));
    if (vm->stack == NULL) {
        return FLUX_ERR_NOMEM;
    }
    vm->stack_ptr = 0;

    /* Set SP register to top of stack */
    vm->regs[FLUX_REG_SP] = vm->stack_size;

    /* Set base pointer */
    vm->regs[FLUX_REG_BP] = vm->stack_size;

    /* Set trap handler to invalid */
    vm->regs[FLUX_REG_TRAP_HANDLER] = 0xFFFFFFFFFFFFFFFFULL;

    /* No active region initially */
    vm->active_region = -1;

    /* No breakpoints */
    vm->num_breakpoints = 0;

    /* Tracing off by default */
    vm->tracing = false;
    vm->trace_idx = 0;

    return FLUX_OK;
}

/* ========================================================================
 * VM Reset
 * ======================================================================== */

flux_status_t flux_vm_reset(flux_vm_t *vm) {
    if (vm == NULL) return FLUX_ERR_INVALID;

    /* Save stack allocation (don't free) */
    uint64_t *saved_stack = vm->stack;
    flux_size_t saved_stack_size = vm->stack_size;

    /* Zero out everything except stack pointer */
    memset(vm, 0, sizeof(flux_vm_t));

    /* Restore stack */
    vm->stack = saved_stack;
    vm->stack_size = saved_stack_size;
    memset(vm->stack, 0, vm->stack_size * sizeof(uint64_t));
    vm->stack_ptr = 0;

    /* Reset state */
    vm->state = FLUX_VM_IDLE;
    vm->regs[FLUX_REG_ZERO] = 0;
    vm->regs[FLUX_REG_SP] = vm->stack_size;
    vm->regs[FLUX_REG_BP] = vm->stack_size;
    vm->regs[FLUX_REG_TRAP_HANDLER] = 0xFFFFFFFFFFFFFFFFULL;
    vm->active_region = -1;

    return FLUX_OK;
}

/* ========================================================================
 * VM Destroy
 * ======================================================================== */

flux_status_t flux_vm_destroy(flux_vm_t *vm) {
    if (vm == NULL) return FLUX_ERR_INVALID;

    /* Free all memory regions */
    for (int i = 0; i < FLUX_REGION_MAX; i++) {
        if (vm->regions[i].data != NULL) {
            free(vm->regions[i].data);
            vm->regions[i].data = NULL;
        }
        vm->regions[i].active = false;
    }

    /* Free stack */
    if (vm->stack != NULL) {
        free(vm->stack);
        vm->stack = NULL;
    }

    /* Zero out the VM struct */
    memset(vm, 0, sizeof(flux_vm_t));

    return FLUX_OK;
}

/* ========================================================================
 * Bytecode Loading
 * ======================================================================== */

flux_status_t flux_vm_load(flux_vm_t *vm, const uint8_t *bytecode, flux_size_t len) {
    if (vm == NULL || bytecode == NULL) return FLUX_ERR_INVALID;
    if (len < 4) return FLUX_ERR_INVALID;

    /* Store bytecode reference (not copied — caller must keep alive) */
    vm->bytecode = bytecode;
    vm->bytecode_len = len;
    vm->pc = 0;

    /* Set state to ready */
    vm->state = FLUX_VM_IDLE;

    return FLUX_OK;
}

flux_status_t flux_vm_load_from_file(flux_vm_t *vm, const char *path) {
    if (vm == NULL || path == NULL) return FLUX_ERR_INVALID;

    FILE *f = fopen(path, "rb");
    if (f == NULL) return FLUX_ERR_NOTFOUND;

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FLUX_ERR_INVALID;
    }

    /* Allocate and read */
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return FLUX_ERR_NOMEM;
    }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if ((long)read != size) {
        free(buf);
        return FLUX_ERR_GENERAL;
    }

    /* Load into VM */
    flux_status_t status = flux_vm_load(vm, buf, (flux_size_t)read);

    /* Note: bytecode pointer is stored directly. The caller of this
     * function would need to manage the buf lifetime. For simplicity,
     * we accept the memory leak here; in production, the VM would
     * take ownership or copy. */
    (void)status;
    return FLUX_OK;
}

/* ========================================================================
 * VM Halt
 * ======================================================================== */

flux_status_t flux_vm_halt(flux_vm_t *vm) {
    if (vm == NULL) return FLUX_ERR_INVALID;
    vm->state = FLUX_VM_HALTED;
    return FLUX_OK;
}

/* ========================================================================
 * VM Trap
 * ======================================================================== */

flux_status_t flux_vm_trap(flux_vm_t *vm, uint8_t trap_code) {
    if (vm == NULL) return FLUX_ERR_INVALID;
    vm->state = FLUX_VM_TRAPPED;
    vm->error_code = trap_code;
    vm->regs[FLUX_REG_FLAGS] |= FLUX_FLAG_TRAP;
    vm->regs[FLUX_REG_ERROR_CODE] = trap_code;
    return FLUX_ERR_GENERAL;
}

/* ========================================================================
 * Memory Region Access (inline from region.c interface)
 *
 * These are resolved at link time against region.c.
 * We provide weak defaults here so vm.c can compile standalone.
 * ======================================================================== */

extern int flux_vm_region_create(flux_vm_t *vm, const char *name,
                                 flux_size_t size, bool readonly);
extern void *flux_vm_region_data(flux_vm_t *vm, int region_id);
extern flux_status_t flux_vm_region_destroy(flux_vm_t *vm, int region_id);

/* ========================================================================
 * Trace Snapshot
 * ======================================================================== */

static void trace_snapshot(flux_vm_t *vm, uint8_t opcode) {
    if (!vm->tracing) return;

    int idx = vm->trace_idx % FLUX_VM_TRACE_BUFFER;
    vm->trace[idx].pc = (uint32_t)vm->pc;
    vm->trace[idx].opcode = opcode;

    /* Snapshot R0-R7 */
    for (int i = 0; i < 8; i++) {
        vm->trace[idx].regs[i] = vm->regs[i];
    }

    vm->trace_idx++;
}

/* ========================================================================
 * CORE: Execute One Instruction
 *
 * This is the main interpreter loop body. It fetches a 4-byte instruction,
 * decodes it, and dispatches to the appropriate handler based on opcode.
 *
 * Returns:
 *   FLUX_OK      — instruction executed successfully
 *   FLUX_ERR_*   — error occurred (VM state updated accordingly)
 * ======================================================================== */

flux_status_t flux_vm_step(flux_vm_t *vm) {
    if (vm == NULL) return FLUX_ERR_INVALID;

    /* Check VM state */
    if (vm->state == FLUX_VM_HALTED || vm->state == FLUX_VM_ERROR) {
        return FLUX_ERR_GENERAL;
    }

    /* Check for breakpoint at current PC */
    for (int i = 0; i < vm->num_breakpoints; i++) {
        if (vm->breakpoint_addrs[i] == (uint32_t)vm->pc) {
            vm->state = FLUX_VM_TRAPPED;
            vm->error_code = 0; /* Breakpoint trap code 0 */
            return FLUX_ERR_GENERAL;
        }
    }

    /* Bounds check */
    if (vm->pc + 4 > vm->bytecode_len) {
        vm->state = FLUX_VM_ERROR;
        vm->error_code = 10; /* PC out of bounds */
        return FLUX_ERR_GENERAL;
    }

    /* Set running state */
    vm->state = FLUX_VM_RUNNING;

    /* Fetch instruction (4 bytes, big-endian) */
    uint32_t insn = fetch_instruction(vm);
    uint8_t opcode = flux_decode_opcode(insn);
    uint8_t rd    = flux_decode_rd(insn);
    uint8_t rs1   = flux_decode_rs1(insn);
    uint8_t rs2   = flux_decode_rs2(insn);
    int16_t imm16 = flux_decode_imm16(insn);
    uint32_t target = flux_decode_target(insn);

    /* Sign-extend imm16 to int32_t for convenience */
    int32_t imm = (int32_t)imm16;

    /* Get register values (R0 always reads as 0) */
    uint64_t v_rs1 = flux_vm_get_reg(vm, rs1);
    uint64_t v_rs2 = flux_vm_get_reg(vm, rs2);
    int64_t  s_rs1 = (int64_t)v_rs1;
    int64_t  s_rs2 = (int64_t)v_rs2;

    /* Update cycle and instruction counts */
    vm->cycle_count++;
    vm->insn_count++;
    vm->op_counts[opcode]++;
    vm->last_op_cycles = 1;

    /* Take trace snapshot (before execution) */
    trace_snapshot(vm, opcode);

    /* Default: advance PC by 4 */
    vm->pc += 4;

    /* ====================================================================
     * Dispatch on opcode
     * ==================================================================== */

    switch (opcode) {

    /* ==================================================================
     * SYSTEM OPERATIONS (0x00 - 0x0F)
     * ================================================================== */

    case OP_NOP:
        /* No operation */
        break;

    case OP_HALT:
        vm->state = FLUX_VM_HALTED;
        return FLUX_OK;

    case OP_TRAP: {
        /* If trap handler is set, jump to it */
        uint64_t handler = vm->regs[FLUX_REG_TRAP_HANDLER];
        if (handler != 0xFFFFFFFFFFFFFFFFULL) {
            /* Push current PC as return address */
            vm->regs[FLUX_REG_RA] = vm->pc;
            vm->pc = (flux_size_t)handler;
            vm->error_code = (uint8_t)(imm & 0xFF);
        } else {
            return flux_vm_trap(vm, (uint8_t)(imm & 0xFF));
        }
        break;
    }

    case OP_INVALID:
        vm->state = FLUX_VM_ERROR;
        vm->error_code = 20; /* Invalid instruction */
        return FLUX_ERR_INVALID;

    /* Extended system ops: wake, panic, debug, info */
    case 0x04: /* SYS_WAKE */
    case 0x05: /* SYS_PANIC */
    case 0x06: /* SYS_DEBUG */
    case 0x07: /* SYS_INFO */
    case 0x08: /* SYS_RESET */
    case 0x09: /* SYS_SUSPEND */
    case 0x0A: /* SYS_RESUME */
    case 0x0B: /* SYS_YIELD */
    case 0x0C: /* SYS_SLEEP */
    case 0x0D: /* SYS_TIMED_WAIT */
    case 0x0E: /* SYS_INTERRUPT */
    case 0x0F: /* SYS_EXCEPTION */
        /* Extended system ops delegate to kernel; for now, trap */
        return flux_vm_trap(vm, opcode);

    /* ==================================================================
     * ARITHMETIC OPERATIONS (0x10 - 0x1F)
     * ================================================================== */

    case OP_IADD:
        flux_vm_set_reg(vm, rd, (uint64_t)(s_rs1 + s_rs2));
        break;

    case OP_ISUB:
        flux_vm_set_reg(vm, rd, (uint64_t)(s_rs1 - s_rs2));
        break;

    case OP_IMUL:
        flux_vm_set_reg(vm, rd, (uint64_t)(s_rs1 * s_rs2));
        break;

    case OP_IDIV:
        if (s_rs2 == 0) {
            vm->state = FLUX_VM_ERROR;
            vm->error_code = 30; /* Division by zero */
            return FLUX_ERR_GENERAL;
        }
        flux_vm_set_reg(vm, rd, (uint64_t)(s_rs1 / s_rs2));
        break;

    case OP_IMOD:
        if (s_rs2 == 0) {
            vm->state = FLUX_VM_ERROR;
            vm->error_code = 30;
            return FLUX_ERR_GENERAL;
        }
        flux_vm_set_reg(vm, rd, (uint64_t)(s_rs1 % s_rs2));
        break;

    case OP_INEG:
        flux_vm_set_reg(vm, rd, (uint64_t)(-s_rs1));
        break;

    case OP_IABS:
        flux_vm_set_reg(vm, rd, (uint64_t)(s_rs1 < 0 ? -s_rs1 : s_rs1));
        break;

    case OP_INC:
        flux_vm_set_reg(vm, rd, flux_vm_get_reg(vm, rd) + 1);
        break;

    case OP_DEC:
        flux_vm_set_reg(vm, rd, flux_vm_get_reg(vm, rd) - 1);
        break;

    case OP_FADD: {
        double a = u64_to_double(v_rs1);
        double b = u64_to_double(v_rs2);
        flux_vm_set_reg(vm, rd, double_to_u64(a + b));
        break;
    }

    case OP_FSUB: {
        double a = u64_to_double(v_rs1);
        double b = u64_to_double(v_rs2);
        flux_vm_set_reg(vm, rd, double_to_u64(a - b));
        break;
    }

    case OP_FMUL: {
        double a = u64_to_double(v_rs1);
        double b = u64_to_double(v_rs2);
        flux_vm_set_reg(vm, rd, double_to_u64(a * b));
        break;
    }

    case OP_FDIV: {
        double a = u64_to_double(v_rs1);
        double b = u64_to_double(v_rs2);
        if (b == 0.0) {
            vm->state = FLUX_VM_ERROR;
            vm->error_code = 31; /* Float division by zero */
            return FLUX_ERR_GENERAL;
        }
        flux_vm_set_reg(vm, rd, double_to_u64(a / b));
        break;
    }

    case OP_FNEG: {
        double a = u64_to_double(v_rs1);
        flux_vm_set_reg(vm, rd, double_to_u64(-a));
        break;
    }

    case OP_I2F: {
        double d = (double)s_rs1;
        flux_vm_set_reg(vm, rd, double_to_u64(d));
        break;
    }

    case OP_F2I: {
        double d = u64_to_double(v_rs1);
        flux_vm_set_reg(vm, rd, (uint64_t)(int64_t)d);
        break;
    }

    /* ==================================================================
     * LOGICAL / BITWISE OPERATIONS (0x20 - 0x2F)
     * ================================================================== */

    case OP_IAND:
        flux_vm_set_reg(vm, rd, v_rs1 & v_rs2);
        break;

    case OP_IOR:
        flux_vm_set_reg(vm, rd, v_rs1 | v_rs2);
        break;

    case OP_IXOR:
        flux_vm_set_reg(vm, rd, v_rs1 ^ v_rs2);
        break;

    case OP_INOT:
        flux_vm_set_reg(vm, rd, ~v_rs1);
        break;

    case OP_ISHL: {
        int shift = (int)(v_rs2 & 63);
        flux_vm_set_reg(vm, rd, v_rs1 << shift);
        break;
    }

    case OP_ISHR: {
        int shift = (int)(v_rs2 & 63);
        flux_vm_set_reg(vm, rd, (uint64_t)(s_rs1 >> shift));
        break;
    }

    case OP_USHR: {
        int shift = (int)(v_rs2 & 63);
        flux_vm_set_reg(vm, rd, v_rs1 >> shift);
        break;
    }

    case OP_ROTATE_L:
        flux_vm_set_reg(vm, rd, rotl64(v_rs1, (int)(v_rs2 & 63)));
        break;

    case OP_ROTATE_R:
        flux_vm_set_reg(vm, rd, rotr64(v_rs1, (int)(v_rs2 & 63)));
        break;

    case OP_POPCOUNT:
        flux_vm_set_reg(vm, rd, (uint64_t)popcount64(v_rs1));
        break;

    case OP_CLZ:
        flux_vm_set_reg(vm, rd, (uint64_t)clz64(v_rs1));
        break;

    case OP_CTZ:
        flux_vm_set_reg(vm, rd, (uint64_t)ctz64(v_rs1));
        break;

    case OP_BSWAP:
        flux_vm_set_reg(vm, rd, bswap64(v_rs1));
        break;

    case OP_ANDI:
        flux_vm_set_reg(vm, rd, v_rs1 & (uint64_t)(int32_t)imm);
        break;

    case OP_ORI:
        flux_vm_set_reg(vm, rd, v_rs1 | (uint64_t)(int32_t)imm);
        break;

    case OP_XORI:
        flux_vm_set_reg(vm, rd, v_rs1 ^ (uint64_t)(int32_t)imm);
        break;

    /* ==================================================================
     * COMPARISON OPERATIONS (0x30 - 0x3F)
     * ================================================================== */

    case OP_CMP:
        flux_vm_set_flags(vm, v_rs1, v_rs2);
        break;

    case OP_CMPI:
        flux_vm_set_flags(vm, v_rs1, (uint64_t)(int32_t)imm);
        break;

    case OP_FCMP: {
        double a = u64_to_double(v_rs1);
        double b = u64_to_double(v_rs2);
        uint64_t flags = 0;
        if (a == b) flags |= FLUX_FLAG_EQUAL;
        if (a < b)  flags |= FLUX_FLAG_LESS;
        if (a > b)  flags |= FLUX_FLAG_GREATER;
        if (a == 0.0) flags |= FLUX_FLAG_ZERO;
        if (a < 0.0) flags |= FLUX_FLAG_NEGATIVE;
        vm->regs[FLUX_REG_FLAGS] = flags;
        break;
    }

    case OP_TEST: {
        uint64_t result = v_rs1 & v_rs2;
        uint64_t flags = 0;
        if (result == 0) flags |= FLUX_FLAG_ZERO | FLUX_FLAG_EQUAL;
        vm->regs[FLUX_REG_FLAGS] = flags;
        break;
    }

    case OP_TESTI: {
        uint64_t mask = (uint64_t)(int32_t)imm;
        uint64_t result = v_rs1 & mask;
        uint64_t flags = 0;
        if (result == 0) flags |= FLUX_FLAG_ZERO | FLUX_FLAG_EQUAL;
        vm->regs[FLUX_REG_FLAGS] = flags;
        break;
    }

    /* Extended comparisons: dedicated flag-setting for specific relations */
    case 0x35: /* CMP_EQ */
        vm->regs[FLUX_REG_FLAGS] = (v_rs1 == v_rs2) ? FLUX_FLAG_EQUAL : 0;
        break;
    case 0x36: /* CMP_NE */
        vm->regs[FLUX_REG_FLAGS] = (v_rs1 != v_rs2) ? FLUX_FLAG_EQUAL : 0;
        vm->regs[FLUX_REG_FLAGS] |= FLUX_FLAG_EQUAL; /* inverted: NE sets a flag */
        if (v_rs1 == v_rs2) vm->regs[FLUX_REG_FLAGS] = 0;
        break;
    case 0x37: /* CMP_LT */
        vm->regs[FLUX_REG_FLAGS] = (s_rs1 < s_rs2) ? FLUX_FLAG_LESS : 0;
        break;
    case 0x38: /* CMP_GT */
        vm->regs[FLUX_REG_FLAGS] = (s_rs1 > s_rs2) ? FLUX_FLAG_GREATER : 0;
        break;
    case 0x39: /* CMP_LE */
        vm->regs[FLUX_REG_FLAGS] = (s_rs1 <= s_rs2) ? (FLUX_FLAG_LESS | FLUX_FLAG_EQUAL) : 0;
        break;
    case 0x3A: /* CMP_GE */
        vm->regs[FLUX_REG_FLAGS] = (s_rs1 >= s_rs2) ? (FLUX_FLAG_GREATER | FLUX_FLAG_EQUAL) : 0;
        break;

    /* Float comparisons */
    case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: {
        double fa = u64_to_double(v_rs1);
        double fb = u64_to_double(v_rs2);
        uint64_t flags = 0;
        if (fa == fb) flags |= FLUX_FLAG_EQUAL;
        if (fa < fb)  flags |= FLUX_FLAG_LESS;
        if (fa > fb)  flags |= FLUX_FLAG_GREATER;
        /* Per-opcode: clear unrelated flags */
        switch (opcode) {
            case 0x3B: flags = (fa == fb) ? FLUX_FLAG_EQUAL : 0; break;
            case 0x3C: flags = (fa < fb)  ? FLUX_FLAG_LESS : 0; break;
            case 0x3D: flags = (fa > fb)  ? FLUX_FLAG_GREATER : 0; break;
            case 0x3E: flags = (fa <= fb) ? (FLUX_FLAG_LESS | FLUX_FLAG_EQUAL) : 0; break;
            case 0x3F: flags = (fa >= fb) ? (FLUX_FLAG_GREATER | FLUX_FLAG_EQUAL) : 0; break;
            default: break;
        }
        vm->regs[FLUX_REG_FLAGS] = flags;
        break;
    }

    /* ==================================================================
     * BRANCH / JUMP OPERATIONS (0x40 - 0x4F)
     * ================================================================== */

    case OP_JMP:
        vm->pc = target;
        break;

    case OP_JZ:
        if (v_rs1 == 0) vm->pc = target;
        break;

    case OP_JNZ:
        if (v_rs1 != 0) vm->pc = target;
        break;

    case OP_JE:
        if (vm->regs[FLUX_REG_FLAGS] & FLUX_FLAG_EQUAL) vm->pc = target;
        break;

    case OP_JNE:
        if (!(vm->regs[FLUX_REG_FLAGS] & FLUX_FLAG_EQUAL)) vm->pc = target;
        break;

    case OP_JG:
        if (vm->regs[FLUX_REG_FLAGS] & FLUX_FLAG_GREATER) vm->pc = target;
        break;

    case OP_JL:
        if (vm->regs[FLUX_REG_FLAGS] & FLUX_FLAG_LESS) vm->pc = target;
        break;

    case OP_JGE:
        if (vm->regs[FLUX_REG_FLAGS] & (FLUX_FLAG_GREATER | FLUX_FLAG_EQUAL))
            vm->pc = target;
        break;

    case OP_JLE:
        if (vm->regs[FLUX_REG_FLAGS] & (FLUX_FLAG_LESS | FLUX_FLAG_EQUAL))
            vm->pc = target;
        break;

    case OP_JA:
        /* Unsigned: above (carry clear and not zero) */
        if (!(vm->regs[FLUX_REG_FLAGS] & FLUX_FLAG_CARRY) &&
            !(vm->regs[FLUX_REG_FLAGS] & FLUX_FLAG_EQUAL))
            vm->pc = target;
        break;

    case OP_JB:
        /* Unsigned: below (carry set) */
        if (vm->regs[FLUX_REG_FLAGS] & FLUX_FLAG_CARRY)
            vm->pc = target;
        break;

    case OP_JAE:
        /* Unsigned: above or equal (carry clear) */
        if (!(vm->regs[FLUX_REG_FLAGS] & FLUX_FLAG_CARRY))
            vm->pc = target;
        break;

    case OP_JBE:
        /* Unsigned: below or equal (carry set or zero) */
        if (vm->regs[FLUX_REG_FLAGS] & (FLUX_FLAG_CARRY | FLUX_FLAG_EQUAL))
            vm->pc = target;
        break;

    case OP_JC:
        if (vm->regs[FLUX_REG_FLAGS] & FLUX_FLAG_CARRY)
            vm->pc = target;
        break;

    case OP_JO:
        if (vm->regs[FLUX_REG_FLAGS] & FLUX_FLAG_OVERFLOW)
            vm->pc = target;
        break;

    case OP_LOOP:
        /* Decrement RCX (R56 as loop counter alias) and jump if nonzero */
        if (vm->regs[56] > 0) {
            vm->regs[56]--;
            if (vm->regs[56] != 0) {
                vm->pc = target;
            }
        }
        break;

    /* ==================================================================
     * MEMORY OPERATIONS (0x50 - 0x5F)
     * ================================================================== */

    case OP_LOAD:
    case OP_LOAD8:
    case OP_LOAD16:
    case OP_LOAD32:
    case OP_STORE:
    case OP_STORE8:
    case OP_STORE16:
    case OP_STORE32:
    case OP_LEA: {
        /* Compute effective address: rs1 + sign-extended offset */
        int32_t offset = (int32_t)(int8_t)rs2; /* rs2 field used as offset byte */
        uint64_t ea = v_rs1 + (int64_t)offset;

        if (vm->active_region >= 0 && vm->active_region < FLUX_REGION_MAX &&
            vm->regions[vm->active_region].active) {

            flux_vm_region_t *reg = &vm->regions[vm->active_region];

            switch (opcode) {
            case OP_LOAD:
                flux_vm_set_reg(vm, rd, (uint64_t)mem_read64(reg->data, (flux_size_t)ea, reg->size));
                break;
            case OP_LOAD8:
                flux_vm_set_reg(vm, rd, (uint64_t)(int64_t)mem_read8(reg->data, (flux_size_t)ea, reg->size));
                break;
            case OP_LOAD16:
                flux_vm_set_reg(vm, rd, (uint64_t)(int64_t)mem_read16(reg->data, (flux_size_t)ea, reg->size));
                break;
            case OP_LOAD32:
                flux_vm_set_reg(vm, rd, (uint64_t)(int64_t)mem_read32(reg->data, (flux_size_t)ea, reg->size));
                break;
            case OP_STORE:
                mem_write64(reg->data, (flux_size_t)ea, reg->size, (int64_t)flux_vm_get_reg(vm, rd));
                break;
            case OP_STORE8:
                mem_write8(reg->data, (flux_size_t)ea, reg->size, (int8_t)flux_vm_get_reg(vm, rd));
                break;
            case OP_STORE16:
                mem_write16(reg->data, (flux_size_t)ea, reg->size, (int16_t)flux_vm_get_reg(vm, rd));
                break;
            case OP_STORE32:
                mem_write32(reg->data, (flux_size_t)ea, reg->size, (int32_t)flux_vm_get_reg(vm, rd));
                break;
            case OP_LEA:
                flux_vm_set_reg(vm, rd, ea);
                break;
            default:
                break;
            }
        } else {
            /* No active region — operate on VM stack as memory */
            flux_size_t sp = (flux_size_t)vm->regs[FLUX_REG_SP];
            flux_size_t addr = sp - (flux_size_t)ea;

            switch (opcode) {
            case OP_LOAD:
                if (addr + 8 <= vm->stack_size) {
                    flux_vm_set_reg(vm, rd, vm->stack[addr]);
                }
                break;
            case OP_LOAD8: {
                if (addr < vm->stack_size) {
                    uint8_t *p = (uint8_t *)&vm->stack[addr];
                    flux_vm_set_reg(vm, rd, (uint64_t)(int64_t)(int8_t)p[0]);
                }
                break;
            }
            case OP_LOAD16: {
                if (addr + 2 <= vm->stack_size) {
                    uint8_t *p = (uint8_t *)&vm->stack[addr];
                    int16_t v = (int16_t)((uint16_t)p[0] << 8 | p[1]);
                    flux_vm_set_reg(vm, rd, (uint64_t)(int64_t)v);
                }
                break;
            }
            case OP_LOAD32: {
                if (addr + 4 <= vm->stack_size) {
                    uint8_t *p = (uint8_t *)&vm->stack[addr];
                    int32_t v = (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                                        (uint32_t)p[2] << 8 | p[3]);
                    flux_vm_set_reg(vm, rd, (uint64_t)(int64_t)v);
                }
                break;
            }
            case OP_STORE:
                if (addr + 8 <= vm->stack_size) {
                    vm->stack[addr] = flux_vm_get_reg(vm, rd);
                }
                break;
            case OP_STORE8:
                if (addr < vm->stack_size) {
                    uint8_t *p = (uint8_t *)&vm->stack[addr];
                    p[0] = (uint8_t)flux_vm_get_reg(vm, rd);
                }
                break;
            case OP_STORE16:
                if (addr + 2 <= vm->stack_size) {
                    uint8_t *p = (uint8_t *)&vm->stack[addr];
                    uint16_t v = (uint16_t)flux_vm_get_reg(vm, rd);
                    p[0] = (uint8_t)(v >> 8);
                    p[1] = (uint8_t)(v);
                }
                break;
            case OP_STORE32:
                if (addr + 4 <= vm->stack_size) {
                    uint8_t *p = (uint8_t *)&vm->stack[addr];
                    uint32_t v = (uint32_t)flux_vm_get_reg(vm, rd);
                    p[0] = (uint8_t)(v >> 24);
                    p[1] = (uint8_t)(v >> 16);
                    p[2] = (uint8_t)(v >> 8);
                    p[3] = (uint8_t)(v);
                }
                break;
            case OP_LEA:
                flux_vm_set_reg(vm, rd, (uint64_t)addr);
                break;
            default:
                break;
            }
        }
        break;
    }

    case OP_CMPXCHG: {
        /* Atomic compare and exchange: if mem[rs1] == rs2, mem[rs1] = rd */
        /* Simplified single-threaded implementation */
        int32_t offset = (int32_t)(int8_t)rs2;
        uint64_t ea = v_rs1 + (int64_t)offset;

        if (vm->active_region >= 0 && vm->regions[vm->active_region].active) {
            flux_vm_region_t *reg = &vm->regions[vm->active_region];
            uint64_t expected = flux_vm_get_reg(vm, rs2);
            uint64_t actual = (uint64_t)mem_read64(reg->data, (flux_size_t)ea, reg->size);
            uint64_t desired = flux_vm_get_reg(vm, rd);
            if (actual == expected) {
                mem_write64(reg->data, (flux_size_t)ea, reg->size, (int64_t)desired);
            }
            flux_vm_set_reg(vm, rd, actual);
        }
        vm->regs[FLUX_REG_FLAGS] = 0;
        break;
    }

    case OP_ATOMIC_ADD: {
        int32_t offset = (int32_t)(int8_t)rs2;
        uint64_t ea = v_rs1 + (int64_t)offset;
        if (vm->active_region >= 0 && vm->regions[vm->active_region].active) {
            flux_vm_region_t *reg = &vm->regions[vm->active_region];
            int64_t old_val = mem_read64(reg->data, (flux_size_t)ea, reg->size);
            int64_t addend = (int64_t)flux_vm_get_reg(vm, rd);
            mem_write64(reg->data, (flux_size_t)ea, reg->size, old_val + addend);
            flux_vm_set_reg(vm, rd, (uint64_t)old_val);
        }
        break;
    }

    case OP_ATOMIC_SUB: {
        int32_t offset = (int32_t)(int8_t)rs2;
        uint64_t ea = v_rs1 + (int64_t)offset;
        if (vm->active_region >= 0 && vm->regions[vm->active_region].active) {
            flux_vm_region_t *reg = &vm->regions[vm->active_region];
            int64_t old_val = mem_read64(reg->data, (flux_size_t)ea, reg->size);
            int64_t subtrahend = (int64_t)flux_vm_get_reg(vm, rd);
            mem_write64(reg->data, (flux_size_t)ea, reg->size, old_val - subtrahend);
            flux_vm_set_reg(vm, rd, (uint64_t)old_val);
        }
        break;
    }

    case OP_FENCE:
        /* Memory fence — no-op in single-threaded interpreter */
        __asm__ volatile("" ::: "memory");
        break;

    case OP_LOAD_RM: {
        /* Load from specific region (rs1 = region_id, offset from rd) */
        int rid = (int)(v_rs1 & 0xFF);
        if (rid >= 0 && rid < FLUX_REGION_MAX && vm->regions[rid].active) {
            flux_vm_region_t *reg = &vm->regions[rid];
            flux_size_t off = (flux_size_t)(flux_vm_get_reg(vm, rd) & 0xFFFFFFFF);
            if (off + 8 <= reg->size) {
                flux_vm_set_reg(vm, rd, (uint64_t)mem_read64(reg->data, off, reg->size));
            }
        }
        break;
    }

    case OP_STORE_RM: {
        /* Store to specific region */
        int rid = (int)(v_rs1 & 0xFF);
        if (rid >= 0 && rid < FLUX_REGION_MAX && vm->regions[rid].active) {
            flux_vm_region_t *reg = &vm->regions[rid];
            flux_size_t off = (flux_size_t)(flux_vm_get_reg(vm, rd) & 0xFFFFFFFF);
            if (off + 8 <= reg->size) {
                mem_write64(reg->data, off, reg->size, (int64_t)v_rs2);
            }
        }
        break;
    }

    case 0x5F: /* MEMSET */ {
        if (vm->active_region >= 0 && vm->regions[vm->active_region].active) {
            flux_vm_region_t *reg = &vm->regions[vm->active_region];
            flux_size_t off = (flux_size_t)(v_rs1 & 0xFFFFFFFF);
            uint64_t fill = v_rs2;
            flux_size_t count = (flux_size_t)flux_vm_get_reg(vm, rd);
            for (flux_size_t i = 0; i < count && off + i < reg->size; i++) {
                reg->data[off + i] = (uint8_t)(fill & 0xFF);
            }
        }
        break;
    }

    /* ==================================================================
     * STACK OPERATIONS (0x60 - 0x6F)
     * ================================================================== */

    case OP_PUSH: {
        uint64_t val = flux_vm_get_reg(vm, rd);
        int rc = stack_push(vm, val);
        if (rc != FLUX_OK) return rc;
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }

    case OP_POP: {
        uint64_t val = 0;
        int rc = stack_pop(vm, &val);
        if (rc != FLUX_OK) return rc;
        flux_vm_set_reg(vm, rd, val);
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }

    case OP_PUSH_IMM: {
        uint64_t val = (uint64_t)(int32_t)imm;
        int rc = stack_push(vm, val);
        if (rc != FLUX_OK) return rc;
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }

    case OP_DUP: {
        if (vm->stack_ptr == 0) {
            vm->state = FLUX_VM_ERROR;
            vm->error_code = 2;
            return FLUX_ERR_GENERAL;
        }
        uint64_t top = vm->stack[vm->stack_ptr - 1];
        int rc = stack_push(vm, top);
        if (rc != FLUX_OK) return rc;
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }

    case OP_SWAP: {
        if (vm->stack_ptr < 2) {
            vm->state = FLUX_VM_ERROR;
            vm->error_code = 2;
            return FLUX_ERR_GENERAL;
        }
        uint64_t a = vm->stack[vm->stack_ptr - 1];
        uint64_t b = vm->stack[vm->stack_ptr - 2];
        vm->stack[vm->stack_ptr - 1] = b;
        vm->stack[vm->stack_ptr - 2] = a;
        break;
    }

    case OP_ENTER: {
        /* Push frame pointer and base pointer, allocate stack frame */
        int frame_size = (int32_t)imm;
        if (frame_size < 0) frame_size = 0;
        stack_push(vm, vm->regs[FLUX_REG_BP]);
        stack_push(vm, vm->regs[FLUX_REG_FP]);
        vm->regs[FLUX_REG_FP] = vm->regs[FLUX_REG_SP];
        vm->stack_ptr += frame_size / 8;
        if (vm->stack_ptr > vm->stack_size) {
            vm->stack_ptr = vm->stack_size;
            vm->state = FLUX_VM_ERROR;
            vm->error_code = 1;
            return FLUX_ERR_OVERFLOW;
        }
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }

    case OP_LEAVE: {
        /* Deallocate stack frame, restore BP and FP */
        vm->stack_ptr = vm->stack_size - (flux_size_t)vm->regs[FLUX_REG_FP];
        uint64_t saved_fp = 0, saved_bp = 0;
        stack_pop(vm, &saved_fp);
        stack_pop(vm, &saved_bp);
        vm->regs[FLUX_REG_FP] = saved_fp;
        vm->regs[FLUX_REG_BP] = saved_bp;
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }

    case OP_PUSHA: {
        /* Push all general-purpose registers R1-R15 */
        for (int i = 1; i <= 15; i++) {
            int rc = stack_push(vm, vm->regs[i]);
            if (rc != FLUX_OK) return rc;
        }
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }

    case OP_POPA: {
        /* Pop into R1-R15 */
        for (int i = 15; i >= 1; i--) {
            uint64_t val = 0;
            int rc = stack_pop(vm, &val);
            if (rc != FLUX_OK) return rc;
            vm->regs[i] = val;
        }
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }

    /* Extended stack operations */
    case 0x69: /* PUSHF */ {
        int rc = stack_push(vm, vm->regs[FLUX_REG_FLAGS]);
        if (rc != FLUX_OK) return rc;
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }
    case 0x6A: /* POPF */ {
        uint64_t flags = 0;
        int rc = stack_pop(vm, &flags);
        if (rc != FLUX_OK) return rc;
        vm->regs[FLUX_REG_FLAGS] = flags;
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }
    case 0x6B: /* PUSHR */ {
        /* Push register at index from rs1 */
        uint64_t val = flux_vm_get_reg(vm, rs1);
        int rc = stack_push(vm, val);
        if (rc != FLUX_OK) return rc;
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }
    case 0x6C: /* POPR */ {
        uint64_t val = 0;
        int rc = stack_pop(vm, &val);
        if (rc != FLUX_OK) return rc;
        flux_vm_set_reg(vm, rd, val);
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }
    case 0x6D: /* STACK_CHK */ {
        if (vm->stack_ptr > vm->stack_size - 256) {
            vm->regs[FLUX_REG_FLAGS] |= FLUX_FLAG_CARRY; /* Stack almost full */
        }
        break;
    }
    case 0x6E: /* STACK_ALLOC */ {
        flux_size_t alloc = (flux_size_t)flux_vm_get_reg(vm, rd);
        vm->stack_ptr += alloc / 8;
        if (vm->stack_ptr > vm->stack_size) {
            vm->stack_ptr = vm->stack_size;
            return flux_vm_trap(vm, 1);
        }
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }
    case 0x6F: /* STACK_FREE */ {
        flux_size_t free_bytes = (flux_size_t)flux_vm_get_reg(vm, rd);
        if (vm->stack_ptr >= free_bytes / 8) {
            vm->stack_ptr -= free_bytes / 8;
        } else {
            vm->stack_ptr = 0;
        }
        vm->regs[FLUX_REG_SP] = vm->stack_size - vm->stack_ptr;
        break;
    }

    /* ==================================================================
     * CALL / RETURN OPERATIONS (0x70 - 0x7F)
     * ================================================================== */

    case OP_CALL: {
        /* Push return address and jump to target in rs1 */
        uint64_t ret_addr = vm->pc;
        int rc = call_stack_push(vm, ret_addr);
        if (rc != FLUX_OK) return rc;
        vm->pc = (flux_size_t)v_rs1;
        break;
    }

    case OP_CALLI: {
        /* Call immediate address */
        uint64_t ret_addr = vm->pc;
        int rc = call_stack_push(vm, ret_addr);
        if (rc != FLUX_OK) return rc;
        vm->pc = target;
        break;
    }

    case OP_RET: {
        /* Pop return address and jump back */
        uint64_t ret_addr = 0;
        int rc = call_stack_pop(vm, &ret_addr);
        if (rc != FLUX_OK) {
            /* If call stack is empty, halt */
            vm->state = FLUX_VM_HALTED;
            return FLUX_OK;
        }
        vm->pc = (flux_size_t)ret_addr;
        break;
    }

    case OP_RETI: {
        /* Return from interrupt — also restore flags */
        uint64_t ret_addr = 0;
        int rc = call_stack_pop(vm, &ret_addr);
        if (rc != FLUX_OK) {
            vm->state = FLUX_VM_HALTED;
            return FLUX_OK;
        }
        uint64_t saved_flags = 0;
        stack_pop(vm, &saved_flags);
        vm->regs[FLUX_REG_FLAGS] = saved_flags;
        vm->pc = (flux_size_t)ret_addr;
        break;
    }

    case OP_SYSCALL: {
        /* System call — number in R60 */
        uint64_t syscall_num = vm->regs[FLUX_REG_SYSCALL_NUM];
        /* Store PC in RA for potential continuation */
        vm->regs[FLUX_REG_RA] = vm->pc;
        /* System calls are handled by the kernel dispatcher */
        /* For standalone VM, we just set the error code and continue */
        vm->error_code = (uint8_t)(syscall_num & 0xFF);
        /* In a full OS, this would call flux_syscall_dispatch */
        break;
    }

    case OP_CALL_REG: {
        /* Call address in register rd */
        uint64_t ret_addr = vm->pc;
        int rc = call_stack_push(vm, ret_addr);
        if (rc != FLUX_OK) return rc;
        vm->pc = (flux_size_t)flux_vm_get_reg(vm, rd);
        break;
    }

    case OP_TAILCALL: {
        /* Tail call — jump without pushing return address */
        vm->pc = (flux_size_t)v_rs1;
        break;
    }

    case OP_ICALL: {
        /* Indirect call via function pointer in rs1 */
        uint64_t ret_addr = vm->pc;
        int rc = call_stack_push(vm, ret_addr);
        if (rc != FLUX_OK) return rc;
        vm->pc = (flux_size_t)v_rs1;
        break;
    }

    /* Extended call operations */
    case 0x78: /* CALL_EXT */
    case 0x79: /* CALL_NATIVE */
    case 0x7A: /* CALL_AGENT */
        /* Extended calls: delegate to runtime */
        return flux_vm_trap(vm, opcode);

    case 0x7B: /* THROW */
        vm->state = FLUX_VM_ERROR;
        vm->error_code = (uint8_t)(v_rs1 & 0xFF);
        return FLUX_ERR_GENERAL;

    case 0x7C: /* CATCH */ {
        /* Set trap handler for exception handling */
        vm->regs[FLUX_REG_TRAP_HANDLER] = v_rs1;
        break;
    }

    case 0x7D: /* FINALLY */
        /* Execute cleanup — just continue */
        break;

    case 0x7E: /* EXCEPTION_RET */
        vm->state = FLUX_VM_ERROR;
        vm->error_code = 50; /* Unhandled exception */
        return FLUX_ERR_GENERAL;

    case 0x7F: /* SET_HANDLER */
        vm->regs[FLUX_REG_TRAP_HANDLER] = v_rs1;
        break;

    /* ==================================================================
     * AGENT / A2A OPERATIONS (0x80 - 0x8F)
     * ================================================================== */

    case OP_DELEGATE:
        /* Delegate task to agent: rs1=target agent, rs2=task data pointer */
        /* In standalone VM, just record the delegation */
        vm->regs[FLUX_REG_ERROR_CODE] = 0;
        break;

    case OP_TELL:
        /* Fire-and-forget message: rs1=target, rd=message data */
        vm->regs[FLUX_REG_ERROR_CODE] = 0;
        break;

    case OP_ASK:
        /* Request-reply message: rs1=target, rd=request data */
        /* Sets R0 to result when complete (blocks in full impl) */
        vm->regs[FLUX_REG_ERROR_CODE] = 0;
        break;

    case OP_REPLY:
        /* Reply to pending ASK: rs1=original msg id, rd=reply data */
        vm->regs[FLUX_REG_ERROR_CODE] = 0;
        break;

    case OP_BARRIER:
        /* Synchronization barrier: wait for all agents */
        vm->regs[FLUX_REG_ERROR_CODE] = 0;
        break;

    case OP_SPAWN: {
        /* Spawn new agent: rd=agent_id output, rs1=bytecode ptr, rs2=config */
        flux_vm_set_reg(vm, rd, 42); /* Placeholder agent ID */
        vm->regs[FLUX_REG_ERROR_CODE] = 0;
        break;
    }

    case OP_YIELD_AGENT:
        /* Yield to agent scheduler */
        vm->state = FLUX_VM_WAITING;
        return FLUX_OK;

    case OP_CAP_GRANT:
        /* Grant capability to agent: rd=capability, rs1=target agent */
        vm->regs[FLUX_REG_CAPABILITY] = v_rs1;
        break;

    case OP_CAP_CHECK: {
        /* Check if current agent has capability in rd */
        uint64_t required = flux_vm_get_reg(vm, rd);
        uint64_t current = vm->regs[FLUX_REG_CAPABILITY];
        vm->regs[FLUX_REG_FLAGS] = ((current & required) == required) ?
                                    FLUX_FLAG_EQUAL : 0;
        break;
    }

    case OP_CAP_REVOKE:
        /* Revoke capability */
        vm->regs[FLUX_REG_CAPABILITY] &= ~v_rs1;
        break;

    /* Extended agent ops */
    case 0x8A: /* AGENT_WAIT */
    case 0x8B: /* AGENT_SIGNAL */
    case 0x8C: /* AGENT_BROADCAST */
    case 0x8D: /* AGENT_SUBSCRIBE */
    case 0x8E: /* AGENT_UNSUBSCRIBE */
    case 0x8F: /* AGENT_QUERY */
        /* Delegate to agent runtime */
        return flux_vm_trap(vm, opcode);

    /* ==================================================================
     * I/O OPERATIONS (0x90 - 0x9F)
     * ================================================================== */

    case OP_IO_READ:
    case OP_IO_READ8:
    case OP_IO_READ16:
    case OP_IO_READ32:
    case OP_IO_WRITE:
    case OP_IO_WRITE8:
    case OP_IO_WRITE16:
    case OP_IO_WRITE32: {
        /* I/O port in rs1, data in rd (for writes) or rd (for reads) */
        /* Delegate to HAL in full implementation */
        uint64_t port = v_rs1;
        (void)port;
        vm->regs[FLUX_REG_ERROR_CODE] = 0;
        break;
    }

    case 0x98: /* IO_READ64 */
    case 0x99: /* IO_WRITE64 */
    case 0x9A: /* IO_DMA_READ */
    case 0x9B: /* IO_DMA_WRITE */
    case 0x9C: /* IO_IOCTL */
    case 0x9D: /* IO_MMAP */
    case 0x9E: /* IO_MUNMAP */
    case 0x9F: /* IO_POLL */
        vm->regs[FLUX_REG_ERROR_CODE] = 0;
        break;

    /* ==================================================================
     * FLUX-SPECIFIC OPERATIONS (0xA0 - 0xAF)
     * ================================================================== */

    case OP_TILE_LOAD:
        /* Load tile: rd=tile handle, rs1=tile name/index */
        flux_vm_set_reg(vm, rd, v_rs1 + 1); /* Placeholder */
        break;

    case OP_TILE_COMPOSE:
        /* Compose tiles: rs1=tile1, rs2=tile2, rd=result */
        flux_vm_set_reg(vm, rd, v_rs1 ^ v_rs2);
        break;

    case OP_TILE_EXEC:
        /* Execute tile: rs1=tile handle */
        break;

    case OP_REGION_CREATE: {
        /* Create memory region: rd=region_id output, rs1=size */
        int rid = flux_vm_region_create(vm, "dyn", (flux_size_t)v_rs1, false);
        flux_vm_set_reg(vm, rd, (uint64_t)rid);
        break;
    }

    case OP_REGION_DESTROY: {
        /* Destroy memory region: rs1=region_id */
        int rid = (int)(v_rs1 & 0xFF);
        flux_vm_region_destroy(vm, rid);
        break;
    }

    case OP_REGION_MAP:
        /* Map region to address space: rs1=region_id, rd=base address */
        if (v_rs1 < FLUX_REGION_MAX && vm->regions[v_rs1].active) {
            flux_vm_set_reg(vm, rd, (uint64_t)(uintptr_t)vm->regions[v_rs1].data);
        }
        break;

    case OP_ADAPT:
        /* Adaptive execution hint — no-op in interpreter */
        break;

    case OP_EVOLVE:
        /* Self-evolution trigger */
        /* In full implementation, triggers the evolution engine */
        break;

    case OP_MODULE_IMPORT:
        /* Import module: rs1=module name/index */
        break;

    case OP_MODULE_EXPORT:
        /* Export module */
        break;

    case OP_TRACE_ON:
        vm->tracing = true;
        vm->trace_idx = 0;
        break;

    case OP_TRACE_OFF:
        vm->tracing = false;
        break;

    case OP_PROF_START:
        /* Reset profiling counters */
        memset(vm->op_counts, 0, sizeof(vm->op_counts));
        vm->cycle_count = 0;
        vm->insn_count = 0;
        break;

    case OP_PROF_STOP:
        /* Profiling data is already accumulated in op_counts */
        break;

    case OP_GAS_INIT:
        /* Initialize gas counter: rd=initial gas */
        vm->regs[FLUX_REG_CYCLE_COUNT] = flux_vm_get_reg(vm, rd);
        break;

    case OP_GAS_CHECK: {
        /* Check gas remaining — trap if exhausted */
        uint64_t gas = vm->regs[FLUX_REG_CYCLE_COUNT];
        uint64_t cost = flux_vm_get_reg(vm, rd);
        if (gas < cost) {
            return flux_vm_trap(vm, 0xFE); /* Out of gas */
        }
        vm->regs[FLUX_REG_CYCLE_COUNT] = gas - cost;
        break;
    }

    /* ==================================================================
     * NATIVE / EXTENSION OPERATIONS (0xB0 - 0xB7)
     * ================================================================== */

    case OP_NATIVE_CALL:
        /* Call native function: rs1=function pointer */
        return flux_vm_trap(vm, opcode);

    case OP_NATIVE_LOAD:
        /* Load native library */
        return flux_vm_trap(vm, opcode);

    case OP_VEC_LOAD: {
        /* SIMD vector load: rd=destination, rs1=base addr */
        /* Simplified: load 4x32-bit values as two 64-bit registers */
        flux_vm_set_reg(vm, rd, v_rs1);
        break;
    }

    case OP_VEC_STORE: {
        /* SIMD vector store */
        break;
    }

    case OP_VEC_ADD: {
        /* SIMD vector add: rd = vec(rs1) + vec(rs2) */
        /* Simplified: add as 64-bit integers */
        flux_vm_set_reg(vm, rd, v_rs1 + v_rs2);
        break;
    }

    case OP_VEC_MUL: {
        /* SIMD vector multiply */
        flux_vm_set_reg(vm, rd, v_rs1 * v_rs2);
        break;
    }

    case OP_VEC_DOT: {
        /* SIMD dot product (simplified) */
        flux_vm_set_reg(vm, rd, v_rs1 * v_rs2);
        break;
    }

    case OP_VEC_SPLAT: {
        /* SIMD broadcast: replicate rd across all lanes */
        /* In simplified form, rd already holds the scalar */
        break;
    }

    /* ==================================================================
     * DEFAULT: Unknown opcode
     * ================================================================== */

    default:
        vm->state = FLUX_VM_ERROR;
        vm->error_code = 20; /* Unknown opcode */
        return FLUX_ERR_INVALID;
    }

    /* Update cycle count register */
    vm->regs[FLUX_REG_CYCLE_COUNT] = vm->cycle_count;

    return FLUX_OK;
}

/* ========================================================================
 * VM Run — Execute until halt/error/max_cycles
 * ======================================================================== */

flux_status_t flux_vm_run(flux_vm_t *vm, flux_size_t max_cycles) {
    if (vm == NULL) return FLUX_ERR_INVALID;

    vm->state = FLUX_VM_RUNNING;

    while (vm->state == FLUX_VM_RUNNING) {
        if (max_cycles > 0 && vm->cycle_count >= max_cycles) {
            vm->state = FLUX_VM_WAITING; /* Paused due to cycle limit */
            return FLUX_ERR_TIMEOUT;
        }

        flux_status_t rc = flux_vm_step(vm);
        if (rc != FLUX_OK) {
            return rc;
        }
    }

    return FLUX_OK;
}

/* ========================================================================
 * Debug: Print register state
 * ======================================================================== */

void flux_vm_dump_regs(flux_vm_t *vm) {
    if (vm == NULL) return;

    printf("=== FLUX VM Register State ===\n");
    printf("State: %s  PC: 0x%08lX  Cycle: %lu  Error: %u\n",
           vm_state_name(vm->state),
           (unsigned long)vm->pc,
           (unsigned long)vm->cycle_count,
           vm->error_code);

    printf("GP Registers:\n");
    printf("  R0(ZERO)=%016lX  R1(RA)  =%016lX  R2(SP)  =%016lX  R3(BP)  =%016lX\n",
           (unsigned long)vm->regs[0], (unsigned long)vm->regs[1],
           (unsigned long)vm->regs[2], (unsigned long)vm->regs[3]);
    printf("  R4(PC)   =%016lX  R5(FLAGS)=%016lX  R6(FP)  =%016lX  R7(T0)  =%016lX\n",
           (unsigned long)vm->regs[4], (unsigned long)vm->regs[5],
           (unsigned long)vm->regs[6], (unsigned long)vm->regs[7]);
    printf("  R8(S0)   =%016lX  R9(S1)  =%016lX  R10(S2) =%016lX  R11(S3) =%016lX\n",
           (unsigned long)vm->regs[8], (unsigned long)vm->regs[9],
           (unsigned long)vm->regs[10], (unsigned long)vm->regs[11]);
    printf("  R12(S4)  =%016lX  R13(S5) =%016lX  R14(S6) =%016lX  R15(S7) =%016lX\n",
           (unsigned long)vm->regs[12], (unsigned long)vm->regs[13],
           (unsigned long)vm->regs[14], (unsigned long)vm->regs[15]);
    printf("  R16(A0)  =%016lX  R17(A1) =%016lX  R18(A2) =%016lX  R19(A3) =%016lX\n",
           (unsigned long)vm->regs[16], (unsigned long)vm->regs[17],
           (unsigned long)vm->regs[18], (unsigned long)vm->regs[19]);
    printf("  R20(T1)  =%016lX  R21(T2) =%016lX  R22(T3) =%016lX  R23(T4) =%016lX\n",
           (unsigned long)vm->regs[20], (unsigned long)vm->regs[21],
           (unsigned long)vm->regs[22], (unsigned long)vm->regs[23]);

    printf("Special Registers:\n");
    printf("  R56(STATE)   =%016lX  R57(CYC)   =%016lX\n",
           (unsigned long)vm->regs[56], (unsigned long)vm->regs[57]);
    printf("  R58(REG_BASE)=%016lX  R59(REG_SZ) =%016lX\n",
           (unsigned long)vm->regs[58], (unsigned long)vm->regs[59]);
    printf("  R60(SYSCALL) =%016lX  R61(ERR)    =%016lX\n",
           (unsigned long)vm->regs[60], (unsigned long)vm->regs[61]);
    printf("  R62(CAP)     =%016lX  R63(TRAP_H) =%016lX\n",
           (unsigned long)vm->regs[62], (unsigned long)vm->regs[63]);

    printf("Stack: ptr=%lu/%lu  Call depth=%d\n",
           (unsigned long)vm->stack_ptr,
           (unsigned long)vm->stack_size,
           vm->call_depth);
}
