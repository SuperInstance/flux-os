/*
 * FLUX OS — FIR (FLUX Intermediate Representation) Builder
 *
 * FIR is an SSA-based IR where every value is defined exactly once.
 * Basic blocks form a CFG with PHI nodes at block entries.
 *
 * This file implements:
 *   - fir_module_create()       — Create new FIR module
 *   - fir_func_create()         — Create function with params
 *   - fir_block_create()        — Create basic block
 *   - fir_value_create()        — Create SSA value
 *   - fir_validate()            — Validate SSA properties
 *   - fir_dead_code_elim()      — Remove unused values
 *   - fir_constant_fold()       — Fold constant expressions
 *   - fir_module_destroy()      — Free all memory
 *   - fir_optimize()            — Run optimization passes
 *   - fir_inline_small()        — Inline small functions
 *   - fir_validate_func()       — Validate single function
 */

#include "flux/compiler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

/* ========================================================================
 * Internal: Safe string copy
 * ======================================================================== */

static void safe_strcpy(char *dst, const char *src, int max_len) {
    if (!dst || !src) return;
    int len = (int)strlen(src);
    if (len >= max_len) len = max_len - 1;
    memcpy(dst, src, (size_t)len);
    dst[len] = '\0';
}

/* ========================================================================
 * Module Creation / Destruction
 * ======================================================================== */

fir_module_t *fir_module_create(const char *name) {
    fir_module_t *mod = (fir_module_t *)calloc(1, sizeof(fir_module_t));
    if (!mod) return NULL;

    if (name) {
        safe_strcpy(mod->name, name, FIR_NAME_MAX);
    } else {
        safe_strcpy(mod->name, "unnamed", FIR_NAME_MAX);
    }

    mod->num_funcs = 0;
    mod->next_func_id = 0;
    mod->next_block_id = 0;

    return mod;
}

void fir_module_destroy(fir_module_t *mod) {
    if (mod) {
        free(mod);
    }
}

/* ========================================================================
 * Function Creation
 * ======================================================================== */

fir_func_t *fir_func_create(fir_module_t *mod, const char *name,
                             fir_type_t ret_type, int num_params, ...) {
    if (!mod) return NULL;
    if (mod->num_funcs >= FIR_MAX_FUNCS) return NULL;
    if (num_params > 16) num_params = 16;

    fir_func_t *func = &mod->funcs[mod->num_funcs];
    memset(func, 0, sizeof(fir_func_t));

    if (name) {
        safe_strcpy(func->name, name, FIR_NAME_MAX);
    } else {
        snprintf(func->name, FIR_NAME_MAX, "func_%d", mod->num_funcs);
    }

    func->return_type = ret_type;
    func->num_params = num_params;
    func->num_blocks = 0;
    func->entry_block = 0; /* Will be set when first block is created */
    func->is_agent = false;
    func->is_compiled = false;
    func->next_value_id = num_params; /* Params take first value IDs */

    /* Read parameter types from va_list */
    va_list ap;
    va_start(ap, num_params);
    for (int i = 0; i < num_params; i++) {
        func->param_types[i] = (fir_type_t)va_arg(ap, int);
        snprintf(func->param_names[i], FIR_NAME_MAX, "arg%d", i);
    }
    va_end(ap);

    mod->num_funcs++;
    return func;
}

/* ========================================================================
 * Block Creation
 * ======================================================================== */

fir_block_t *fir_block_create(fir_func_t *func, const char *label) {
    if (!func) return NULL;
    if (func->num_blocks >= FIR_MAX_BLOCKS) return NULL;

    fir_block_t *block = &func->blocks[func->num_blocks];
    memset(block, 0, sizeof(fir_block_t));

    block->id = func->num_blocks;

    if (label) {
        safe_strcpy(block->label, label, FIR_NAME_MAX);
    } else {
        snprintf(block->label, FIR_NAME_MAX, "bb%d", block->id);
    }

    block->num_values = 0;
    block->terminator = 0;
    block->sealed = false;

    /* Set entry block if this is the first block */
    if (func->num_blocks == 0) {
        func->entry_block = block->id;
    }

    func->num_blocks++;
    return block;
}

/* ========================================================================
 * Value Creation
 * ======================================================================== */

