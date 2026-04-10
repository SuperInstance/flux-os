/*
 * FLUX OS — Self-Compiler
 *
 * The Self-Compiler is the heart of what makes FLUX OS "intelligently
 * hardware agnostic." It can:
 *
 *   1. Parse FLUX.MD (markdown) specifications into an SSA IR (FIR)
 *   2. Generate C source code from FIR — targeting any C compiler
 *   3. Generate FLUX bytecode from FIR — for the built-in VM
 *   4. Generate native assembly from FIR — using hardware info from HAL
 *   5. Compile itself — the OS can rebuild its own components
 *   6. Act as a developer — given a natural language description,
 *      it generates working code (DEVCODE syscall)
 *
 * The compilation pipeline:
 *
 *   FLUX.MD → Lexer → Parser → AST → FIR (SSA) → Backend → Output
 *
 * Backends:
 *   - C Codegen:     FIR → C source (portable, any C compiler)
 *   - Bytecode Codegen: FIR → FLUX bytecode (built-in VM)
 *   - Native Codegen:   FIR → x86_64/ARM64/RISC-V assembly (HAL-targeted)
 *   - IR Codegen:       FIR → FIR (optimization passes)
 */

#ifndef FLUX_COMPILER_H
#define FLUX_COMPILER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "flux/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Source Language Types
 * ======================================================================== */

typedef enum {
    FLUX_LANG_FLUX_MD = 0,    /* FLUX.MD markdown format */
    FLUX_LANG_C        = 1,    /* C source */
    FLUX_LANG_ASSEMBLY = 2,    /* Assembly */
    FLUX_LANG_BYTECODE = 3,    /* Raw FLUX bytecode */
    FLUX_LANG_FIR      = 4,    /* FIR IR text format */
} flux_lang_t;

/* ========================================================================
 * Compilation Target
 * ======================================================================== */

typedef enum {
    FLUX_TARGET_BYTECODE = 0,  /* FLUX bytecode for VM */
    FLUX_TARGET_C        = 1,  /* C source code */
    FLUX_TARGET_NATIVE   = 2,  /* Native assembly */
    FLUX_TARGET_IR       = 3,  /* FIR IR (optimization) */
    FLUX_TARGET_OBJ      = 4,  /* Object file (via external assembler) */
} flux_target_t;

/* ========================================================================
 * FIR (FLUX Intermediate Representation)
 *
 * FIR is an SSA-based IR. Every value is defined exactly once.
 * Basic blocks form a CFG with SSA phi nodes at block entries.
 * ======================================================================== */

/* FIR Value Types */
typedef enum {
    FIR_TYPE_VOID    = 0,
    FIR_TYPE_I8,
    FIR_TYPE_I16,
    FIR_TYPE_I32,
    FIR_TYPE_I64,
    FIR_TYPE_F32,
    FIR_TYPE_F64,
    FIR_TYPE_PTR,
    FIR_TYPE_FUNC,
    FIR_TYPE_STRUCT,
    FIR_TYPE_ARRAY,
    FIR_TYPE_BOOL,
    FIR_TYPE_AGENT,       /* Agent handle */
    FIR_TYPE_CAPABILITY,  /* Capability token */
    FIR_TYPE_BYTECODE,    /* Bytecode reference */
    FIR_TYPE_REGION,      /* Memory region */
    FIR_TYPE_ANY,         /* Dynamic type */
} fir_type_t;

