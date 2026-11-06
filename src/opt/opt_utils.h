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

/* Shared, target-width-aware evaluator. NULL means unsupported or unsafe to fold. */
anvil_value_t *anvil_opt_fold_integer(anvil_ctx_t *ctx, const anvil_instr_t *instr, anvil_value_t *left, anvil_value_t *right);

static inline unsigned anvil_opt_call_effects(const anvil_instr_t *instr)
{
    return anvil_instr_call_effects(instr);
}

/* ---------------------------------------------------------------------------
 * Instruction erasure.
 *
 * Instructions are context-owned and released with the context.  Optimizers
 * therefore erase an instruction by unlinking it from its block, while the
 * ownership chain keeps the allocation alive.  Keeping the old next pointer
 * also makes erase-during-iteration safe.  Every pass must leave independently
 * verifiable IR; a non-canonical NOP with operands/result is not valid IR.
 * ------------------------------------------------------------------------- */

static inline bool anvil_opt_erase_instr(anvil_instr_t *instr)
{
    if (!instr || !instr->parent) return false;

    anvil_block_t *block = instr->parent;
    if (instr->prev) instr->prev->next = instr->next;
    else block->first = instr->next;

    if (instr->next) instr->next->prev = instr->prev;
    else block->last = instr->prev;

    /* The builder cursor may still name a context-owned instruction that was
     * just erased. Repair it before clearing parent so subsequent appends are
     * well-defined. Keep `next` as an iteration cursor, but clear parent/prev
     * to make a second erase a harmless false return. */
    anvil_ctx_t *ctx = block->parent && block->parent->parent
        ? block->parent->parent->ctx : NULL;
    if (ctx && ctx->insert_point == instr) ctx->insert_point = block->last;
    instr->parent = NULL;
    instr->prev = NULL;

    return true;
}

/* The one value a PHI takes, ignoring its own result (what only flows
 * around a loop), or NULL when it takes more than one.  In a block other
 * than the entry that value dominates the PHI's block, so it may replace
 * the PHI (the entry can be a loop header whose PHI reads a later value). */
static inline anvil_value_t *anvil_opt_phi_single_value(const anvil_instr_t *phi)
{
    anvil_value_t *single = NULL;
    for (size_t incoming = 0; incoming < phi->num_operands; incoming++) {
        anvil_value_t *value = phi->operands[incoming];
        if (value == phi->result || value == single)
            continue;
        if (single)
            return NULL;
        single = value;
    }
    return single;
}

/* Types mem2reg keeps in values: integers, floating point, pointers. */
static inline bool anvil_opt_scalar_type(const anvil_type_t *type)
{
    return type && ((type->kind >= ANVIL_TYPE_I1 && type->kind <= ANVIL_TYPE_F64) || type->kind == ANVIL_TYPE_PTR);
}

/* Whether a call may observe memory as it stands when it runs: it reads
 * memory, or control may leave through it (a trap, an unwind, a call that
 * does not return) and the state there is visible. */
static inline bool anvil_opt_call_observes_memory(const anvil_instr_t *call)
{
    unsigned barriers = ANVIL_EFFECT_READ_MEMORY | ANVIL_EFFECT_MAY_TRAP | ANVIL_EFFECT_MAY_UNWIND | ANVIL_EFFECT_MAY_NOT_RETURN | ANVIL_EFFECT_OBSERVABLE;
    return (anvil_opt_call_effects(call) & barriers) != 0;
}

/* A detached instruction of `op` on one or two operands (NULL: none). */
static inline anvil_instr_t *anvil_opt_make_instr(anvil_ctx_t *ctx, anvil_op_t op, anvil_type_t *type, anvil_value_t *first, anvil_value_t *second,
                                                  const char *name)
{
    anvil_instr_t *instr = anvil_instr_create(ctx, op, type, name);
    if (!instr || (first && !anvil_instr_add_operand(instr, first)) || (second && !anvil_instr_add_operand(instr, second)))
        return NULL;
    return instr;
}

/* Both operands are required: a missing one (a constant that could not be
 * made) fails like an allocation. */