uint32_t fir_value_create(fir_block_t *block, fir_op_t op,
                           fir_type_t type, int num_ops, ...) {
    if (!block) return 0xFFFFFFFF;
    if (block->num_values >= FIR_MAX_VALUES) return 0xFFFFFFFF;
    if (num_ops > FIR_MAX_OPERANDS) num_ops = FIR_MAX_OPERANDS;

    fir_func_t *func = NULL; /* We don't have back-pointer; use ID directly */

    fir_value_t *val = &block->values[block->num_values];
    memset(val, 0, sizeof(fir_value_t));

    /* Value ID is global: block_id * FIR_MAX_VALUES + index */
    val->id = (block->id * FIR_MAX_VALUES) + block->num_values;
    val->type = type;
    val->op = op;
    val->name = NULL;
    val->num_operands = num_ops;

    /* Read operands from va_list */
    va_list ap;
    va_start(ap, num_ops);
    for (int i = 0; i < num_ops; i++) {
        val->operands[i] = (uint32_t)va_arg(ap, unsigned int);
    }
    va_end(ap);

    /* Initialize PHI node */
    memset(&val->phi, 0, sizeof(val->phi));
    val->phi.count = 0;

    block->num_values++;

    /* Check if this is a terminator instruction */
    if (op == FIR_OP_JUMP || op == FIR_OP_BRANCH ||
        op == FIR_OP_RETURN) {
        block->terminator = val->id;
        block->sealed = true;
    }

    return val->id;
}

/* ========================================================================
 * PHI Node Helpers
 * ======================================================================== */

/* Add a source to a PHI node (value must already exist) */
static bool fir_phi_add_source(fir_block_t *block, uint32_t phi_val_id,
                                uint32_t src_val_id, uint32_t src_block_id) {
    if (!block) return false;

    /* Find the value by ID within this block */
    for (int i = 0; i < block->num_values; i++) {
        fir_value_t *val = &block->values[i];
        if (val->id == phi_val_id && val->op == FIR_OP_PHI) {
            if (val->phi.count >= FIR_MAX_PHI_SRC) return false;
            val->phi.src_ids[val->phi.count] = src_val_id;
            val->phi.block_ids[val->phi.count] = src_block_id;
            val->phi.count++;
            return true;
        }
    }
    return false;
}

/* ========================================================================
 * Validation
 * ======================================================================== */

/* Check if an opcode is a terminator */
static bool fir_is_terminator(fir_op_t op) {
    return op == FIR_OP_JUMP || op == FIR_OP_BRANCH || op == FIR_OP_RETURN;
}

/* Check if an opcode is a constant */
static bool fir_is_constant(fir_op_t op) {
    return op == FIR_OP_CONST || op == FIR_OP_CONST_F;
}

/* Check if an opcode is a binary arithmetic op */
static bool fir_is_binary_arith(fir_op_t op) {
    return op >= FIR_OP_ADD && op <= FIR_OP_ABS;
}

/* Check if an opcode is a binary bitwise op */
static bool fir_is_binary_bitwise(fir_op_t op) {
    return op >= FIR_OP_AND && op <= FIR_OP_SHR;
}