/* FIR Opcodes */
typedef enum {
    /* Constants */
    FIR_OP_CONST    = 0,
    FIR_OP_CONST_F  = 1,

    /* Arithmetic */
    FIR_OP_ADD      = 10,
    FIR_OP_SUB      = 11,
    FIR_OP_MUL      = 12,
    FIR_OP_DIV      = 13,
    FIR_OP_MOD      = 14,
    FIR_OP_NEG      = 15,
    FIR_OP_ABS      = 16,

    /* Bitwise */
    FIR_OP_AND      = 20,
    FIR_OP_OR       = 21,
    FIR_OP_XOR      = 22,
    FIR_OP_NOT      = 23,
    FIR_OP_SHL      = 24,
    FIR_OP_SHR      = 25,

    /* Comparison */
    FIR_OP_EQ       = 30,
    FIR_OP_NE       = 31,
    FIR_OP_LT       = 32,
    FIR_OP_GT       = 33,
    FIR_OP_LE       = 34,
    FIR_OP_GE       = 35,

    /* Control Flow */
    FIR_OP_JUMP     = 40,
    FIR_OP_BRANCH   = 41,
    FIR_OP_RETURN   = 42,
    FIR_OP_CALL     = 43,
    FIR_OP_PHI      = 44,

    /* Memory */
    FIR_OP_ALLOC    = 50,
    FIR_OP_LOAD     = 51,
    FIR_OP_STORE    = 52,
    FIR_OP_GEP      = 53,  /* Get element pointer */

    /* Aggregate */
    FIR_OP_EXTRACT  = 60,
    FIR_OP_INSERT   = 61,
    FIR_OP Aggregate= 62,

    /* Agent / A2A */
    FIR_OP_DELEGATE = 70,  /* Delegate to agent */
    FIR_OP_TELL     = 71,  /* Send message to agent */
    FIR_OP_ASK      = 72,  /* Request from agent */
    FIR_OP_BARRIER  = 73,  /* Sync point */
    FIR_OP_SPAWN    = 74,  /* Spawn new agent */

    /* FLUX-specific */
    FIR_OP_TILE     = 80,  /* Tile composition */
    FIR_OP_REGION   = 81,  /* Memory region op */
    FIR_OP_COMPOSE  = 82,  /* Module composition */
    FIR_OP_ADAPT    = 83,  /* Adaptive execution hint */

    /* System */
    FIR_OP_SYSCALL  = 90,
    FIR_OP_INTRINSIC = 91,
    FIR_OP_TRAP     = 92,
} fir_op_t;

/* Maximum string lengths */
#define FIR_NAME_MAX     128
#define FIR_MAX_OPERANDS 4
#define FIR_MAX_PHI_SRC  8
#define FIR_MAX_BLOCKS   256
#define FIR_MAX_VALUES   1024
#define FIR_MAX_FUNCS    64

/* ========================================================================
 * FIR Value
 * ======================================================================== */

typedef struct {
    uint32_t    id;
    fir_type_t  type;
    fir_op_t    op;
    const char *name;

    /* Operands (value IDs) */
    uint32_t    operands[FIR_MAX_OPERANDS];
    int         num_operands;

    /* Constant data */
    union {
        int64_t  ival;
        double   fval;
        const char *sval;
        uint32_t block_id;
    };

    /* For PHI nodes */
    struct {
        uint32_t src_ids[FIR_MAX_PHI_SRC];
        uint32_t block_ids[FIR_MAX_PHI_SRC];
        int      count;
    } phi;
} fir_value_t;

/* ========================================================================
 * FIR Basic Block
 * ======================================================================== */

typedef struct {
    uint32_t    id;
    char        label[FIR_NAME_MAX];
    fir_value_t values[FIR_MAX_VALUES];
    int         num_values;
    uint32_t    terminator;  /* Value ID of branch/jump/return */
    bool        sealed;
} fir_block_t;

/* ========================================================================
 * FIR Function
 * ======================================================================== */

typedef struct {
    char        name[FIR_NAME_MAX];
    fir_type_t  return_type;
    fir_type_t  param_types[16];
    char        param_names[16][FIR_NAME_MAX];
    int         num_params;
    uint32_t    entry_block;
    fir_block_t blocks[FIR_MAX_BLOCKS];
    int         num_blocks;
    bool        is_agent;    /* Is this an agent function? */
    bool        is_compiled; /* Was this self-generated? */
    uint32_t    next_value_id;
} fir_func_t;