static inline anvil_instr_t *anvil_opt_make_binary(anvil_ctx_t *ctx, anvil_op_t op, anvil_type_t *type, anvil_value_t *left, anvil_value_t *right,
                                                   const char *name)
{
    return left && right ? anvil_opt_make_instr(ctx, op, type, left, right, name) : NULL;
}

/* Integer operations whose operands may swap (the comparisons included). */
static inline bool anvil_opt_commutative(anvil_op_t op)
{
    switch (op) {
    case ANVIL_OP_ADD:
    case ANVIL_OP_MUL:
    case ANVIL_OP_UMULH:
    case ANVIL_OP_SMULH:
    case ANVIL_OP_AND:
    case ANVIL_OP_OR:
    case ANVIL_OP_XOR:
    case ANVIL_OP_CMP_EQ:
    case ANVIL_OP_CMP_NE:
        return true;
    default:
        return false;
    }
}

/* Links a detached instruction into `position`'s block just before it. */
static inline void anvil_opt_insert_before(anvil_instr_t *position, anvil_instr_t *instr)
{
    anvil_block_t *block = position->parent;
    instr->parent = block;
    instr->owner_module = block->owner_module;
    if (instr->result)
        instr->result->owner_module = block->owner_module;
    instr->prev = position->prev;
    instr->next = position;
    if (position->prev)
        position->prev->next = instr;
    else
        block->first = instr;
    position->prev = instr;
}

/* Splits the single-index `gep` (gep T p, a op b, the index distributing:
 * anvil_memory_index_distributes) into gep T (gep T p, first), rest, the
 * inner GEP placed before `position`.  False only on allocation failure,
 * the IR unchanged. */
static inline bool anvil_opt_split_gep(anvil_ctx_t *ctx, anvil_instr_t *gep, anvil_instr_t *position, anvil_value_t *first, anvil_value_t *rest)
{
    anvil_instr_t *base = anvil_instr_create(ctx, ANVIL_OP_GEP, gep->result->type, gep->result->name);
    if (!base || !anvil_instr_copy_attributes(base, gep) || !anvil_instr_add_operand(base, gep->operands[0]) || !anvil_instr_add_operand(base, first))
        return false;
    anvil_opt_insert_before(position, base);
    gep->operands[0] = base->result;
    gep->operands[1] = rest;
    return true;
}

/* The comparison that holds exactly where `op` (with `pred` for FCMP)
 * does not; false for an opcode that is not a comparison. */