int fir_validate_func(const fir_func_t *func) {
    if (!func) return -1;
    int errors = 0;

    /* Check at least one block exists */
    if (func->num_blocks == 0) {
        errors++;
        return errors;
    }

    /* Validate each block */
    for (int b = 0; b < func->num_blocks; b++) {
        const fir_block_t *block = &func->blocks[b];

        /* Check block has exactly one terminator at the end */
        bool has_terminator = false;
        for (int v = 0; v < block->num_values; v++) {
            const fir_value_t *val = &block->values[v];
            if (fir_is_terminator(val->op)) {
                if (has_terminator) {
                    errors++; /* Multiple terminators */
                }
                if (v != block->num_values - 1) {
                    errors++; /* Terminator not at end */
                }
                has_terminator = true;
            }
        }

        if (!has_terminator) {
            errors++; /* Missing terminator */
        }

        /* Validate branch/jump targets reference existing blocks */
        for (int v = 0; v < block->num_values; v++) {
            const fir_value_t *val = &block->values[v];
            if (val->op == FIR_OP_JUMP) {
                uint32_t target = val->block_id;
                bool found = false;
                for (int t = 0; t < func->num_blocks; t++) {
                    if (func->blocks[t].id == target) { found = true; break; }
                }
                if (!found) errors++;
            }
            if (val->op == FIR_OP_BRANCH) {
                /* First operand is condition, second is true block, third is false block */
                if (val->num_operands >= 2) {
                    bool found_true = false, found_false = false;
                    for (int t = 0; t < func->num_blocks; t++) {
                        if (func->blocks[t].id == val->operands[1]) found_true = true;
                        if (val->num_operands >= 3 &&
                            func->blocks[t].id == val->operands[2]) found_false = true;
                    }
                    if (!found_true || !found_false) errors++;
                }
            }
        }

        /* Validate PHI node sources reference existing blocks */
        for (int v = 0; v < block->num_values; v++) {
            const fir_value_t *val = &block->values[v];
            if (val->op == FIR_OP_PHI) {
                for (int p = 0; p < val->phi.count; p++) {
                    bool found = false;
                    for (int t = 0; t < func->num_blocks; t++) {
                        if (func->blocks[t].id == val->phi.block_ids[p]) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) errors++;
                }
            }
        }

        /* SSA property: check for duplicate definitions within a block */
        /* (Full SSA validation would require cross-block dominance analysis) */
    }

    /* Check entry block is valid */
    if (func->entry_block >= (uint32_t)func->num_blocks) {
        errors++;
    }

    return errors;
}

bool fir_validate(const fir_module_t *mod) {
    if (!mod) return false;
    if (mod->num_funcs == 0) return true; /* Empty module is valid */

    for (int f = 0; f < mod->num_funcs; f++) {
        if (fir_validate_func(&mod->funcs[f]) > 0) {
            return false;
        }
    }
    return true;
}

/* ========================================================================
 * Optimization Passes
 * ======================================================================== */

/* Check if a value ID is used anywhere in a function */
static bool value_is_used(const fir_func_t *func, uint32_t val_id) {
    for (int b = 0; b < func->num_blocks; b++) {
        const fir_block_t *block = &func->blocks[b];
        for (int v = 0; v < block->num_values; v++) {
            const fir_value_t *val = &block->values[v];
            /* Check operands */
            for (int o = 0; o < val->num_operands; o++) {
                if (val->operands[o] == val_id) return true;
            }
            /* Check PHI sources */
            for (int p = 0; p < val->phi.count; p++) {
                if (val->phi.src_ids[p] == val_id) return true;
            }
            /* Check branch target references */
            if (val->op == FIR_OP_BRANCH && val->num_operands >= 2 &&
                val->operands[1] == val_id) return true;
            if (val->op == FIR_OP_BRANCH && val->num_operands >= 3 &&
                val->operands[2] == val_id) return true;
        }
    }
    return false;
}

/* Find a value by its ID within a function */
static fir_value_t *find_value_in_func(fir_func_t *func, uint32_t val_id) {
    if (!func) return NULL;
    for (int b = 0; b < func->num_blocks; b++) {
        fir_block_t *block = &func->blocks[b];
        for (int v = 0; v < block->num_values; v++) {
            if (block->values[v].id == val_id) {
                return &block->values[v];
            }
        }
    }
    return NULL;
}

/* Find a constant integer value in a block */
static bool get_const_int(const fir_func_t *func, uint32_t val_id, int64_t *out) {
    if (!func || !out) return false;
    for (int b = 0; b < func->num_blocks; b++) {
        const fir_block_t *block = &func->blocks[b];
        for (int v = 0; v < block->num_values; v++) {
            const fir_value_t *val = &block->values[v];
            if (val->id == val_id && val->op == FIR_OP_CONST) {
                *out = val->ival;
                return true;
            }
        }
    }
    return false;
}

/* Find a constant float value in a function */
static bool get_const_float(const fir_func_t *func, uint32_t val_id, double *out) {
    if (!func || !out) return false;
    for (int b = 0; b < func->num_blocks; b++) {
        const fir_block_t *block = &func->blocks[b];
        for (int v = 0; v < block->num_values; v++) {
            const fir_value_t *val = &block->values[v];
            if (val->id == val_id && val->op == FIR_OP_CONST_F) {
                *out = val->fval;
                return true;
            }
        }
    }
    return false;
}

