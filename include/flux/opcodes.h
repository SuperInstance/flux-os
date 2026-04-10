/*
 * FLUX OS — Opcodes Definition
 *
 * Complete opcode table for the FLUX bytecode VM. These opcodes are
 * shared across Python, Rust, and C implementations of FLUX.
 *
 * Instruction formats:
 *   A-type: [opcode] [rd] [rs1] [rs2]         — Register-Register
 *   B-type: [opcode] [rd] [imm16]              — Register-Immediate
 *   C-type: [opcode] [rs1] [rs2]              — Compare/Branch (flags)
 *   D-type: [opcode] [rd] [rs1] [offset]      — Memory (load/store)
 *   E-type: [opcode] [imm32]                   — System/Extended
 */

#ifndef FLUX_OPCODES_H
#define FLUX_OPCODES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Instruction Categories
 * ======================================================================== */

#define OP_CAT_SYSTEM     0x00
#define OP_CAT_ARITH      0x10
#define OP_CAT_LOGIC      0x20
#define OP_CAT_COMPARE    0x30
#define OP_CAT_BRANCH     0x40
#define OP_CAT_MEMORY     0x50
#define OP_CAT_STACK      0x60
#define OP_CAT_CALL       0x70
#define OP_CAT_AGENT      0x80
#define OP_CAT_IO         0x90
#define OP_CAT_FLUX       0xA0
#define OP_CAT_NATIVE     0xB0
#define OP_CAT_RESERVED   0xF0

/* ========================================================================
 * System Operations (0x00 - 0x0F)
 * ======================================================================== */

#define OP_NOP            0x00  /* No operation */
#define OP_HALT           0x01  /* Halt the VM */
#define OP_TRAP           0x02  /* Trap / breakpoint */
#define OP_INVALID        0x03  /* Invalid instruction (error) */

/* ========================================================================
 * Arithmetic Operations (0x10 - 0x1F)
 * Format A: IADD rd, rs1, rs2   — rd = rs1 + rs2
 * Format B: IADDI rd, rs1, imm  — rd = rs1 + imm
 * ======================================================================== */

#define OP_IADD           0x10  /* rd = rs1 + rs2 (integer) */
#define OP_ISUB           0x11  /* rd = rs1 - rs2 */
#define OP_IMUL           0x12  /* rd = rs1 * rs2 */
#define OP_IDIV           0x13  /* rd = rs1 / rs2 */
#define OP_IMOD           0x14  /* rd = rs1 % rs2 */
#define OP_INEG           0x15  /* rd = -rs1 */
#define OP_IABS           0x16  /* rd = |rs1| */
#define OP_INC            0x17  /* rd = rd + 1 */
#define OP_DEC            0x18  /* rd = rd - 1 */
#define OP_FADD           0x19  /* rd = f(rs1) + f(rs2) */
#define OP_FSUB           0x1A  /* rd = f(rs1) - f(rs2) */
#define OP_FMUL           0x1B  /* rd = f(rs1) * f(rs2) */
#define OP_FDIV           0x1C  /* rd = f(rs1) / f(rs2) */
#define OP_FNEG           0x1D  /* rd = -f(rs1) */
#define OP_I2F            0x1E  /* rd = (float)rs1 */
#define OP_F2I            0x1F  /* rd = (int)rs1 */

/* ========================================================================
 * Logical / Bitwise Operations (0x20 - 0x2F)
 * Format A: IAND rd, rs1, rs2
 * ======================================================================== */

#define OP_IAND           0x20  /* rd = rs1 & rs2 */
#define OP_IOR            0x21  /* rd = rs1 | rs2 */
#define OP_IXOR           0x22  /* rd = rs1 ^ rs2 */
#define OP_INOT           0x23  /* rd = ~rs1 */
#define OP_ISHL           0x24  /* rd = rs1 << rs2 */
#define OP_ISHR           0x25  /* rd = rs1 >> rs2 (arithmetic) */
#define OP_USHR           0x26  /* rd = rs1 >> rs2 (logical) */
#define OP_ROTATE_L       0x27  /* rd = rotate_left(rs1, rs2) */
#define OP_ROTATE_R       0x28  /* rd = rotate_right(rs1, rs2) */
#define OP_POPCOUNT       0x29  /* rd = popcount(rs1) */
#define OP_CLZ            0x2A  /* rd = count_leading_zeros(rs1) */
#define OP_CTZ            0x2B  /* rd = count_trailing_zeros(rs1) */
#define OP_BSWAP          0x2C  /* rd = byte_swap(rs1) */
#define OP_ANDI           0x2D  /* rd = rs1 & imm */
#define OP_ORI            0x2E  /* rd = rs1 | imm */
#define OP_XORI           0x2F  /* rd = rs1 ^ imm */

/* ========================================================================
 * Comparison Operations (0x30 - 0x3F)
 * Sets flags register; no destination
 * Format A: CMP rs1, rs2
 * ======================================================================== */