static inline bool anvil_opt_inverse_compare(anvil_op_t op, anvil_fcmp_pred_t pred, anvil_op_t *inverse_op, anvil_fcmp_pred_t *inverse_pred)
{
    static const anvil_fcmp_pred_t fcmp_inverse[] = {
        [ANVIL_FCMP_FALSE] = ANVIL_FCMP_TRUE, [ANVIL_FCMP_OEQ] = ANVIL_FCMP_UNE, [ANVIL_FCMP_OGT] = ANVIL_FCMP_ULE,
        [ANVIL_FCMP_OGE] = ANVIL_FCMP_ULT,    [ANVIL_FCMP_OLT] = ANVIL_FCMP_UGE, [ANVIL_FCMP_OLE] = ANVIL_FCMP_UGT,
        [ANVIL_FCMP_ONE] = ANVIL_FCMP_UEQ,    [ANVIL_FCMP_ORD] = ANVIL_FCMP_UNO, [ANVIL_FCMP_UEQ] = ANVIL_FCMP_ONE,
        [ANVIL_FCMP_UGT] = ANVIL_FCMP_OLE,    [ANVIL_FCMP_UGE] = ANVIL_FCMP_OLT, [ANVIL_FCMP_ULT] = ANVIL_FCMP_OGE,
        [ANVIL_FCMP_ULE] = ANVIL_FCMP_OGT,    [ANVIL_FCMP_UNE] = ANVIL_FCMP_OEQ, [ANVIL_FCMP_UNO] = ANVIL_FCMP_ORD,
        [ANVIL_FCMP_TRUE] = ANVIL_FCMP_FALSE,
    };
    *inverse_pred = pred;
    switch (op) {
    case ANVIL_OP_CMP_EQ: *inverse_op = ANVIL_OP_CMP_NE; return true;
    case ANVIL_OP_CMP_NE: *inverse_op = ANVIL_OP_CMP_EQ; return true;
    case ANVIL_OP_CMP_LT: *inverse_op = ANVIL_OP_CMP_GE; return true;
    case ANVIL_OP_CMP_GE: *inverse_op = ANVIL_OP_CMP_LT; return true;
    case ANVIL_OP_CMP_LE: *inverse_op = ANVIL_OP_CMP_GT; return true;
    case ANVIL_OP_CMP_GT: *inverse_op = ANVIL_OP_CMP_LE; return true;
    case ANVIL_OP_CMP_ULT: *inverse_op = ANVIL_OP_CMP_UGE; return true;
    case ANVIL_OP_CMP_UGE: *inverse_op = ANVIL_OP_CMP_ULT; return true;
    case ANVIL_OP_CMP_ULE: *inverse_op = ANVIL_OP_CMP_UGT; return true;
    case ANVIL_OP_CMP_UGT: *inverse_op = ANVIL_OP_CMP_ULE; return true;
    case ANVIL_OP_FCMP:
        if ((unsigned)pred > (unsigned)ANVIL_FCMP_TRUE)
            return false;
        *inverse_op = op;
        *inverse_pred = fcmp_inverse[pred];
        return true;
    default:
        return false;
    }
}

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
    if (!anvil_opt_is_const_int(val) || !val->type) return false;

    unsigned bits;
    switch (val->type->kind) {
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:  bits = 8;  break;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16: bits = 16; break;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32: bits = 32; break;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64: bits = 64; break;
        default: return false;
    }

    uint64_t value = val->type->is_signed ? (uint64_t)val->data.i
                                          : val->data.u;
    uint64_t mask = bits == 64 ? UINT64_MAX
                               : ((UINT64_C(1) << bits) - 1);
    return (value & mask) == mask;
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

/* The high half of the 2*bits-bit product of a and b (their low `bits`,
 * bits <= 64; signed or unsigned): what UMULH/SMULH compute. */
static inline uint64_t anvil_opt_mul_high(uint64_t a, uint64_t b, unsigned bits, bool is_signed)
{
    uint64_t mask = bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    if (bits <= 32) {
        uint64_t sign = UINT64_C(1) << (bits - 1);
        int64_t x = is_signed ? (int64_t)(((a & mask) ^ sign) - sign) : (int64_t)(a & mask);
        int64_t y = is_signed ? (int64_t)(((b & mask) ^ sign) - sign) : (int64_t)(b & mask);
        return (uint64_t)((x * y) >> bits) & mask;
    }
    uint64_t al = a & 0xffffffffu, ah = a >> 32, bl = b & 0xffffffffu, bh = b >> 32;
    uint64_t low = al * bl, middle1 = ah * bl, middle2 = al * bh;
    uint64_t carry = ((low >> 32) + (middle1 & 0xffffffffu) + (middle2 & 0xffffffffu)) >> 32;
    uint64_t high = ah * bh + (middle1 >> 32) + (middle2 >> 32) + carry;
    /* A negative operand contributes -2^64 times the other one. */
    if (is_signed)
        high -= ((int64_t)a < 0 ? b : 0) + ((int64_t)b < 0 ? a : 0);
    return high;
}

/* Instructions without effects: they cannot trap, touch memory or the
 * floating-point environment.  Moving one to every path that already runs
 * it (sinking into successors, copying a loop's iterations) is safe. */
