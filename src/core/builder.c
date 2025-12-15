/*
 * ANVIL - IR Builder implementation
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>

/* Helper to create binary operation */
static anvil_value_t *build_binop(anvil_ctx_t *ctx, anvil_op_t op,
                                   anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    if (!ctx || !lhs || !rhs) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, op, lhs->type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, lhs);
    anvil_instr_add_operand(instr, rhs);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

/* Helper to create unary operation */
static anvil_value_t *build_unop(anvil_ctx_t *ctx, anvil_op_t op,
                                  anvil_value_t *val, const char *name)
{
    if (!ctx || !val) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, op, val->type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

/* Helper to create comparison operation */
static anvil_value_t *build_cmp(anvil_ctx_t *ctx, anvil_op_t op,
                                 anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    if (!ctx || !lhs || !rhs) return NULL;
    
    /* Comparison result is always i8 (boolean) */
    anvil_instr_t *instr = anvil_instr_create(ctx, op, ctx->type_i8, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, lhs);
    anvil_instr_add_operand(instr, rhs);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

/* Arithmetic operations */
anvil_value_t *anvil_build_add(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_ADD, lhs, rhs, name);
}

anvil_value_t *anvil_build_sub(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SUB, lhs, rhs, name);
}

anvil_value_t *anvil_build_mul(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_MUL, lhs, rhs, name);
}

anvil_value_t *anvil_build_sdiv(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SDIV, lhs, rhs, name);
}

anvil_value_t *anvil_build_udiv(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_UDIV, lhs, rhs, name);
}

anvil_value_t *anvil_build_smod(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SMOD, lhs, rhs, name);
}

anvil_value_t *anvil_build_umod(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_UMOD, lhs, rhs, name);
}

anvil_value_t *anvil_build_neg(anvil_ctx_t *ctx, anvil_value_t *val, const char *name)
{
    return build_unop(ctx, ANVIL_OP_NEG, val, name);
}

/* Bitwise operations */
anvil_value_t *anvil_build_and(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_AND, lhs, rhs, name);
}

anvil_value_t *anvil_build_or(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_OR, lhs, rhs, name);
}

anvil_value_t *anvil_build_xor(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_XOR, lhs, rhs, name);
}

anvil_value_t *anvil_build_not(anvil_ctx_t *ctx, anvil_value_t *val, const char *name)
{
    return build_unop(ctx, ANVIL_OP_NOT, val, name);
}

anvil_value_t *anvil_build_shl(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *amt, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SHL, val, amt, name);
}

anvil_value_t *anvil_build_shr(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *amt, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SHR, val, amt, name);
}

anvil_value_t *anvil_build_sar(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *amt, const char *name)
{
    return build_binop(ctx, ANVIL_OP_SAR, val, amt, name);
}

/* Comparison operations */
anvil_value_t *anvil_build_cmp_eq(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_EQ, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_ne(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_NE, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_lt(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_LT, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_le(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_LE, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_gt(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_GT, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_ge(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_GE, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_ult(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_ULT, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_ule(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_ULE, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_ugt(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_UGT, lhs, rhs, name);
}

anvil_value_t *anvil_build_cmp_uge(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_CMP_UGE, lhs, rhs, name);
}

/* Memory operations */
anvil_value_t *anvil_build_alloca(anvil_ctx_t *ctx, anvil_type_t *type, const char *name)
{
    if (!ctx || !type) return NULL;
    
    anvil_type_t *ptr_type = anvil_type_ptr(ctx, type);
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_ALLOCA, ptr_type, name);
    if (!instr) return NULL;
    
    anvil_instr_insert(ctx, instr);
    return instr->result;
}

anvil_value_t *anvil_build_load(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *ptr, const char *name)
{
    if (!ctx || !type || !ptr) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_LOAD, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, ptr);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_store(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *ptr)
{
    if (!ctx || !val || !ptr) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_STORE, ctx->type_void, NULL);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_add_operand(instr, ptr);
    anvil_instr_insert(ctx, instr);
    
    return NULL; /* Store has no result */
}

anvil_value_t *anvil_build_gep(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *ptr,
                                anvil_value_t **indices, size_t num_indices, const char *name)
{
    if (!ctx || !type || !ptr) return NULL;
    
    anvil_type_t *ptr_type = anvil_type_ptr(ctx, type);
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_GEP, ptr_type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, ptr);
    for (size_t i = 0; i < num_indices; i++) {
        anvil_instr_add_operand(instr, indices[i]);
    }
    
    anvil_instr_insert(ctx, instr);
    return instr->result;
}

anvil_value_t *anvil_build_struct_gep(anvil_ctx_t *ctx, anvil_type_t *struct_type,
                                       anvil_value_t *ptr, unsigned field_idx, const char *name)
{
    if (!ctx || !struct_type || !ptr) return NULL;
    if (struct_type->kind != ANVIL_TYPE_STRUCT) return NULL;
    if (field_idx >= struct_type->data.struc.num_fields) return NULL;
    
    /* Get field type and create pointer to it */
    anvil_type_t *field_type = struct_type->data.struc.fields[field_idx];
    anvil_type_t *ptr_type = anvil_type_ptr(ctx, field_type);
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_STRUCT_GEP, ptr_type, name);
    if (!instr) return NULL;
    
    /* Store base pointer as operand */
    anvil_instr_add_operand(instr, ptr);
    
    /* Store field index as constant operand */
    anvil_value_t *idx_val = anvil_const_i32(ctx, (int32_t)field_idx);
    anvil_instr_add_operand(instr, idx_val);
    
    /* Store struct type reference for offset calculation */
    instr->aux_type = struct_type;
    
    anvil_instr_insert(ctx, instr);
    return instr->result;
}

/* Control flow */
anvil_value_t *anvil_build_br(anvil_ctx_t *ctx, anvil_block_t *dest)
{
    if (!ctx || !dest) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_BR, ctx->type_void, NULL);
    if (!instr) return NULL;
    
    instr->true_block = dest;
    anvil_instr_insert(ctx, instr);
    
    return NULL;
}

anvil_value_t *anvil_build_br_cond(anvil_ctx_t *ctx, anvil_value_t *cond,
                                    anvil_block_t *then_block, anvil_block_t *else_block)
{
    if (!ctx || !cond || !then_block || !else_block) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_BR_COND, ctx->type_void, NULL);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, cond);
    instr->true_block = then_block;
    instr->false_block = else_block;
    anvil_instr_insert(ctx, instr);
    
    return NULL;
}