#define OP_CMP            0x30  /* Compare rs1 vs rs2, set flags */
#define OP_CMPI           0x31  /* Compare rs1 vs imm */
#define OP_FCMP           0x32  /* Float compare */
#define OP_TEST           0x33  /* TEST rs1, rs2 (AND for flags) */
#define OP_TESTI          0x34  /* TEST rs1, imm */

/* ========================================================================
 * Branch / Jump Operations (0x40 - 0x4F)
 * Format C: JE target (conditional on flags)
 * ======================================================================== */

#define OP_JMP            0x40  /* Unconditional jump to target */
#define OP_JZ             0x41  /* Jump if zero */
#define OP_JNZ            0x42  /* Jump if not zero */
#define OP_JE             0x43  /* Jump if equal */
#define OP_JNE            0x44  /* Jump if not equal */
#define OP_JG             0x45  /* Jump if greater (signed) */
#define OP_JL             0x46  /* Jump if less (signed) */
#define OP_JGE            0x47  /* Jump if greater or equal */
#define OP_JLE            0x48  /* Jump if less or equal */
#define OP_JA             0x49  /* Jump if above (unsigned) */
#define OP_JB             0x4A  /* Jump if below (unsigned) */
#define OP_JAE            0x4B  /* Jump if above or equal (unsigned) */
#define OP_JBE            0x4C  /* Jump if below or equal (unsigned) */
#define OP_JC             0x4D  /* Jump if carry */
#define OP_JO             0x4E  /* Jump if overflow */
#define OP_LOOP           0x4F  /* Decrement RCX, jump if RCX != 0 */

/* ========================================================================
 * Memory Operations (0x50 - 0x5F)
 * Format D: LOAD rd, [rs1 + offset]
 * ======================================================================== */

#define OP_LOAD           0x50  /* rd = mem[rs1 + offset] (64-bit) */
#define OP_LOAD8          0x51  /* rd = mem8[rs1 + offset] */
#define OP_LOAD16         0x52  /* rd = mem16[rs1 + offset] */
#define OP_LOAD32         0x53  /* rd = mem32[rs1 + offset] */
#define OP_STORE          0x54  /* mem[rs1 + offset] = rd (64-bit) */
#define OP_STORE8         0x55  /* mem8[rs1 + offset] = rd */
#define OP_STORE16        0x56  /* mem16[rs1 + offset] = rd */
#define OP_STORE32        0x57  /* mem32[rs1 + offset] = rd */
#define OP_LEA            0x58  /* rd = rs1 + offset */
#define OP_CMPXCHG        0x59  /* Atomic compare and exchange */
#define OP_ATOMIC_ADD     0x5A  /* Atomic fetch and add */
#define OP_ATOMIC_SUB     0x5B  /* Atomic fetch and sub */
#define OP_FENCE          0x5C  /* Memory fence */
#define OP_LOAD_RM        0x5D  /* Load from region + offset */
#define OP_STORE_RM       0x5E  /* Store to region + offset */

/* ========================================================================
 * Stack Operations (0x60 - 0x6F)
 * ======================================================================== */

#define OP_PUSH           0x60  /* Push rd onto stack */
#define OP_POP            0x61  /* Pop top of stack into rd */
#define OP_PUSH_IMM       0x62  /* Push immediate onto stack */
#define OP_DUP            0x63  /* Duplicate top of stack */
#define OP_SWAP           0x64  /* Swap top two stack elements */
#define OP_ENTER          0x65  /* Stack frame setup (imm bytes) */
#define OP_LEAVE          0x66  /* Stack frame teardown */
#define OP_PUSHA          0x67  /* Push all registers */
#define OP_POPA           0x68  /* Pop all registers */

/* ========================================================================
 * Call / Return Operations (0x70 - 0x7F)
 * ======================================================================== */

#define OP_CALL           0x70  /* Call subroutine (target in rs1 or imm) */
#define OP_CALLI          0x71  /* Call immediate address */
#define OP_RET            0x72  /* Return from subroutine */
#define OP_RETI           0x73  /* Return from interrupt */
#define OP_SYSCALL        0x74  /* System call (number in R60) */
#define OP_CALL_REG       0x75  /* Call address in register */
#define OP_TAILCALL       0x76  /* Tail call optimization */
#define OP_ICALL          0x77  /* Indirect call via function pointer */

/* ========================================================================
 * Agent / A2A Operations (0x80 - 0x8F)
 * ======================================================================== */

#define OP_DELEGATE       0x80  /* Delegate task to agent */
#define OP_TELL           0x81  /* Send message to agent (fire-and-forget) */
#define OP_ASK            0x82  /* Ask agent for result (request-reply) */
#define OP_REPLY          0x83  /* Reply to ASK */
#define OP_BARRIER        0x84  /* Synchronization barrier */
#define OP_SPAWN          0x85  /* Spawn new agent */
#define OP_YIELD_AGENT    0x86  /* Yield to agent scheduler */
#define OP_CAP_GRANT      0x87  /* Grant capability */
#define OP_CAP_CHECK      0x88  /* Check capability */
#define OP_CAP_REVOKE     0x89  /* Revoke capability */

