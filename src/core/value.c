/*
 * ANVIL - Value and instruction implementation
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>

anvil_value_t *anvil_value_create(anvil_ctx_t *ctx, anvil_val_kind_t kind,
                                   anvil_type_t *type, const char *name)
{
    if (!ctx) return NULL;
    
    anvil_value_t *val = calloc(1, sizeof(anvil_value_t));
    if (!val) return NULL;
    
    val->kind = kind;
    val->type = type;
    val->name = name ? strdup(name) : NULL;
    val->id = ctx->next_value_id++;
    
    return val;
}

anvil_instr_t *anvil_instr_create(anvil_ctx_t *ctx, anvil_op_t op,
                                   anvil_type_t *type, const char *name)
{
    if (!ctx) return NULL;
    
    anvil_instr_t *instr = calloc(1, sizeof(anvil_instr_t));
    if (!instr) return NULL;
    
    instr->op = op;
    
    /* Create result value if not void */
    if (type && type->kind != ANVIL_TYPE_VOID) {
        instr->result = anvil_value_create(ctx, ANVIL_VAL_INSTR, type, name);
        if (instr->result) {
            instr->result->data.instr = instr;
        }
    }
    
    return instr;
}

void anvil_instr_add_operand(anvil_instr_t *instr, anvil_value_t *val)
{
    if (!instr) return;
    
    size_t new_count = instr->num_operands + 1;
    anvil_value_t **new_ops = realloc(instr->operands, new_count * sizeof(anvil_value_t *));
    if (!new_ops) return;
    
    new_ops[instr->num_operands] = val;
    instr->operands = new_ops;
    instr->num_operands = new_count;
}

void anvil_instr_insert(anvil_ctx_t *ctx, anvil_instr_t *instr)
{
    if (!ctx || !instr || !ctx->insert_block) return;
    
    anvil_block_t *block = ctx->insert_block;
    instr->parent = block;
    
    if (!block->first) {
        block->first = instr;
        block->last = instr;
    } else {
        instr->prev = block->last;
        block->last->next = instr;
        block->last = instr;
    }
}

/* Constants */
anvil_value_t *anvil_const_i8(anvil_ctx_t *ctx, int8_t val)
{
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_i8, NULL);
    if (v) v->data.i = val;
    return v;
}

anvil_value_t *anvil_const_i16(anvil_ctx_t *ctx, int16_t val)
{
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_i16, NULL);
    if (v) v->data.i = val;
    return v;
}

anvil_value_t *anvil_const_i32(anvil_ctx_t *ctx, int32_t val)
{
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_i32, NULL);
    if (v) v->data.i = val;
    return v;
}

anvil_value_t *anvil_const_i64(anvil_ctx_t *ctx, int64_t val)
{
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_i64, NULL);
    if (v) v->data.i = val;
    return v;
}

anvil_value_t *anvil_const_u8(anvil_ctx_t *ctx, uint8_t val)
{
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_u8, NULL);
    if (v) v->data.u = val;
    return v;
}

anvil_value_t *anvil_const_u16(anvil_ctx_t *ctx, uint16_t val)
{
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_u16, NULL);
    if (v) v->data.u = val;
    return v;
}

anvil_value_t *anvil_const_u32(anvil_ctx_t *ctx, uint32_t val)
{
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_u32, NULL);
    if (v) v->data.u = val;
    return v;
}

anvil_value_t *anvil_const_u64(anvil_ctx_t *ctx, uint64_t val)
{
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_u64, NULL);
    if (v) v->data.u = val;
    return v;
}

anvil_value_t *anvil_const_f32(anvil_ctx_t *ctx, float val)
{
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_FLOAT, ctx->type_f32, NULL);
    if (v) v->data.f = val;
    return v;
}

anvil_value_t *anvil_const_f64(anvil_ctx_t *ctx, double val)
{
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_FLOAT, ctx->type_f64, NULL);
    if (v) v->data.f = val;
    return v;
}

anvil_value_t *anvil_const_null(anvil_ctx_t *ctx, anvil_type_t *ptr_type)
{
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_NULL, ptr_type, NULL);
    if (v) v->data.u = 0;
    return v;
}

anvil_value_t *anvil_const_string(anvil_ctx_t *ctx, const char *str)
{
    anvil_type_t *type = anvil_type_ptr(ctx, ctx->type_i8);
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_STRING, type, NULL);
    if (v) v->data.str = str ? strdup(str) : NULL;
    return v;
}
