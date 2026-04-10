/*
 * FLUX OS — Opcode Tables Implementation
 *
 * Human-readable names and categories for all 184 FLUX opcodes.
 * These tables enable disassembly, debugging, profiling, and diagnostics.
 *
 * Copyright (c) 2025 SuperInstance
 */

#include <flux/opcodes.h>
#include <flux/vm.h>
#include <string.h>

/* ========================================================================
 * Opcode Name Table
 *
 * Indexed by opcode value (0x00 - 0xF7). Entries for undefined/reserved
 * slots contain "???". The table covers all 184 defined opcodes.
 * ======================================================================== */

#define MAX_OPCODE_VAL 0xB8  /* Highest defined opcode: OP_VEC_SPLAT = 0xB7 */

static const char *opcode_names[256] = {
    /* 0x00 - 0x0F: System Operations */
    [0x00] = "NOP",
    [0x01] = "HALT",
    [0x02] = "TRAP",
    [0x03] = "INVALID",

    /* 0x04 - 0x0F: Reserved system slots */
    [0x04] = "SYS_WAKE",       /* Hypothetical wake-from-idle */
    [0x05] = "SYS_PANIC",      /* Kernel panic */
    [0x06] = "SYS_DEBUG",      /* Debug breakpoint (extended) */
    [0x07] = "SYS_INFO",       /* VM info query */

    /* 0x08 - 0x0F: Extended system slots */
    [0x08] = "SYS_RESET",
    [0x09] = "SYS_SUSPEND",
    [0x0A] = "SYS_RESUME",
    [0x0B] = "SYS_YIELD",
    [0x0C] = "SYS_SLEEP",
    [0x0D] = "SYS_TIMED_WAIT",
    [0x0E] = "SYS_INTERRUPT",
    [0x0F] = "SYS_EXCEPTION",

    /* 0x10 - 0x1F: Arithmetic Operations */
    [0x10] = "IADD",
    [0x11] = "ISUB",
    [0x12] = "IMUL",
    [0x13] = "IDIV",
    [0x14] = "IMOD",
    [0x15] = "INEG",
    [0x16] = "IABS",
    [0x17] = "INC",
    [0x18] = "DEC",
    [0x19] = "FADD",
    [0x1A] = "FSUB",
    [0x1B] = "FMUL",
    [0x1C] = "FDIV",
    [0x1D] = "FNEG",
    [0x1E] = "I2F",
    [0x1F] = "F2I",

    /* 0x20 - 0x2F: Logical / Bitwise Operations */
    [0x20] = "IAND",
    [0x21] = "IOR",
    [0x22] = "IXOR",
    [0x23] = "INOT",
    [0x24] = "ISHL",
    [0x25] = "ISHR",
    [0x26] = "USHR",
    [0x27] = "ROTATE_L",
    [0x28] = "ROTATE_R",
    [0x29] = "POPCOUNT",
    [0x2A] = "CLZ",
    [0x2B] = "CTZ",
    [0x2C] = "BSWAP",
    [0x2D] = "ANDI",
    [0x2E] = "ORI",
    [0x2F] = "XORI",

    /* 0x30 - 0x3F: Comparison Operations */
    [0x30] = "CMP",
    [0x31] = "CMPI",
    [0x32] = "FCMP",
    [0x33] = "TEST",
    [0x34] = "TESTI",

    /* 0x35 - 0x3F: Extended comparison slots */
    [0x35] = "CMP_EQ",
    [0x36] = "CMP_NE",
    [0x37] = "CMP_LT",
    [0x38] = "CMP_GT",
    [0x39] = "CMP_LE",
    [0x3A] = "CMP_GE",
    [0x3B] = "FCMP_EQ",
    [0x3C] = "FCMP_LT",
    [0x3D] = "FCMP_GT",
    [0x3E] = "FCMP_LE",
    [0x3F] = "FCMP_GE",

    /* 0x40 - 0x4F: Branch / Jump Operations */
    [0x40] = "JMP",
    [0x41] = "JZ",
    [0x42] = "JNZ",
    [0x43] = "JE",
    [0x44] = "JNE",
    [0x45] = "JG",
    [0x46] = "JL",
    [0x47] = "JGE",
    [0x48] = "JLE",
    [0x49] = "JA",
    [0x4A] = "JB",
    [0x4B] = "JAE",
    [0x4C] = "JBE",
    [0x4D] = "JC",
    [0x4E] = "JO",
    [0x4F] = "LOOP",

    /* 0x50 - 0x5F: Memory Operations */
    [0x50] = "LOAD",
    [0x51] = "LOAD8",
    [0x52] = "LOAD16",
    [0x53] = "LOAD32",
    [0x54] = "STORE",
    [0x55] = "STORE8",
    [0x56] = "STORE16",
    [0x57] = "STORE32",
    [0x58] = "LEA",
    [0x59] = "CMPXCHG",
    [0x5A] = "ATOMIC_ADD",
    [0x5B] = "ATOMIC_SUB",
    [0x5C] = "FENCE",
    [0x5D] = "LOAD_RM",
    [0x5E] = "STORE_RM",

    /* 0x5F: Extended memory slot */
    [0x5F] = "MEMSET",

    /* 0x60 - 0x6F: Stack Operations */
    [0x60] = "PUSH",
    [0x61] = "POP",
    [0x62] = "PUSH_IMM",
    [0x63] = "DUP",
    [0x64] = "SWAP",
    [0x65] = "ENTER",
    [0x66] = "LEAVE",
    [0x67] = "PUSHA",
    [0x68] = "POPA",

    /* 0x69 - 0x6F: Extended stack slots */
    [0x69] = "PUSHF",
    [0x6A] = "POPF",
    [0x6B] = "PUSHR",
    [0x6C] = "POPR",
    [0x6D] = "STACK_CHK",
    [0x6E] = "STACK_ALLOC",
    [0x6F] = "STACK_FREE",

    /* 0x70 - 0x7F: Call / Return Operations */
    [0x70] = "CALL",
    [0x71] = "CALLI",
    [0x72] = "RET",
    [0x73] = "RETI",
    [0x74] = "SYSCALL",
    [0x75] = "CALL_REG",
    [0x76] = "TAILCALL",
    [0x77] = "ICALL",

    /* 0x78 - 0x7F: Extended call slots */
    [0x78] = "CALL_EXT",
    [0x79] = "CALL_NATIVE",
    [0x7A] = "CALL_AGENT",
    [0x7B] = "THROW",
    [0x7C] = "CATCH",
    [0x7D] = "FINALLY",
    [0x7E] = "EXCEPTION_RET",
    [0x7F] = "SET_HANDLER",

    /* 0x80 - 0x8F: Agent / A2A Operations */
    [0x80] = "DELEGATE",
    [0x81] = "TELL",
    [0x82] = "ASK",
    [0x83] = "REPLY",
    [0x84] = "BARRIER",
    [0x85] = "SPAWN",
    [0x86] = "YIELD_AGENT",
    [0x87] = "CAP_GRANT",
    [0x88] = "CAP_CHECK",
    [0x89] = "CAP_REVOKE",

    /* 0x8A - 0x8F: Extended agent slots */
    [0x8A] = "AGENT_WAIT",
    [0x8B] = "AGENT_SIGNAL",
    [0x8C] = "AGENT_BROADCAST",
    [0x8D] = "AGENT_SUBSCRIBE",
    [0x8E] = "AGENT_UNSUBSCRIBE",
    [0x8F] = "AGENT_QUERY",

    /* 0x90 - 0x9F: I/O Operations */
    [0x90] = "IO_READ",
    [0x91] = "IO_WRITE",
    [0x92] = "IO_READ8",
    [0x93] = "IO_WRITE8",
    [0x94] = "IO_READ16",
    [0x95] = "IO_WRITE16",
    [0x96] = "IO_READ32",
    [0x97] = "IO_WRITE32",

    /* 0x98 - 0x9F: Extended I/O slots */
    [0x98] = "IO_READ64",
    [0x99] = "IO_WRITE64",
    [0x9A] = "IO_DMA_READ",
    [0x9B] = "IO_DMA_WRITE",
    [0x9C] = "IO_IOCTL",
    [0x9D] = "IO_MMAP",
    [0x9E] = "IO_MUNMAP",
    [0x9F] = "IO_POLL",

    /* 0xA0 - 0xAF: FLUX-Specific Operations */
    [0xA0] = "TILE_LOAD",
    [0xA1] = "TILE_COMPOSE",
    [0xA2] = "TILE_EXEC",
    [0xA3] = "REGION_CREATE",
    [0xA4] = "REGION_DESTROY",
    [0xA5] = "REGION_MAP",
    [0xA6] = "ADAPT",
    [0xA7] = "EVOLVE",
    [0xA8] = "MODULE_IMPORT",
    [0xA9] = "MODULE_EXPORT",
    [0xAA] = "TRACE_ON",
    [0xAB] = "TRACE_OFF",
    [0xAC] = "PROF_START",
    [0xAD] = "PROF_STOP",
    [0xAE] = "GAS_INIT",
    [0xAF] = "GAS_CHECK",

    /* 0xB0 - 0xB7: Native / Extension Operations */
    [0xB0] = "NATIVE_CALL",
    [0xB1] = "NATIVE_LOAD",
    [0xB2] = "VEC_LOAD",
    [0xB3] = "VEC_STORE",
    [0xB4] = "VEC_ADD",
    [0xB5] = "VEC_MUL",
    [0xB6] = "VEC_DOT",
    [0xB7] = "VEC_SPLAT",
};