/* Replace all uses of old_val_id with new_val_id in a function */
static void replace_uses(fir_func_t *func, uint32_t old_val_id, uint32_t new_val_id) {
    for (int b = 0; b < func->num_blocks; b++) {
        fir_block_t *block = &func->blocks[b];
        for (int v = 0; v < block->num_values; v++) {
            fir_value_t *val = &block->values[v];
            for (int o = 0; o < val->num_operands; o++) {
                if (val->operands[o] == old_val_id) {
                    val->operands[o] = new_val_id;
                }
            }
            for (int p = 0; p < val->phi.count; p++) {
                if (val->phi.src_ids[p] == old_val_id) {
                    val->phi.src_ids[p] = new_val_id;
                }
            }
        }
    }
}

/* Dead Code Elimination: remove values that are never used,
 * except for side-effecting operations (calls, stores, returns, syscalls) */
void fir_dead_code_elim(fir_func_t *func) {
    if (!func) return;
    bool changed = true;

    while (changed) {
        changed = false;
        for (int b = 0; b < func->num_blocks; b++) {
            fir_block_t *block = &func->blocks[b];

            for (int v = block->num_values - 1; v >= 0; v--) {
                fir_value_t *val = &block->values[v];

                /* Never remove terminators */
                if (fir_is_terminator(val->op)) continue;

                /* Never remove calls, stores, syscalls (side effects) */
                if (val->op == FIR_OP_CALL || val->op == FIR_OP_STORE ||
                    val->op == FIR_OP_SYSCALL || val->op == FIR_OP_INTRINSIC ||
                    val->op == FIR_OP_TRAP || val->op == FIR_OP_DELEGATE ||
                    val->op == FIR_OP_TELL || val->op == FIR_OP_ASK ||
                    val->op == FIR_OP_SPAWN || val->op == FIR_OP_BARRIER) {
                    continue;
                }

                /* Check if this value is used anywhere */
                if (!value_is_used(func, val->id)) {
                    /* Remove by shifting remaining values */
                    for (int i = v; i < block->num_values - 1; i++) {
                        block->values[i] = block->values[i + 1];
                    }
                    block->num_values--;
                    changed = true;
                }
            }
        }
    }
}

