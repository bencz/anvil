/*
 * ANVIL - Shared helpers for optimisation passes.
 *
 * Previously every pass carried its own copy of replace_uses, same_pointer,
 * is_const_int, etc. This header centralises them so passes agree on semantics
 * and we avoid drift (one file had args in a different order).
 */

#ifndef ANVIL_OPT_UTILS_H
#define ANVIL_OPT_UTILS_H

#include "anvil/anvil_internal.h"
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Constant queries
 * ------------------------------------------------------------------------- */

static inline bool anvil_opt_is_const_int(const anvil_value_t *val)
{
    return val && val->kind == ANVIL_VAL_CONST_INT;
}

static inline bool anvil_opt_is_const_float(const anvil_value_t *val)
{
    return val && val->kind == ANVIL_VAL_CONST_FLOAT;
}

static inline int64_t anvil_opt_get_const_int(const anvil_value_t *val)
{
    return val->data.i;
}

static inline double anvil_opt_get_const_float(const anvil_value_t *val)
{
    return val->data.f;
}

static inline bool anvil_opt_is_zero(const anvil_value_t *val)
{
    if (anvil_opt_is_const_int(val))   return val->data.i == 0;
    if (anvil_opt_is_const_float(val)) return val->data.f == 0.0;
    return false;
}

static inline bool anvil_opt_is_one(const anvil_value_t *val)
{
    if (anvil_opt_is_const_int(val))   return val->data.i == 1;
    if (anvil_opt_is_const_float(val)) return val->data.f == 1.0;
    return false;
}

static inline bool anvil_opt_is_all_ones(const anvil_value_t *val)
{
    if (!anvil_opt_is_const_int(val)) return false;
    int64_t v = val->data.i;
    return v == -1 || v == (int64_t)0xFFFFFFFFFFFFFFFFLL;
}

/* ---------------------------------------------------------------------------
 * Pointer-aliasing query used by the memory passes.
 * Conservative: returns true only when p1 and p2 must refer to the same slot.
 * ------------------------------------------------------------------------- */

static inline bool anvil_opt_same_pointer(const anvil_value_t *p1, const anvil_value_t *p2)
{
    if (!p1 || !p2) return false;
    if (p1 == p2)   return true;

    if (p1->kind == ANVIL_VAL_INSTR && p2->kind == ANVIL_VAL_INSTR) {
        if (p1->data.instr == p2->data.instr) return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Typed-constant constructors (wrap the context-based builder API).
 * ------------------------------------------------------------------------- */

static inline anvil_value_t *anvil_opt_make_const_int(anvil_ctx_t *ctx,
                                                      const anvil_type_t *type,
                                                      int64_t value)
{
    if (!type) return NULL;
    switch (type->kind) {
        case ANVIL_TYPE_I8:  return anvil_const_i8(ctx,  (int8_t)value);
        case ANVIL_TYPE_I16: return anvil_const_i16(ctx, (int16_t)value);
        case ANVIL_TYPE_I32: return anvil_const_i32(ctx, (int32_t)value);
        case ANVIL_TYPE_I64: return anvil_const_i64(ctx, value);
        case ANVIL_TYPE_U8:  return anvil_const_u8(ctx,  (uint8_t)value);
        case ANVIL_TYPE_U16: return anvil_const_u16(ctx, (uint16_t)value);
        case ANVIL_TYPE_U32: return anvil_const_u32(ctx, (uint32_t)value);
        case ANVIL_TYPE_U64: return anvil_const_u64(ctx, (uint64_t)value);
        default: return NULL;
    }
}

static inline anvil_value_t *anvil_opt_make_const_float(anvil_ctx_t *ctx,
                                                        const anvil_type_t *type,
                                                        double value)
{
    if (!type) return NULL;
    switch (type->kind) {
        case ANVIL_TYPE_F32: return anvil_const_f32(ctx, (float)value);
        case ANVIL_TYPE_F64: return anvil_const_f64(ctx, value);
        default: return NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Use replacement.
 *
 * Previously five slightly different copies existed across passes. These two
 * canonical variants cover every call-site:
 *
 *   anvil_opt_replace_uses_in_func: rewrite every operand equal to old_val
 *     across every instruction of every block. Safe whenever new_val
 *     dominates every use of old_val (the common case when new_val replaces
 *     old_val at a fold/propagate site).
 *
 *   anvil_opt_replace_uses_in_block_after: rewrite operands only within the
 *     same block, starting after `start`. Used by the local CSE pass, where
 *     we intentionally keep the scope intra-block.
 * ------------------------------------------------------------------------- */

static inline int anvil_opt_replace_uses_in_func(anvil_func_t *func,
                                                 anvil_value_t *old_val,
                                                 anvil_value_t *new_val)
{
    int count = 0;
    if (!func || !old_val) return 0;

    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_NOP) continue;
            for (size_t i = 0; i < instr->num_operands; i++) {
                if (instr->operands[i] == old_val) {
                    instr->operands[i] = new_val;
                    count++;
                }
            }
        }
    }
    return count;
}

static inline int anvil_opt_replace_uses_in_block_after(anvil_instr_t *start,
                                                        anvil_value_t *old_val,
                                                        anvil_value_t *new_val)
{
    int count = 0;
    if (!start || !old_val) return 0;

    for (anvil_instr_t *instr = start->next; instr; instr = instr->next) {
        if (instr->op == ANVIL_OP_NOP) continue;
        for (size_t i = 0; i < instr->num_operands; i++) {
            if (instr->operands[i] == old_val) {
                instr->operands[i] = new_val;
                count++;
            }
        }
    }
    return count;
}

#endif /* ANVIL_OPT_UTILS_H */