/* ========================================================================
 * I/O Operations (0x90 - 0x9F)
 * ======================================================================== */

#define OP_IO_READ        0x90  /* Read from I/O port/device */
#define OP_IO_WRITE       0x91  /* Write to I/O port/device */
#define OP_IO_READ8       0x92  /* 8-bit I/O read */
#define OP_IO_WRITE8      0x93  /* 8-bit I/O write */
#define OP_IO_READ16      0x94  /* 16-bit I/O read */
#define OP_IO_WRITE16     0x95  /* 16-bit I/O write */
#define OP_IO_READ32      0x96  /* 32-bit I/O read */
#define OP_IO_WRITE32     0x97  /* 32-bit I/O write */

/* ========================================================================
 * FLUX-Specific Operations (0xA0 - 0xAF)
 * ======================================================================== */

#define OP_TILE_LOAD      0xA0  /* Load tile (module) */
#define OP_TILE_COMPOSE   0xA1  /* Compose tiles */
#define OP_TILE_EXEC      0xA2  /* Execute tile */
#define OP_REGION_CREATE  0xA3  /* Create memory region */
#define OP_REGION_DESTROY 0xA4  /* Destroy memory region */
#define OP_REGION_MAP     0xA5  /* Map region to address */
#define OP_ADAPT          0xA6  /* Hint: adapt execution strategy */
#define OP_EVOLVE         0xA7  /* Self-evolution trigger */
#define OP_MODULE_IMPORT  0xA8  /* Import module */
#define OP_MODULE_EXPORT  0xA9  /* Export module */
#define OP_TRACE_ON       0xAA  /* Enable execution tracing */
#define OP_TRACE_OFF      0xAB  /* Disable execution tracing */
#define OP_PROF_START     0xAC  /* Start profiling */
#define OP_PROF_STOP      0xAD  /* Stop profiling */
#define OP_GAS_INIT       0xAE  /* Initialize gas counter */
#define OP_GAS_CHECK      0xAF  /* Check gas remaining */

/* ========================================================================
 * Native / Extension Operations (0xB0 - 0xEF)
 * ======================================================================== */

#define OP_NATIVE_CALL    0xB0  /* Call native function */
#define OP_NATIVE_LOAD    0xB1  /* Load native library */
#define OP_VEC_LOAD       0xB2  /* SIMD vector load */
#define OP_VEC_STORE      0xB3  /* SIMD vector store */
#define OP_VEC_ADD        0xB4  /* SIMD vector add */
#define OP_VEC_MUL        0xB5  /* SIMD vector multiply */
#define OP_VEC_DOT        0xB6  /* SIMD dot product */
#define OP_VEC_SPLAT      0xB7  /* SIMD broadcast scalar */

/* Extended opcodes 0xC0-0xEF reserved for future use */

/* ========================================================================
 * Instruction Encoding Helpers
 * ======================================================================== */

/* Encode A-type instruction: [opcode] [rd] [rs1] [rs2] */
static inline uint32_t flux_encode_a(uint8_t opcode, uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return ((uint32_t)opcode << 24) | ((uint32_t)rd << 16) | ((uint32_t)rs1 << 8) | (uint32_t)rs2;
}

/* Encode B-type instruction: [opcode] [rd] [imm16] */
static inline uint32_t flux_encode_b(uint8_t opcode, uint8_t rd, int16_t imm) {
    return ((uint32_t)opcode << 24) | ((uint32_t)rd << 16) | ((uint16_t)imm);
}

/* Encode C-type instruction: [opcode] [target24] */
static inline uint32_t flux_encode_c(uint8_t opcode, uint32_t target) {
    return ((uint32_t)opcode << 24) | (target & 0x00FFFFFF);
}

/* Encode E-type instruction: [opcode] [imm32] */
static inline uint32_t flux_encode_e(uint8_t opcode, uint32_t imm) {
    return ((uint32_t)opcode << 24) | imm;
}

/* Decode helpers */
static inline uint8_t flux_decode_opcode(uint32_t insn) {
    return (uint8_t)(insn >> 24);
}
static inline uint8_t flux_decode_rd(uint32_t insn) {
    return (uint8_t)((insn >> 16) & 0xFF);
}
static inline uint8_t flux_decode_rs1(uint32_t insn) {
    return (uint8_t)((insn >> 8) & 0xFF);
}
static inline uint8_t flux_decode_rs2(uint32_t insn) {
    return (uint8_t)(insn & 0xFF);
}
static inline int16_t flux_decode_imm16(uint32_t insn) {
    return (int16_t)(insn & 0xFFFF);
}
static inline uint32_t flux_decode_target(uint32_t insn) {
    return insn & 0x00FFFFFF;
}

/* Total opcode count */
#define FLUX_OPCODE_COUNT  184

/* Opcode name lookup */
const char *flux_opcode_name(uint8_t opcode);
const char *flux_opcode_category_name(uint8_t opcode);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_OPCODES_H */