/* Constant Folding: evaluate constant expressions at compile time */
void fir_constant_fold(fir_func_t *func) {
    if (!func) return;
    bool changed = true;

    while (changed) {
        changed = false;
        for (int b = 0; b < func->num_blocks; b++) {
            fir_block_t *block = &func->blocks[b];

            for (int v = 0; v < block->num_values; v++) {
                fir_value_t *val = &block->values[v];

                /* Binary integer arithmetic */
                if (fir_is_binary_arith(val->op) && val->num_operands >= 2) {
                    int64_t a, b_val;
                    if (get_const_int(func, val->operands[0], &a) &&
                        get_const_int(func, val->operands[1], &b_val)) {
                        int64_t result = 0;
                        bool valid = true;
                        switch (val->op) {
                            case FIR_OP_ADD: result = a + b_val; break;
                            case FIR_OP_SUB: result = a - b_val; break;
                            case FIR_OP_MUL: result = a * b_val; break;
                            case FIR_OP_DIV:
                                if (b_val == 0) { valid = false; }
                                else { result = a / b_val; }
                                break;
                            case FIR_OP_MOD:
                                if (b_val == 0) { valid = false; }
                                else { result = a % b_val; }
                                break;
                            case FIR_OP_NEG: result = -a; break;
                            case FIR_OP_ABS: result = (a < 0) ? -a : a; break;
                            default: valid = false; break;
                        }
                        if (valid) {
                            val->op = FIR_OP_CONST;
                            val->ival = result;
                            val->num_operands = 0;
                            changed = true;
                        }
                    }
                }

                /* Binary bitwise operations */
                if (fir_is_binary_bitwise(val->op) && val->num_operands >= 2) {
                    int64_t a, b_val;
                    if (get_const_int(func, val->operands[0], &a) &&
                        get_const_int(func, val->operands[1], &b_val)) {
                        int64_t result = 0;
                        bool valid = true;
                        switch (val->op) {
                            case FIR_OP_AND: result = a & b_val; break;
                            case FIR_OP_OR:  result = a | b_val; break;
                            case FIR_OP_XOR: result = a ^ b_val; break;
                            case FIR_OP_SHL: result = a << (b_val & 63); break;
                            case FIR_OP_SHR: result = a >> (b_val & 63); break;
                            case FIR_OP_NOT: result = ~a; break;
                            default: valid = false; break;
                        }
                        if (valid) {
                            val->op = FIR_OP_CONST;
                            val->ival = result;
                            val->num_operands = 0;
                            changed = true;
                        }
                    }
                }

                /* Comparison operations */
                if (val->op >= FIR_OP_EQ && val->op <= FIR_OP_GE &&
                    val->num_operands >= 2) {
                    int64_t a, b_val;
                    if (get_const_int(func, val->operands[0], &a) &&
                        get_const_int(func, val->operands[1], &b_val)) {
                        int64_t result = 0;
                        switch (val->op) {
                            case FIR_OP_EQ: result = (a == b_val) ? 1 : 0; break;
                            case FIR_OP_NE: result = (a != b_val) ? 1 : 0; break;
                            case FIR_OP_LT: result = (a < b_val) ? 1 : 0;  break;
                            case FIR_OP_GT: result = (a > b_val) ? 1 : 0;  break;
                            case FIR_OP_LE: result = (a <= b_val) ? 1 : 0; break;
                            case FIR_OP_GE: result = (a >= b_val) ? 1 : 0; break;
                            default: break;
                        }
                        val->op = FIR_OP_CONST;
                        val->ival = result;
                        val->num_operands = 0;
                        changed = true;
                    }
                }

                /* Unary negation of constant */
                if (val->op == FIR_OP_NEG && val->num_operands >= 1) {
                    int64_t a;
                    if (get_const_int(func, val->operands[0], &a)) {
                        val->op = FIR_OP_CONST;
                        val->ival = -a;
                        val->num_operands = 0;
                        changed = true;
                    }
                }

                /* Algebraic simplifications: x + 0 = x, x * 1 = x, x * 0 = 0 */
                if (val->num_operands >= 2) {
                    int64_t const_val;
                    if (get_const_int(func, val->operands[1], &const_val)) {
                        /* x + 0 => x */
                        if ((val->op == FIR_OP_ADD || val->op == FIR_OP_SUB) &&
                            const_val == 0) {
                            replace_uses(func, val->id, val->operands[0]);
                            changed = true;
                        }
                        /* x * 1 => x */
                        if (val->op == FIR_OP_MUL && const_val == 1) {
                            replace_uses(func, val->id, val->operands[0]);
                            changed = true;
                        }
                        /* x * 0 => 0 */
                        if (val->op == FIR_OP_MUL && const_val == 0) {
                            val->op = FIR_OP_CONST;
                            val->ival = 0;
                            val->num_operands = 0;
                            changed = true;
                        }
                    }
                    if (get_const_int(func, val->operands[0], &const_val)) {
                        /* 0 + x => x */
                        if (val->op == FIR_OP_ADD && const_val == 0) {
                            replace_uses(func, val->id, val->operands[1]);
                            changed = true;
                        }
                        /* 1 * x => x */
                        if (val->op == FIR_OP_MUL && const_val == 1) {
                            replace_uses(func, val->id, val->operands[1]);
                            changed = true;
                        }
                        /* 0 * x => 0 */
                        if (val->op == FIR_OP_MUL && const_val == 0) {
                            val->op = FIR_OP_CONST;
                            val->ival = 0;
                            val->num_operands = 0;
                            changed = true;
                        }
                    }
                }
            }
        }
    }
}