/* ========================================================================
 * FIR Module (Compilation Unit)
 * ======================================================================== */

typedef struct {
    char        name[FIR_NAME_MAX];
    fir_func_t  funcs[FIR_MAX_FUNCS];
    int         num_funcs;
    uint32_t    next_func_id;
    uint32_t    next_block_id;
} fir_module_t;

/* ========================================================================
 * Compiler Error
 * ======================================================================== */

#define FLUX_COMPILE_ERR_MAX 256

typedef struct {
    int         line;
    int         col;
    char        message[FLUX_COMPILE_ERR_MAX];
    char        context[512];     /* Source context around error */
    bool        fatal;
} flux_compile_error_t;

/* ========================================================================
 * Compiler Result
 * ======================================================================== */

typedef struct {
    char                  *output;         /* Generated code/binary */
    flux_size_t           output_len;
    fir_module_t         *ir;              /* FIR if requested */
    flux_compile_error_t  errors[32];
    int                   num_errors;
    int                   warnings;
    bool                  success;
    uint64_t              compile_time_us; /* Compilation time in microseconds */
    flux_size_t           output_size;     /* Generated output size */
} flux_compile_result_t;

/* ========================================================================
 * Self-Compilation Context
 *
 * When the OS acts as developer (DEVCODE syscall), it uses this context
 * to track what it's generating, why, and for what hardware.
 * ======================================================================== */

typedef struct {
    char        intent[256];         /* Natural language description */
    char        target_arch[32];     /* Target architecture */
    char        target_opt[32];      /* Optimization level */
    bool        use_simd;            /* Use SIMD instructions? */
    bool        use_parallel;        /* Parallelize? */
    uint32_t    complexity_budget;   /* Max complexity score */
    char        constraints[256];    /* Additional constraints */
} flux_devcode_ctx_t;

/* ========================================================================
 * Compiler API
 * ======================================================================== */

/* Initialize the compiler */
flux_status_t flux_compiler_init(void);
void          flux_compiler_shutdown(void);

/* Main compilation entry point */
flux_compile_result_t flux_compile(const char *source, flux_size_t source_len,
                                   flux_lang_t input_lang, flux_target_t target,
                                   const char *module_name);

/* FIR construction helpers */
fir_module_t  *fir_module_create(const char *name);
fir_func_t    *fir_func_create(fir_module_t *mod, const char *name,
                               fir_type_t ret_type, int num_params, ...);
fir_block_t   *fir_block_create(fir_func_t *func, const char *label);
uint32_t      fir_value_create(fir_block_t *block, fir_op_t op,
                               fir_type_t type, int num_ops, ...);
void           fir_module_destroy(fir_module_t *mod);

/* FIR validation */
bool           fir_validate(const fir_module_t *mod);
int            fir_validate_func(const fir_func_t *func);

/* FIR optimization passes */
int            fir_optimize(flux_module_t *mod, int level); /* 0=none, 1=speed, 2=size, 3=aggressive */
void           fir_dead_code_elim(fir_func_t *func);
void           fir_constant_fold(fir_func_t *func);
void           fir_inline_small(fir_module_t *mod, uint32_t threshold);

/* Backend-specific code generation */
flux_status_t  flux_codegen_c(const fir_module_t *mod, flux_compile_result_t *result);
flux_status_t  flux_codegen_bytecode(const fir_module_t *mod, flux_compile_result_t *result);
flux_status_t  flux_codegen_native(const fir_module_t *mod, flux_compile_result_t *result,
                                   flux_arch_t arch);

/* DevCode: OS acts as developer */
flux_status_t  flux_devcode(const flux_devcode_ctx_t *ctx, flux_compile_result_t *result);

/* Self-compilation: compile the OS itself */
flux_status_t  flux_self_compile(flux_compile_result_t *result);

/* Cleanup */
void           flux_compile_result_free(flux_compile_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_COMPILER_H */