static inline bool anvil_opt_pure(const anvil_instr_t *instr)
{
    switch (instr->op) {
    case ANVIL_OP_ADD:
    case ANVIL_OP_SUB:
    case ANVIL_OP_MUL:
    case ANVIL_OP_UMULH:
    case ANVIL_OP_SMULH:
    case ANVIL_OP_NEG:
    case ANVIL_OP_AND:
    case ANVIL_OP_OR:
    case ANVIL_OP_XOR:
    case ANVIL_OP_NOT:
    case ANVIL_OP_SHL:
    case ANVIL_OP_SHR:
    case ANVIL_OP_SAR:
    case ANVIL_OP_ROTL:
    case ANVIL_OP_CMP_EQ:
    case ANVIL_OP_CMP_NE:
    case ANVIL_OP_CMP_LT:
    case ANVIL_OP_CMP_LE:
    case ANVIL_OP_CMP_GT:
    case ANVIL_OP_CMP_GE:
    case ANVIL_OP_CMP_ULT:
    case ANVIL_OP_CMP_ULE:
    case ANVIL_OP_CMP_UGT:
    case ANVIL_OP_CMP_UGE:
    case ANVIL_OP_SELECT:
    case ANVIL_OP_TRUNC:
    case ANVIL_OP_ZEXT:
    case ANVIL_OP_SEXT:
    case ANVIL_OP_BITCAST:
    case ANVIL_OP_PTRTOINT:
    case ANVIL_OP_INTTOPTR:
    case ANVIL_OP_GEP:
    case ANVIL_OP_STRUCT_GEP:
        return true;
    default:
        return false;
    }
}

/* Pure instructions that may also run where the code did not run them
 * (hoisted out of a loop, ahead of a branch): no operand makes them
 * undefined.  A shift count at or above the width is undefined behaviour,
 * so only a constant one below it qualifies. */
static inline bool anvil_opt_speculatable(const anvil_instr_t *instr)
{
    if (!anvil_opt_pure(instr))
        return false;
    if (instr->op != ANVIL_OP_SHL && instr->op != ANVIL_OP_SHR && instr->op != ANVIL_OP_SAR)
        return true;
    anvil_type_t *type = instr->num_operands == 2 ? instr->operands[0]->type : NULL;
    anvil_type_t *lane = type ? anvil_type_vector_element(type) : NULL;
    return type && anvil_opt_is_const_int(instr->operands[1]) &&
           (uint64_t)instr->operands[1]->data.i < (uint64_t)anvil_type_bit_width(lane ? lane : type);
}

/* Whether `value` is an integer constant whose bits, in its type's width,
 * are one power of two 2^shift. */
static inline bool anvil_opt_power_of_two(anvil_value_t *value, int *shift)
{
    if (!value || value->kind != ANVIL_VAL_CONST_INT || !value->type || !shift)
        return false;
    unsigned bits;
    switch (value->type->kind) {
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:  bits = 8;  break;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16: bits = 16; break;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32: bits = 32; break;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64: bits = 64; break;
        default: return false;
    }
    uint64_t mask = bits == 64 ? UINT64_MAX : ((UINT64_C(1) << bits) - 1);
    uint64_t v = anvil_const_int_unsigned_value(value) & mask;
    if (v == 0 || (v & (v - 1)) != 0)
        return false;
    int s = 0;
    while ((v >> s) != 1)
        s++;
    *shift = s;
    return true;
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
 * Previously five slightly different copies existed across passes.  The
 * canonical helper below covers every current call-site:
 *
 *   anvil_opt_replace_uses_in_func: rewrite every operand equal to old_val
 *     across every instruction of every block. Safe whenever new_val
 *     dominates every use of old_val (the common case when new_val replaces
 *     old_val at a fold/propagate site).
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

anvil_func_t **anvil_opt_call_order(anvil_module_t *module);
/* Whether `value` (an initializer, or a part of one) names `target`. */
bool anvil_opt_initializer_names(const anvil_value_t *value, const anvil_value_t *target);
/* Whether every reference to `func` in the module is the callee of a direct
 * call (in convention *cc when cc is given): no other operand and no
 * initializer names it.  *calls (when given) receives their number. */
bool anvil_opt_direct_calls(const anvil_module_t *mod, const anvil_func_t *func, const anvil_cc_t *cc, size_t *calls);

#endif /* ANVIL_OPT_UTILS_H */