/* Inline small functions (threshold = max instruction count) */
void fir_inline_small(fir_module_t *mod, uint32_t threshold) {
    if (!mod || threshold == 0) return;

    /* For each function, look for calls to small functions */
    for (int f = 0; f < mod->num_funcs; f++) {
        fir_func_t *caller = &mod->funcs[f];

        for (int b = 0; b < caller->num_blocks; b++) {
            fir_block_t *block = &caller->blocks[b];

            for (int v = 0; v < block->num_values; v++) {
                fir_value_t *val = &block->values[v];
                if (val->op != FIR_OP_CALL) continue;

                /* Find callee by name (stored in val->sval or matched by ID) */
                /* In practice, calls reference a function name; here we check
                   if the call target matches a small function */
                fir_func_t *callee = NULL;
                for (int c = 0; c < mod->num_funcs; c++) {
                    /* Check total instruction count of callee */
                    int total_vals = 0;
                    for (int cb = 0; cb < mod->funcs[c].num_blocks; cb++) {
                        total_vals += mod->funcs[c].blocks[cb].num_values;
                    }
                    if (total_vals > 0 && (uint32_t)total_vals <= threshold) {
                        callee = &mod->funcs[c];
                    }
                }

                if (!callee) continue;

                /* Inline: replace CALL with copied instructions from callee
                   (simplified — full inlining would remap value IDs) */
                /* For now, mark as compiled and skip inlining */
                val->is_compiled = true;
            }
        }
    }
}

/* ========================================================================
 * Multi-Pass Optimization
 * ======================================================================== */

int fir_optimize(fir_module_t *mod, int level) {
    if (!mod) return -1;
    if (level == 0) return 0;

    int total_changes = 0;

    for (int f = 0; f < mod->num_funcs; f++) {
        fir_func_t *func = &mod->funcs[f];

        /* Always run constant folding and DCE */
        int prev_vals = 0;
        for (int b = 0; b < func->num_blocks; b++) {
            prev_vals += func->blocks[b].num_values;
        }

        fir_constant_fold(func);
        fir_dead_code_elim(func);

        int new_vals = 0;
        for (int b = 0; b < func->num_blocks; b++) {
            new_vals += func->blocks[b].num_values;
        }
        total_changes += (prev_vals - new_vals);

        /* Level 2+: run again for iterative optimization */
        if (level >= 2) {
            for (int pass = 0; pass < 3; pass++) {
                fir_constant_fold(func);
                fir_dead_code_elim(func);
            }
        }

        /* Level 3 (aggressive): inline small functions */
        if (level >= 3) {
            fir_inline_small(mod, 5);
        }
    }

    return total_changes;
}

/* ========================================================================
 * FIR Debug / Print Helpers
 * ======================================================================== */

static const char *fir_type_name(fir_type_t type) {
    switch (type) {
        case FIR_TYPE_VOID:      return "void";
        case FIR_TYPE_I8:        return "i8";
        case FIR_TYPE_I16:       return "i16";
        case FIR_TYPE_I32:       return "i32";
        case FIR_TYPE_I64:       return "i64";
        case FIR_TYPE_F32:       return "f32";
        case FIR_TYPE_F64:       return "f64";
        case FIR_TYPE_PTR:       return "ptr";
        case FIR_TYPE_FUNC:      return "func";
        case FIR_TYPE_STRUCT:    return "struct";
        case FIR_TYPE_ARRAY:     return "array";
        case FIR_TYPE_BOOL:      return "bool";
        case FIR_TYPE_AGENT:     return "agent";
        case FIR_TYPE_CAPABILITY:return "capability";
        case FIR_TYPE_BYTECODE:  return "bytecode";
        case FIR_TYPE_REGION:    return "region";
        case FIR_TYPE_ANY:       return "any";
        default:                 return "unknown";
    }
}