anvil_value_t *anvil_build_call(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *callee,
                                 anvil_value_t **args, size_t num_args, const char *name)
{
    if (!ctx || !callee) return NULL;
    
    anvil_type_t *ret_type = type;
    if (type && type->kind == ANVIL_TYPE_FUNC) {
        ret_type = type->data.func.ret;
    }
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_CALL, ret_type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, callee);
    for (size_t i = 0; i < num_args; i++) {
        anvil_instr_add_operand(instr, args[i]);
    }
    
    anvil_instr_insert(ctx, instr);
    return instr->result;
}

anvil_value_t *anvil_build_ret(anvil_ctx_t *ctx, anvil_value_t *val)
{
    if (!ctx) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_RET, ctx->type_void, NULL);
    if (!instr) return NULL;
    
    if (val) {
        anvil_instr_add_operand(instr, val);
    }
    anvil_instr_insert(ctx, instr);
    
    return NULL;
}

anvil_value_t *anvil_build_ret_void(anvil_ctx_t *ctx)
{
    return anvil_build_ret(ctx, NULL);
}

/* Type conversions */
anvil_value_t *anvil_build_trunc(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_TRUNC, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_zext(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_ZEXT, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_sext(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_SEXT, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_bitcast(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_BITCAST, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_ptrtoint(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_PTRTOINT, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_inttoptr(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_INTTOPTR, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_fptrunc(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_FPTRUNC, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_fpext(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_FPEXT, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_fptosi(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_FPTOSI, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_fptoui(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_FPTOUI, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_sitofp(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_SITOFP, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

anvil_value_t *anvil_build_uitofp(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name)
{
    if (!ctx || !val || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_UITOFP, type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}

/* Floating-point operations */
anvil_value_t *anvil_build_fadd(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_FADD, lhs, rhs, name);
}

anvil_value_t *anvil_build_fsub(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_FSUB, lhs, rhs, name);
}

anvil_value_t *anvil_build_fmul(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_FMUL, lhs, rhs, name);
}

anvil_value_t *anvil_build_fdiv(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_binop(ctx, ANVIL_OP_FDIV, lhs, rhs, name);
}

anvil_value_t *anvil_build_fneg(anvil_ctx_t *ctx, anvil_value_t *val, const char *name)
{
    return build_unop(ctx, ANVIL_OP_FNEG, val, name);
}

anvil_value_t *anvil_build_fabs(anvil_ctx_t *ctx, anvil_value_t *val, const char *name)
{
    return build_unop(ctx, ANVIL_OP_FABS, val, name);
}

anvil_value_t *anvil_build_fcmp(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name)
{
    return build_cmp(ctx, ANVIL_OP_FCMP, lhs, rhs, name);
}

/* Misc */
anvil_value_t *anvil_build_phi(anvil_ctx_t *ctx, anvil_type_t *type, const char *name)
{
    if (!ctx || !type) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_PHI, type, name);
    if (!instr) return NULL;
    
    anvil_instr_insert(ctx, instr);
    return instr->result;
}

void anvil_phi_add_incoming(anvil_value_t *phi, anvil_value_t *val, anvil_block_t *block)
{
    if (!phi || !val || !block) return;
    if (phi->kind != ANVIL_VAL_INSTR) return;
    
    anvil_instr_t *instr = phi->data.instr;
    if (instr->op != ANVIL_OP_PHI) return;
    
    anvil_instr_add_operand(instr, val);
    
    /* Add block to phi_blocks array */
    size_t new_count = instr->num_phi_incoming + 1;
    anvil_block_t **new_blocks = realloc(instr->phi_blocks, new_count * sizeof(anvil_block_t *));
    if (!new_blocks) return;
    
    new_blocks[instr->num_phi_incoming] = block;
    instr->phi_blocks = new_blocks;
    instr->num_phi_incoming = new_count;
}

anvil_value_t *anvil_build_select(anvil_ctx_t *ctx, anvil_value_t *cond,
                                   anvil_value_t *then_val, anvil_value_t *else_val, const char *name)
{
    if (!ctx || !cond || !then_val || !else_val) return NULL;
    
    anvil_instr_t *instr = anvil_instr_create(ctx, ANVIL_OP_SELECT, then_val->type, name);
    if (!instr) return NULL;
    
    anvil_instr_add_operand(instr, cond);
    anvil_instr_add_operand(instr, then_val);
    anvil_instr_add_operand(instr, else_val);
    anvil_instr_insert(ctx, instr);
    
    return instr->result;
}