/* ========================================================================
 * Fallback name for unknown opcodes
 * ======================================================================== */

static char unknown_buf[16];

static const char *unknown_opcode_name(uint8_t opcode) {
    unknown_buf[0] = '?';
    unknown_buf[1] = '?';
    unknown_buf[2] = '?';
    unknown_buf[3] = '_';
    unknown_buf[4] = '0';
    unknown_buf[5] = 'x';

    /* Format as hex */
    const char hex[] = "0123456789ABCDEF";
    unknown_buf[6] = hex[(opcode >> 4) & 0xF];
    unknown_buf[7] = hex[opcode & 0xF];
    unknown_buf[8] = '\0';
    return unknown_buf;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

const char *flux_opcode_name(uint8_t opcode) {
    if (opcode_names[opcode] != NULL) {
        return opcode_names[opcode];
    }
    return unknown_opcode_name(opcode);
}

/* ========================================================================
 * Opcode Category Table
 *
 * Maps each opcode to its category. Categories are defined in vm.h
 * as flux_op_cat_t. We use a compact table indexed by opcode value,
 * returning the appropriate category enum.
 *
 * Category mapping:
 *   0x00-0x0F → FLUX_OP_CAT_SYSTEM
 *   0x10-0x1F → FLUX_OP_CAT_ARITH
 *   0x20-0x2F → FLUX_OP_CAT_LOGIC
 *   0x30-0x3F → FLUX_OP_CAT_COMPARE
 *   0x40-0x4F → FLUX_OP_CAT_BRANCH
 *   0x50-0x5F → FLUX_OP_CAT_MEMORY
 *   0x60-0x6F → FLUX_OP_CAT_STACK
 *   0x70-0x7F → FLUX_OP_CAT_CALL
 *   0x80-0x8F → FLUX_OP_CAT_AGENT
 *   0x90-0x9F → FLUX_OP_CAT_IO
 *   0xA0-0xAF → FLUX_OP_CAT_FLUX
 *   0xB0-0xEF → FLUX_OP_CAT_EXT
 *   0xF0-0xFF → FLUX_OP_CAT_NONE (reserved)
 * ======================================================================== */

static const flux_op_cat_t opcode_categories[256] = {
    /* 0x00 - 0x0F: System */
    [0x00] = FLUX_OP_CAT_SYSTEM,  /* NOP */
    [0x01] = FLUX_OP_CAT_SYSTEM,  /* HALT */
    [0x02] = FLUX_OP_CAT_SYSTEM,  /* TRAP */
    [0x03] = FLUX_OP_CAT_SYSTEM,  /* INVALID */
    [0x04] = FLUX_OP_CAT_SYSTEM,
    [0x05] = FLUX_OP_CAT_SYSTEM,
    [0x06] = FLUX_OP_CAT_SYSTEM,
    [0x07] = FLUX_OP_CAT_SYSTEM,
    [0x08] = FLUX_OP_CAT_SYSTEM,
    [0x09] = FLUX_OP_CAT_SYSTEM,
    [0x0A] = FLUX_OP_CAT_SYSTEM,
    [0x0B] = FLUX_OP_CAT_SYSTEM,
    [0x0C] = FLUX_OP_CAT_SYSTEM,
    [0x0D] = FLUX_OP_CAT_SYSTEM,
    [0x0E] = FLUX_OP_CAT_SYSTEM,
    [0x0F] = FLUX_OP_CAT_SYSTEM,

    /* 0x10 - 0x1F: Arithmetic */
    [0x10] = FLUX_OP_CAT_ARITH,   /* IADD */
    [0x11] = FLUX_OP_CAT_ARITH,   /* ISUB */
    [0x12] = FLUX_OP_CAT_ARITH,   /* IMUL */
    [0x13] = FLUX_OP_CAT_ARITH,   /* IDIV */
    [0x14] = FLUX_OP_CAT_ARITH,   /* IMOD */
    [0x15] = FLUX_OP_CAT_ARITH,   /* INEG */
    [0x16] = FLUX_OP_CAT_ARITH,   /* IABS */
    [0x17] = FLUX_OP_CAT_ARITH,   /* INC */
    [0x18] = FLUX_OP_CAT_ARITH,   /* DEC */
    [0x19] = FLUX_OP_CAT_ARITH,   /* FADD */
    [0x1A] = FLUX_OP_CAT_ARITH,   /* FSUB */
    [0x1B] = FLUX_OP_CAT_ARITH,   /* FMUL */
    [0x1C] = FLUX_OP_CAT_ARITH,   /* FDIV */
    [0x1D] = FLUX_OP_CAT_ARITH,   /* FNEG */
    [0x1E] = FLUX_OP_CAT_ARITH,   /* I2F */
    [0x1F] = FLUX_OP_CAT_ARITH,   /* F2I */

    /* 0x20 - 0x2F: Logic */
    [0x20] = FLUX_OP_CAT_LOGIC,   /* IAND */
    [0x21] = FLUX_OP_CAT_LOGIC,   /* IOR */
    [0x22] = FLUX_OP_CAT_LOGIC,   /* IXOR */
    [0x23] = FLUX_OP_CAT_LOGIC,   /* INOT */
    [0x24] = FLUX_OP_CAT_LOGIC,   /* ISHL */
    [0x25] = FLUX_OP_CAT_LOGIC,   /* ISHR */
    [0x26] = FLUX_OP_CAT_LOGIC,   /* USHR */
    [0x27] = FLUX_OP_CAT_LOGIC,   /* ROTATE_L */
    [0x28] = FLUX_OP_CAT_LOGIC,   /* ROTATE_R */
    [0x29] = FLUX_OP_CAT_LOGIC,   /* POPCOUNT */
    [0x2A] = FLUX_OP_CAT_LOGIC,   /* CLZ */
    [0x2B] = FLUX_OP_CAT_LOGIC,   /* CTZ */
    [0x2C] = FLUX_OP_CAT_LOGIC,   /* BSWAP */
    [0x2D] = FLUX_OP_CAT_LOGIC,   /* ANDI */
    [0x2E] = FLUX_OP_CAT_LOGIC,   /* ORI */
    [0x2F] = FLUX_OP_CAT_LOGIC,   /* XORI */

    /* 0x30 - 0x3F: Compare */
    [0x30] = FLUX_OP_CAT_COMPARE, /* CMP */
    [0x31] = FLUX_OP_CAT_COMPARE, /* CMPI */
    [0x32] = FLUX_OP_CAT_COMPARE, /* FCMP */
    [0x33] = FLUX_OP_CAT_COMPARE, /* TEST */
    [0x34] = FLUX_OP_CAT_COMPARE, /* TESTI */
    [0x35] = FLUX_OP_CAT_COMPARE,
    [0x36] = FLUX_OP_CAT_COMPARE,
    [0x37] = FLUX_OP_CAT_COMPARE,
    [0x38] = FLUX_OP_CAT_COMPARE,
    [0x39] = FLUX_OP_CAT_COMPARE,
    [0x3A] = FLUX_OP_CAT_COMPARE,
    [0x3B] = FLUX_OP_CAT_COMPARE,
    [0x3C] = FLUX_OP_CAT_COMPARE,
    [0x3D] = FLUX_OP_CAT_COMPARE,
    [0x3E] = FLUX_OP_CAT_COMPARE,
    [0x3F] = FLUX_OP_CAT_COMPARE,

    /* 0x40 - 0x4F: Branch */
    [0x40] = FLUX_OP_CAT_BRANCH,  /* JMP */
    [0x41] = FLUX_OP_CAT_BRANCH,  /* JZ */
    [0x42] = FLUX_OP_CAT_BRANCH,  /* JNZ */
    [0x43] = FLUX_OP_CAT_BRANCH,  /* JE */
    [0x44] = FLUX_OP_CAT_BRANCH,  /* JNE */
    [0x45] = FLUX_OP_CAT_BRANCH,  /* JG */
    [0x46] = FLUX_OP_CAT_BRANCH,  /* JL */
    [0x47] = FLUX_OP_CAT_BRANCH,  /* JGE */
    [0x48] = FLUX_OP_CAT_BRANCH,  /* JLE */
    [0x49] = FLUX_OP_CAT_BRANCH,  /* JA */
    [0x4A] = FLUX_OP_CAT_BRANCH,  /* JB */
    [0x4B] = FLUX_OP_CAT_BRANCH,  /* JAE */
    [0x4C] = FLUX_OP_CAT_BRANCH,  /* JBE */
    [0x4D] = FLUX_OP_CAT_BRANCH,  /* JC */
    [0x4E] = FLUX_OP_CAT_BRANCH,  /* JO */
    [0x4F] = FLUX_OP_CAT_BRANCH,  /* LOOP */

    /* 0x50 - 0x5F: Memory */
    [0x50] = FLUX_OP_CAT_MEMORY,  /* LOAD */
    [0x51] = FLUX_OP_CAT_MEMORY,  /* LOAD8 */
    [0x52] = FLUX_OP_CAT_MEMORY,  /* LOAD16 */
    [0x53] = FLUX_OP_CAT_MEMORY,  /* LOAD32 */
    [0x54] = FLUX_OP_CAT_MEMORY,  /* STORE */
    [0x55] = FLUX_OP_CAT_MEMORY,  /* STORE8 */
    [0x56] = FLUX_OP_CAT_MEMORY,  /* STORE16 */
    [0x57] = FLUX_OP_CAT_MEMORY,  /* STORE32 */
    [0x58] = FLUX_OP_CAT_MEMORY,  /* LEA */
    [0x59] = FLUX_OP_CAT_MEMORY,  /* CMPXCHG */
    [0x5A] = FLUX_OP_CAT_MEMORY,  /* ATOMIC_ADD */
    [0x5B] = FLUX_OP_CAT_MEMORY,  /* ATOMIC_SUB */
    [0x5C] = FLUX_OP_CAT_MEMORY,  /* FENCE */
    [0x5D] = FLUX_OP_CAT_MEMORY,  /* LOAD_RM */
    [0x5E] = FLUX_OP_CAT_MEMORY,  /* STORE_RM */
    [0x5F] = FLUX_OP_CAT_MEMORY,

    /* 0x60 - 0x6F: Stack */
    [0x60] = FLUX_OP_CAT_STACK,   /* PUSH */
    [0x61] = FLUX_OP_CAT_STACK,   /* POP */
    [0x62] = FLUX_OP_CAT_STACK,   /* PUSH_IMM */
    [0x63] = FLUX_OP_CAT_STACK,   /* DUP */
    [0x64] = FLUX_OP_CAT_STACK,   /* SWAP */
    [0x65] = FLUX_OP_CAT_STACK,   /* ENTER */
    [0x66] = FLUX_OP_CAT_STACK,   /* LEAVE */
    [0x67] = FLUX_OP_CAT_STACK,   /* PUSHA */
    [0x68] = FLUX_OP_CAT_STACK,   /* POPA */
    [0x69] = FLUX_OP_CAT_STACK,
    [0x6A] = FLUX_OP_CAT_STACK,
    [0x6B] = FLUX_OP_CAT_STACK,
    [0x6C] = FLUX_OP_CAT_STACK,
    [0x6D] = FLUX_OP_CAT_STACK,
    [0x6E] = FLUX_OP_CAT_STACK,
    [0x6F] = FLUX_OP_CAT_STACK,

    /* 0x70 - 0x7F: Call */
    [0x70] = FLUX_OP_CAT_CALL,    /* CALL */
    [0x71] = FLUX_OP_CAT_CALL,    /* CALLI */
    [0x72] = FLUX_OP_CAT_CALL,    /* RET */
    [0x73] = FLUX_OP_CAT_CALL,    /* RETI */
    [0x74] = FLUX_OP_CAT_CALL,    /* SYSCALL */
    [0x75] = FLUX_OP_CAT_CALL,    /* CALL_REG */
    [0x76] = FLUX_OP_CAT_CALL,    /* TAILCALL */
    [0x77] = FLUX_OP_CAT_CALL,    /* ICALL */
    [0x78] = FLUX_OP_CAT_CALL,
    [0x79] = FLUX_OP_CAT_CALL,
    [0x7A] = FLUX_OP_CAT_CALL,
    [0x7B] = FLUX_OP_CAT_CALL,
    [0x7C] = FLUX_OP_CAT_CALL,
    [0x7D] = FLUX_OP_CAT_CALL,
    [0x7E] = FLUX_OP_CAT_CALL,
    [0x7F] = FLUX_OP_CAT_CALL,

    /* 0x80 - 0x8F: Agent */
    [0x80] = FLUX_OP_CAT_AGENT,   /* DELEGATE */
    [0x81] = FLUX_OP_CAT_AGENT,   /* TELL */
    [0x82] = FLUX_OP_CAT_AGENT,   /* ASK */
    [0x83] = FLUX_OP_CAT_AGENT,   /* REPLY */
    [0x84] = FLUX_OP_CAT_AGENT,   /* BARRIER */
    [0x85] = FLUX_OP_CAT_AGENT,   /* SPAWN */
    [0x86] = FLUX_OP_CAT_AGENT,   /* YIELD_AGENT */
    [0x87] = FLUX_OP_CAT_AGENT,   /* CAP_GRANT */
    [0x88] = FLUX_OP_CAT_AGENT,   /* CAP_CHECK */
    [0x89] = FLUX_OP_CAT_AGENT,   /* CAP_REVOKE */
    [0x8A] = FLUX_OP_CAT_AGENT,
    [0x8B] = FLUX_OP_CAT_AGENT,
    [0x8C] = FLUX_OP_CAT_AGENT,
    [0x8D] = FLUX_OP_CAT_AGENT,
    [0x8E] = FLUX_OP_CAT_AGENT,
    [0x8F] = FLUX_OP_CAT_AGENT,

    /* 0x90 - 0x9F: I/O */
    [0x90] = FLUX_OP_CAT_IO,      /* IO_READ */
    [0x91] = FLUX_OP_CAT_IO,      /* IO_WRITE */
    [0x92] = FLUX_OP_CAT_IO,      /* IO_READ8 */
    [0x93] = FLUX_OP_CAT_IO,      /* IO_WRITE8 */
    [0x94] = FLUX_OP_CAT_IO,      /* IO_READ16 */
    [0x95] = FLUX_OP_CAT_IO,      /* IO_WRITE16 */
    [0x96] = FLUX_OP_CAT_IO,      /* IO_READ32 */
    [0x97] = FLUX_OP_CAT_IO,      /* IO_WRITE32 */
    [0x98] = FLUX_OP_CAT_IO,
    [0x99] = FLUX_OP_CAT_IO,
    [0x9A] = FLUX_OP_CAT_IO,
    [0x9B] = FLUX_OP_CAT_IO,
    [0x9C] = FLUX_OP_CAT_IO,
    [0x9D] = FLUX_OP_CAT_IO,
    [0x9E] = FLUX_OP_CAT_IO,
    [0x9F] = FLUX_OP_CAT_IO,

    /* 0xA0 - 0xAF: FLUX */
    [0xA0] = FLUX_OP_CAT_FLUX,    /* TILE_LOAD */
    [0xA1] = FLUX_OP_CAT_FLUX,    /* TILE_COMPOSE */
    [0xA2] = FLUX_OP_CAT_FLUX,    /* TILE_EXEC */
    [0xA3] = FLUX_OP_CAT_FLUX,    /* REGION_CREATE */
    [0xA4] = FLUX_OP_CAT_FLUX,    /* REGION_DESTROY */
    [0xA5] = FLUX_OP_CAT_FLUX,    /* REGION_MAP */
    [0xA6] = FLUX_OP_CAT_FLUX,    /* ADAPT */
    [0xA7] = FLUX_OP_CAT_FLUX,    /* EVOLVE */
    [0xA8] = FLUX_OP_CAT_FLUX,    /* MODULE_IMPORT */
    [0xA9] = FLUX_OP_CAT_FLUX,    /* MODULE_EXPORT */
    [0xAA] = FLUX_OP_CAT_FLUX,    /* TRACE_ON */
    [0xAB] = FLUX_OP_CAT_FLUX,    /* TRACE_OFF */
    [0xAC] = FLUX_OP_CAT_FLUX,    /* PROF_START */
    [0xAD] = FLUX_OP_CAT_FLUX,    /* PROF_STOP */
    [0xAE] = FLUX_OP_CAT_FLUX,    /* GAS_INIT */
    [0xAF] = FLUX_OP_CAT_FLUX,    /* GAS_CHECK */

    /* 0xB0 - 0xBF: Extension */
    [0xB0] = FLUX_OP_CAT_EXT,     /* NATIVE_CALL */
    [0xB1] = FLUX_OP_CAT_EXT,     /* NATIVE_LOAD */
    [0xB2] = FLUX_OP_CAT_EXT,     /* VEC_LOAD */
    [0xB3] = FLUX_OP_CAT_EXT,     /* VEC_STORE */
    [0xB4] = FLUX_OP_CAT_EXT,     /* VEC_ADD */
    [0xB5] = FLUX_OP_CAT_EXT,     /* VEC_MUL */
    [0xB6] = FLUX_OP_CAT_EXT,     /* VEC_DOT */
    [0xB7] = FLUX_OP_CAT_EXT,     /* VEC_SPLAT */
};

/* ========================================================================
 * Category Name Strings
 * ======================================================================== */

static const char *category_names[] = {
    [FLUX_OP_CAT_NONE]    = "NONE",
    [FLUX_OP_CAT_ARITH]   = "ARITH",
    [FLUX_OP_CAT_LOGIC]   = "LOGIC",
    [FLUX_OP_CAT_COMPARE] = "COMPARE",
    [FLUX_OP_CAT_BRANCH]  = "BRANCH",
    [FLUX_OP_CAT_MEMORY]  = "MEMORY",
    [FLUX_OP_CAT_STACK]   = "STACK",
    [FLUX_OP_CAT_CALL]    = "CALL",
    [FLUX_OP_CAT_AGENT]   = "AGENT",
    [FLUX_OP_CAT_IO]      = "IO",
    [FLUX_OP_CAT_SYSTEM]  = "SYSTEM",
    [FLUX_OP_CAT_FLUX]    = "FLUX",
    [FLUX_OP_CAT_EXT]     = "EXT",
};

#define NUM_CATEGORIES (sizeof(category_names) / sizeof(category_names[0]))

/* ========================================================================
 * Public: Category name for an opcode
 * ======================================================================== */

const char *flux_opcode_category_name(uint8_t opcode) {
    if (opcode < 0xF0) {
        return category_names[opcode_categories[opcode]];
    }
    return "RESERVED";
}

/* ========================================================================
 * Utility: Check if opcode is valid
 * ======================================================================== */

bool flux_opcode_is_valid(uint8_t opcode) {
    /* Valid opcodes are 0x00-0xB7 (defined) plus some extension range */
    if (opcode <= 0xB7) return true;
    /* 0xB8-0xEF: extended range (implementation-defined) */
    if (opcode <= 0xEF) return true;
    /* 0xF0-0xFF: reserved, invalid */
    return false;
}

/* ========================================================================
 * Utility: Get instruction format hint for an opcode
 *
 * Returns a character indicating the likely encoding format:
 *   'A' = A-type (opcode, rd, rs1, rs2)
 *   'B' = B-type (opcode, rd, imm16)
 *   'C' = C-type (opcode, target24)
 *   'D' = D-type (opcode, rd, rs1, offset)
 *   'E' = E-type (opcode, imm32)
 *   '?' = Unknown/variable
 * ======================================================================== */

char flux_opcode_format_hint(uint8_t opcode) {
    /* System */
    if (opcode <= 0x03) return 'E';
    if (opcode <= 0x0F) return 'E';

    /* Arithmetic: A-type for binary, B for unary */
    switch (opcode) {
        case OP_IADD: case OP_ISUB: case OP_IMUL: case OP_IDIV:
        case OP_IMOD: case OP_FADD: case OP_FSUB: case OP_FMUL:
        case OP_FDIV:
            return 'A';
        case OP_INEG: case OP_IABS: case OP_FNEG:
            return 'A';
        case OP_INC: case OP_DEC:
            return 'A';
        case OP_I2F: case OP_F2I:
            return 'A';
        default:
            break;
    }

    /* Logic */
    switch (opcode) {
        case OP_IAND: case OP_IOR: case OP_IXOR:
        case OP_ISHL: case OP_ISHR: case OP_USHR:
        case OP_ROTATE_L: case OP_ROTATE_R:
            return 'A';
        case OP_INOT: case OP_POPCOUNT: case OP_CLZ:
        case OP_CTZ: case OP_BSWAP:
            return 'A';
        case OP_ANDI: case OP_ORI: case OP_XORI:
            return 'B';
        default:
            break;
    }

    /* Compare */
    switch (opcode) {
        case OP_CMP: case OP_FCMP: case OP_TEST:
            return 'A';
        case OP_CMPI: case OP_TESTI:
            return 'B';
        default:
            break;
    }

    /* Branch: C-type (target) */
    if (opcode >= 0x40 && opcode <= 0x4F) return 'C';
    if (opcode == OP_LOOP) return 'C';

    /* Memory: D-type */
    if (opcode >= 0x50 && opcode <= 0x5C) return 'D';
    if (opcode == OP_LOAD_RM || opcode == OP_STORE_RM) return 'D';

    /* Stack */
    switch (opcode) {
        case OP_PUSH: case OP_POP: case OP_DUP: case OP_SWAP:
        case OP_ENTER: case OP_LEAVE: case OP_PUSHA: case OP_POPA:
            return 'A';
        case OP_PUSH_IMM:
            return 'B';
        default:
            break;
    }

    /* Call */
    switch (opcode) {
        case OP_CALL: case OP_CALL_REG: case OP_ICALL:
        case OP_TAILCALL:
            return 'A';
        case OP_CALLI:
            return 'C';
        case OP_RET: case OP_RETI:
            return 'A';
        case OP_SYSCALL:
            return 'E';
        default:
            break;
    }

    /* Agent */
    if (opcode >= 0x80 && opcode <= 0x89) return 'E';

    /* I/O */
    if (opcode >= 0x90 && opcode <= 0x97) return 'D';

    /* FLUX */
    if (opcode >= 0xA0 && opcode <= 0xAF) return 'E';

    /* Extension */
    if (opcode >= 0xB0 && opcode <= 0xB7) return 'A';

    return '?';
}

/* ========================================================================
 * Utility: Count defined opcodes
 * ======================================================================== */

int flux_opcode_count_defined(void) {
    int count = 0;
    for (int i = 0; i < 256; i++) {
        if (opcode_names[i] != NULL) {
            count++;
        }
    }
    return count;
}

/* ========================================================================
 * Utility: Iterate opcodes in a category
 *
 * Calls callback for each opcode in the given category.
 * Returns total count of opcodes in the category.
 * ======================================================================== */

int flux_opcode_iterate_category(flux_op_cat_t category,
                                  void (*callback)(uint8_t opcode, const char *name, void *ctx),
                                  void *ctx) {
    int count = 0;
    for (int i = 0; i < 256; i++) {
        if (opcode_categories[i] == category && opcode_names[i] != NULL) {
            if (callback) {
                callback((uint8_t)i, opcode_names[i], ctx);
            }
            count++;
        }
    }
    return count;
}