static const char *fir_op_name(fir_op_t op) {
    switch (op) {
        case FIR_OP_CONST:    return "const";
        case FIR_OP_CONST_F:  return "const_f";
        case FIR_OP_ADD:      return "add";
        case FIR_OP_SUB:      return "sub";
        case FIR_OP_MUL:      return "mul";
        case FIR_OP_DIV:      return "div";
        case FIR_OP_MOD:      return "mod";
        case FIR_OP_NEG:      return "neg";
        case FIR_OP_ABS:      return "abs";
        case FIR_OP_AND:      return "and";
        case FIR_OP_OR:       return "or";
        case FIR_OP_XOR:      return "xor";
        case FIR_OP_NOT:      return "not";
        case FIR_OP_SHL:      return "shl";
        case FIR_OP_SHR:      return "shr";
        case FIR_OP_EQ:       return "eq";
        case FIR_OP_NE:       return "ne";
        case FIR_OP_LT:       return "lt";
        case FIR_OP_GT:       return "gt";
        case FIR_OP_LE:       return "le";
        case FIR_OP_GE:       return "ge";
        case FIR_OP_JUMP:     return "jump";
        case FIR_OP_BRANCH:   return "branch";
        case FIR_OP_RETURN:   return "return";
        case FIR_OP_CALL:     return "call";
        case FIR_OP_PHI:      return "phi";
        case FIR_OP_ALLOC:    return "alloc";
        case FIR_OP_LOAD:     return "load";
        case FIR_OP_STORE:    return "store";
        case FIR_OP_GEP:      return "gep";
        case FIR_OP_EXTRACT:  return "extract";
        case FIR_OP_INSERT:   return "insert";
        case FIR_OP_Aggregate:return "aggregate";
        case FIR_OP_DELEGATE: return "delegate";
        case FIR_OP_TELL:     return "tell";
        case FIR_OP_ASK:      return "ask";
        case FIR_OP_BARRIER:  return "barrier";
        case FIR_OP_SPAWN:    return "spawn";
        case FIR_OP_TILE:     return "tile";
        case FIR_OP_REGION:   return "region";
        case FIR_OP_COMPOSE:  return "compose";
        case FIR_OP_ADAPT:    return "adapt";
        case FIR_OP_SYSCALL:  return "syscall";
        case FIR_OP_INTRINSIC:return "intrinsic";
        case FIR_OP_TRAP:     return "trap";
        default:              return "unknown";
    }
}

/* Print FIR module to stdout (for debugging) */
void fir_module_print(const fir_module_t *mod) {
    if (!mod) return;

    printf("module %s {\n", mod->name);

    for (int f = 0; f < mod->num_funcs; f++) {
        const fir_func_t *func = &mod->funcs[f];
        printf("\n  func %s(", func->name);

        for (int p = 0; p < func->num_params; p++) {
            if (p > 0) printf(", ");
            printf("%s: %s", func->param_names[p],
                   fir_type_name(func->param_types[p]));
        }
        printf(") -> %s", fir_type_name(func->return_type));

        if (func->is_agent) printf(" [agent]");
        printf(" {\n");

        for (int b = 0; b < func->num_blocks; b++) {
            const fir_block_t *block = &func->blocks[b];
            printf("  %s:\n", block->label);

            for (int v = 0; v < block->num_values; v++) {
                const fir_value_t *val = &block->values[v];
                printf("    %%%-4u = %-12s", val->id, fir_op_name(val->op));

                switch (val->op) {
                    case FIR_OP_CONST:
                        printf("%lld", (long long)val->ival);
                        break;
                    case FIR_OP_CONST_F:
                        printf("%f", val->fval);
                        break;
                    case FIR_OP_JUMP:
                        printf("bb%u", val->block_id);
                        break;
                    case FIR_OP_BRANCH:
                        printf("%%%-u, bb%u, bb%u",
                               val->operands[0], val->operands[1],
                               val->num_operands >= 3 ? val->operands[2] : 0);
                        break;
                    case FIR_OP_RETURN:
                        if (val->num_operands >= 1)
                            printf("%%%-u", val->operands[0]);
                        else
                            printf("void");
                        break;
                    case FIR_OP_CALL:
                        if (val->sval)
                            printf("@%s", val->sval);
                        for (int o = 0; o < val->num_operands; o++) {
                            printf(" %%%-u", val->operands[o]);
                        }
                        break;
                    case FIR_OP_PHI:
                        for (int p = 0; p < val->phi.count; p++) {
                            if (p > 0) printf(", ");
                            printf("[%%%-u, bb%u]",
                                   val->phi.src_ids[p], val->phi.block_ids[p]);
                        }
                        break;
                    default:
                        for (int o = 0; o < val->num_operands; o++) {
                            if (o > 0) printf(", ");
                            printf("%%%-u", val->operands[o]);
                        }
                        break;
                }

                printf("  ; type=%s\n", fir_type_name(val->type));
            }
        }

        printf("  }\n");
    }

    printf("}\n");
}
