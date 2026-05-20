/*
 * ANVIL - Source-level IR verifier.
 */

#include "anvil/anvil_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool verify_fail(char *error, size_t error_len, const char *fmt, ...)
{
    if (error && error_len > 0) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, error_len, fmt, args);
        va_end(args);
    }
    return false;
}

static const char *func_name(const anvil_func_t *func)
{
    return (func && func->name) ? func->name : "<anon>";
}

static const char *block_name(const anvil_block_t *block)
{
    return (block && block->name) ? block->name : "<anon>";
}

static bool type_is_integer(const anvil_type_t *type)
{
    if (!type) return false;
    switch (type->kind) {
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U8:
        case ANVIL_TYPE_U16:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_U64:
            return true;
        default:
            return false;
    }
}

static bool type_is_float(const anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_F32 ||
                    type->kind == ANVIL_TYPE_F64);
}

static bool type_is_void(const anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_VOID;
}

static bool type_is_bool_like(const anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_I8 ||
                    type->kind == ANVIL_TYPE_U8);
}

static bool type_equal_depth(const anvil_type_t *lhs,
                             const anvil_type_t *rhs,
                             unsigned depth)
{
    if (lhs == rhs) return true;
    if (!lhs || !rhs) return false;
    if (lhs->kind != rhs->kind) return false;
    if (depth > 32) return false;

    switch (lhs->kind) {
        case ANVIL_TYPE_VOID:
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U8:
        case ANVIL_TYPE_U16:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F32:
        case ANVIL_TYPE_F64:
            return true;

        case ANVIL_TYPE_DECIMAL:
            return lhs->data.decimal.encoding == rhs->data.decimal.encoding &&
                   lhs->data.decimal.precision == rhs->data.decimal.precision &&
                   lhs->data.decimal.scale == rhs->data.decimal.scale;

        case ANVIL_TYPE_PTR:
            return type_equal_depth(lhs->data.pointee,
                                    rhs->data.pointee,
                                    depth + 1);

        case ANVIL_TYPE_ARRAY:
            return lhs->data.array.count == rhs->data.array.count &&
                   type_equal_depth(lhs->data.array.elem,
                                    rhs->data.array.elem,
                                    depth + 1);

        case ANVIL_TYPE_STRUCT:
            if (lhs->data.struc.num_fields != rhs->data.struc.num_fields) {
                return false;
            }
            for (size_t i = 0; i < lhs->data.struc.num_fields; i++) {
                if (!type_equal_depth(lhs->data.struc.fields[i],
                                      rhs->data.struc.fields[i],
                                      depth + 1)) {
                    return false;
                }
            }
            return true;

        case ANVIL_TYPE_FUNC:
            if (!type_equal_depth(lhs->data.func.ret,
                                  rhs->data.func.ret,
                                  depth + 1) ||
                lhs->data.func.num_params != rhs->data.func.num_params ||
                lhs->data.func.variadic != rhs->data.func.variadic) {
                return false;
            }
            for (size_t i = 0; i < lhs->data.func.num_params; i++) {
                if (!type_equal_depth(lhs->data.func.params[i],
                                      rhs->data.func.params[i],
                                      depth + 1)) {
                    return false;
                }
            }
            return true;
    }

    return false;
}

static bool type_equal(const anvil_type_t *lhs, const anvil_type_t *rhs)
{
    return type_equal_depth(lhs, rhs, 0);
}

static bool block_belongs_to_func(const anvil_func_t *func,
                                  const anvil_block_t *block)
{
    if (!func || !block) return false;
    for (const anvil_block_t *cur = func->blocks; cur; cur = cur->next) {
        if (cur == block) return true;
    }
    return false;
}

static bool module_has_global_value(const anvil_module_t *mod,
                                    const anvil_value_t *value)
{
    if (!mod || !value) return false;
    for (const anvil_global_t *global = mod->globals; global; global = global->next) {
        if (global->value == value) return true;
    }
    return false;
}

static bool module_has_func_value(const anvil_module_t *mod,
                                  const anvil_value_t *value)
{
    if (!mod || !value) return false;
    for (const anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (func->value == value) return true;
    }
    return false;
}

static bool func_has_param_value(const anvil_func_t *func,
                                 const anvil_value_t *value)
{
    if (!func || !value || value->kind != ANVIL_VAL_PARAM) return false;
    if (value->data.param.func != func) return false;
    if (value->data.param.index >= func->num_params) return false;
    return func->params && func->params[value->data.param.index] == value;
}

static bool func_has_instr_result(const anvil_func_t *func,
                                  const anvil_value_t *value)
{
    if (!func || !value || value->kind != ANVIL_VAL_INSTR ||
        !value->data.instr) {
        return false;
    }
    const anvil_instr_t *instr = value->data.instr;
    return instr->result == value &&
           instr->parent &&
           instr->parent->parent == func &&
           block_belongs_to_func(func, instr->parent);
}

static bool verify_value_ref(const anvil_module_t *mod,
                             const anvil_func_t *func,
                             const anvil_value_t *value,
                             char *error,
                             size_t error_len)
{
    if (!value) {
        return verify_fail(error, error_len,
                           "function %s references a null value",
                           func_name(func));
    }
    if (!value->type) {
        return verify_fail(error, error_len,
                           "function %s references a value without type",
                           func_name(func));
    }

    switch (value->kind) {
        case ANVIL_VAL_CONST_INT:
        case ANVIL_VAL_CONST_FLOAT:
        case ANVIL_VAL_CONST_DECIMAL:
        case ANVIL_VAL_CONST_STRING:
            return true;

        case ANVIL_VAL_CONST_NULL:
            if (value->type->kind == ANVIL_TYPE_PTR) return true;
            return verify_fail(error, error_len,
                               "function %s has null constant with non-pointer type",
                               func_name(func));

        case ANVIL_VAL_CONST_ARRAY:
            if (value->type->kind != ANVIL_TYPE_ARRAY ||
                value->data.array.num_elements != value->type->data.array.count) {
                return verify_fail(error, error_len,
                                   "function %s has malformed array constant",
                                   func_name(func));
            }
            for (size_t i = 0; i < value->data.array.num_elements; i++) {
                anvil_value_t *element = value->data.array.elements[i];
                if (!verify_value_ref(mod, func, element, error, error_len)) {
                    return false;
                }
                if (!type_equal(element->type, value->type->data.array.elem)) {
                    return verify_fail(error, error_len,
                                       "function %s has array constant element type mismatch",
                                       func_name(func));
                }
            }
            return true;

        case ANVIL_VAL_GLOBAL:
            if (module_has_global_value(mod, value)) return true;
            return verify_fail(error, error_len,
                               "function %s references a global outside its module",
                               func_name(func));

        case ANVIL_VAL_FUNC:
            if (module_has_func_value(mod, value)) return true;
            return verify_fail(error, error_len,
                               "function %s references a function outside its module",
                               func_name(func));

        case ANVIL_VAL_PARAM:
            if (func_has_param_value(func, value)) return true;
            return verify_fail(error, error_len,
                               "function %s references a parameter outside the function",
                               func_name(func));

        case ANVIL_VAL_INSTR:
            if (func_has_instr_result(func, value)) return true;
            return verify_fail(error, error_len,
                               "function %s references an instruction result outside the function",
                               func_name(func));

        case ANVIL_VAL_BLOCK:
            break;
    }

    return verify_fail(error, error_len,
                       "function %s references an unsupported value kind",
                       func_name(func));
}

static anvil_type_t *memory_object_type(const anvil_value_t *value)
{
    if (!value || !value->type) return NULL;
    if (value->kind == ANVIL_VAL_GLOBAL && value->type->kind != ANVIL_TYPE_FUNC) {
        return value->type;
    }
    if (value->type->kind == ANVIL_TYPE_PTR) {
        return value->type->data.pointee;
    }
    return NULL;
}

static anvil_type_t *callee_func_type(const anvil_value_t *callee)
{
    if (!callee || !callee->type) return NULL;
    if (callee->type->kind == ANVIL_TYPE_FUNC) return callee->type;
    if (callee->type->kind == ANVIL_TYPE_PTR &&
        callee->type->data.pointee &&
        callee->type->data.pointee->kind == ANVIL_TYPE_FUNC) {
        return callee->type->data.pointee;
    }
    return NULL;
}

static bool op_is_terminator(anvil_op_t op)
{
    return op == ANVIL_OP_RET ||
           op == ANVIL_OP_BR ||
           op == ANVIL_OP_BR_COND ||
           op == ANVIL_OP_SWITCH;
}

static bool block_has_successor(const anvil_block_t *from,
                                const anvil_block_t *to)
{
    if (!from || !to || !from->last) return false;
    if (from->last->op == ANVIL_OP_BR) {
        return from->last->true_block == to;
    }
    if (from->last->op == ANVIL_OP_BR_COND) {
        return from->last->true_block == to ||
               from->last->false_block == to;
    }
    if (from->last->op == ANVIL_OP_SWITCH) {
        if (from->last->true_block == to) return true;
        for (size_t i = 0; i < from->last->num_switch_cases; i++) {
            if (from->last->switch_blocks &&
                from->last->switch_blocks[i] == to) {
                return true;
            }
        }
    }
    return false;
}

static bool phi_has_incoming_from(const anvil_instr_t *phi,
                                  const anvil_block_t *block)
{
    if (!phi || !block) return false;
    for (size_t i = 0; i < phi->num_phi_incoming; i++) {
        if (phi->phi_blocks && phi->phi_blocks[i] == block) return true;
    }
    return false;
}

static bool verify_same_type_operands(const anvil_module_t *mod,
                                      const anvil_func_t *func,
                                      const anvil_instr_t *instr,
                                      const char *kind,
                                      char *error,
                                      size_t error_len)
{
    if (instr->num_operands != 2 || !instr->result) {
        return verify_fail(error, error_len,
                           "%s in function %s must have two operands and a result",
                           kind, func_name(func));
    }

    anvil_value_t *lhs = instr->operands[0];
    anvil_value_t *rhs = instr->operands[1];
    if (!verify_value_ref(mod, func, lhs, error, error_len) ||
        !verify_value_ref(mod, func, rhs, error, error_len)) {
        return false;
    }
    if (!type_equal(lhs->type, rhs->type) ||
        !type_equal(instr->result->type, lhs->type)) {
        return verify_fail(error, error_len,
                           "%s in function %s has operand type mismatch",
                           kind, func_name(func));
    }
    return true;
}

static bool verify_binop(const anvil_module_t *mod,
                         const anvil_func_t *func,
                         const anvil_instr_t *instr,
                         char *error,
                         size_t error_len)
{
    if (!verify_same_type_operands(mod, func, instr, "binary instruction",
                                   error, error_len)) {
        return false;
    }

    anvil_type_t *type = instr->result->type;
    switch (instr->op) {
        case ANVIL_OP_ADD:
        case ANVIL_OP_SUB:
        case ANVIL_OP_MUL:
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
        case ANVIL_OP_SMOD:
        case ANVIL_OP_UMOD:
        case ANVIL_OP_AND:
        case ANVIL_OP_OR:
        case ANVIL_OP_XOR:
            if (type_is_integer(type)) return true;
            break;

        case ANVIL_OP_FADD:
        case ANVIL_OP_FSUB:
        case ANVIL_OP_FMUL:
        case ANVIL_OP_FDIV:
            if (type_is_float(type)) return true;
            break;

        default:
            break;
    }

    return verify_fail(error, error_len,
                       "binary instruction in function %s uses an invalid operand type",
                       func_name(func));
}

static bool verify_shift(const anvil_module_t *mod,
                         const anvil_func_t *func,
                         const anvil_instr_t *instr,
                         char *error,
                         size_t error_len)
{
    if (instr->num_operands != 2 || !instr->result) {
        return verify_fail(error, error_len,
                           "shift in function %s must have two operands and a result",
                           func_name(func));
    }
    anvil_value_t *value = instr->operands[0];
    anvil_value_t *amount = instr->operands[1];
    if (!verify_value_ref(mod, func, value, error, error_len) ||
        !verify_value_ref(mod, func, amount, error, error_len)) {
        return false;
    }
    if (!type_is_integer(value->type) ||
        !type_is_integer(amount->type) ||
        !type_equal(instr->result->type, value->type)) {
        return verify_fail(error, error_len,
                           "shift in function %s has invalid operand type",
                           func_name(func));
    }
    return true;
}

static bool verify_cmp(const anvil_module_t *mod,
                       const anvil_func_t *func,
                       const anvil_instr_t *instr,
                       char *error,
                       size_t error_len)
{
    if (instr->num_operands != 2 || !instr->result) {
        return verify_fail(error, error_len,
                           "comparison in function %s must have two operands and a result",
                           func_name(func));
    }
    anvil_value_t *lhs = instr->operands[0];
    anvil_value_t *rhs = instr->operands[1];
    if (!verify_value_ref(mod, func, lhs, error, error_len) ||
        !verify_value_ref(mod, func, rhs, error, error_len)) {
        return false;
    }
    if (!type_equal(lhs->type, rhs->type) ||
        !type_is_bool_like(instr->result->type)) {
        return verify_fail(error, error_len,
                           "comparison in function %s has operand/result type mismatch",
                           func_name(func));
    }

    if (instr->op == ANVIL_OP_FCMP) {
        if (type_is_float(lhs->type)) return true;
    } else if (type_is_integer(lhs->type) || lhs->type->kind == ANVIL_TYPE_PTR) {
        return true;
    }

    return verify_fail(error, error_len,
                       "comparison in function %s uses an invalid operand type",
                       func_name(func));
}

static bool verify_unop(const anvil_module_t *mod,
                        const anvil_func_t *func,
                        const anvil_instr_t *instr,
                        char *error,
                        size_t error_len)
{
    if (instr->num_operands != 1 || !instr->result) {
        return verify_fail(error, error_len,
                           "unary instruction in function %s must have one operand and a result",
                           func_name(func));
    }
    anvil_value_t *src = instr->operands[0];
    if (!verify_value_ref(mod, func, src, error, error_len)) return false;
    if (!type_equal(src->type, instr->result->type)) {
        return verify_fail(error, error_len,
                           "unary instruction in function %s has result type mismatch",
                           func_name(func));
    }

    if ((instr->op == ANVIL_OP_NEG && type_is_integer(src->type)) ||
        (instr->op == ANVIL_OP_NOT && type_is_integer(src->type)) ||
        (instr->op == ANVIL_OP_FNEG && type_is_float(src->type)) ||
        (instr->op == ANVIL_OP_FABS && type_is_float(src->type))) {
        return true;
    }

    return verify_fail(error, error_len,
                       "unary instruction in function %s uses an invalid operand type",
                       func_name(func));
}

static bool verify_cast(const anvil_module_t *mod,
                        const anvil_func_t *func,
                        const anvil_instr_t *instr,
                        char *error,
                        size_t error_len)
{
    if (instr->num_operands != 1 || !instr->result) {
        return verify_fail(error, error_len,
                           "cast in function %s must have one operand and a result",
                           func_name(func));
    }
    anvil_value_t *src = instr->operands[0];
    anvil_type_t *dst_type = instr->result->type;
    if (!verify_value_ref(mod, func, src, error, error_len)) return false;

    switch (instr->op) {
        case ANVIL_OP_TRUNC:
            if (type_is_integer(src->type) && type_is_integer(dst_type) &&
                src->type->size > dst_type->size) return true;
            break;
        case ANVIL_OP_ZEXT:
        case ANVIL_OP_SEXT:
            if (type_is_integer(src->type) && type_is_integer(dst_type) &&
                src->type->size < dst_type->size) return true;
            break;
        case ANVIL_OP_FPTRUNC:
            if (type_is_float(src->type) && type_is_float(dst_type) &&
                src->type->size > dst_type->size) return true;
            break;
        case ANVIL_OP_FPEXT:
            if (type_is_float(src->type) && type_is_float(dst_type) &&
                src->type->size < dst_type->size) return true;
            break;
        case ANVIL_OP_FPTOSI:
        case ANVIL_OP_FPTOUI:
            if (type_is_float(src->type) && type_is_integer(dst_type)) return true;
            break;
        case ANVIL_OP_SITOFP:
        case ANVIL_OP_UITOFP:
            if (type_is_integer(src->type) && type_is_float(dst_type)) return true;
            break;
        case ANVIL_OP_PTRTOINT:
            if (src->type->kind == ANVIL_TYPE_PTR && type_is_integer(dst_type)) {
                return true;
            }
            break;
        case ANVIL_OP_INTTOPTR:
            if (type_is_integer(src->type) && dst_type->kind == ANVIL_TYPE_PTR) {
                return true;
            }
            break;
        case ANVIL_OP_BITCAST:
            if (!type_is_void(src->type) && !type_is_void(dst_type) &&
                src->type->size == dst_type->size) {
                return true;
            }
            break;
        default:
            break;
    }

    return verify_fail(error, error_len,
                       "cast in function %s uses incompatible source/result types",
                       func_name(func));
}

static bool verify_memory(const anvil_module_t *mod,
                          const anvil_func_t *func,
                          const anvil_instr_t *instr,
                          char *error,
                          size_t error_len)
{
    if (instr->op == ANVIL_OP_LOAD) {
        if (instr->num_operands != 1 || !instr->result) {
            return verify_fail(error, error_len,
                               "load in function %s must have one address and a result",
                               func_name(func));
        }
        anvil_value_t *addr = instr->operands[0];
        if (!verify_value_ref(mod, func, addr, error, error_len)) return false;
        anvil_type_t *object_type = memory_object_type(addr);
        if (!object_type || !type_equal(object_type, instr->result->type)) {
            return verify_fail(error, error_len,
                               "load in function %s has address/result type mismatch",
                               func_name(func));
        }
        return true;
    }

    if (instr->op == ANVIL_OP_STORE) {
        if (instr->num_operands != 2 || instr->result) {
            return verify_fail(error, error_len,
                               "store in function %s must have value/address operands and no result",
                               func_name(func));
        }
        anvil_value_t *value = instr->operands[0];
        anvil_value_t *addr = instr->operands[1];
        if (!verify_value_ref(mod, func, value, error, error_len) ||
            !verify_value_ref(mod, func, addr, error, error_len)) {
            return false;
        }
        anvil_type_t *object_type = memory_object_type(addr);
        if (!object_type || !type_equal(object_type, value->type)) {
            return verify_fail(error, error_len,
                               "store in function %s has value/address type mismatch",
                               func_name(func));
        }
        return true;
    }

    return verify_fail(error, error_len,
                       "internal verifier error in function %s",
                       func_name(func));
}

static bool verify_gep(const anvil_module_t *mod,
                       const anvil_func_t *func,
                       const anvil_instr_t *instr,
                       char *error,
                       size_t error_len)
{
    if (instr->num_operands < 1 || !instr->result ||
        instr->result->type->kind != ANVIL_TYPE_PTR) {
        return verify_fail(error, error_len,
                           "GEP in function %s must have an address operand and pointer result",
                           func_name(func));
    }
    if (!verify_value_ref(mod, func, instr->operands[0], error, error_len)) {
        return false;
    }
    if (!memory_object_type(instr->operands[0])) {
        return verify_fail(error, error_len,
                           "GEP in function %s requires an addressable base",
                           func_name(func));
    }
    for (size_t i = 1; i < instr->num_operands; i++) {
        anvil_value_t *index = instr->operands[i];
        if (!verify_value_ref(mod, func, index, error, error_len)) return false;
        if (!type_is_integer(index->type)) {
            return verify_fail(error, error_len,
                               "GEP in function %s uses a non-integer index",
                               func_name(func));
        }
    }
    return true;
}

static bool verify_struct_gep(const anvil_module_t *mod,
                              const anvil_func_t *func,
                              const anvil_instr_t *instr,
                              char *error,
                              size_t error_len)
{
    if (instr->num_operands != 2 || !instr->result ||
        instr->result->type->kind != ANVIL_TYPE_PTR ||
        !instr->aux_type ||
        instr->aux_type->kind != ANVIL_TYPE_STRUCT) {
        return verify_fail(error, error_len,
                           "struct GEP in function %s is malformed",
                           func_name(func));
    }

    anvil_value_t *base = instr->operands[0];
    anvil_value_t *index = instr->operands[1];
    if (!verify_value_ref(mod, func, base, error, error_len) ||
        !verify_value_ref(mod, func, index, error, error_len)) {
        return false;
    }
    if (!memory_object_type(base) ||
        index->kind != ANVIL_VAL_CONST_INT ||
        index->data.i < 0 ||
        (size_t)index->data.i >= instr->aux_type->data.struc.num_fields) {
        return verify_fail(error, error_len,
                           "struct GEP in function %s has invalid base or field index",
                           func_name(func));
    }

    anvil_type_t *field_type =
        instr->aux_type->data.struc.fields[(size_t)index->data.i];
    if (!type_equal(instr->result->type->data.pointee, field_type)) {
        return verify_fail(error, error_len,
                           "struct GEP in function %s has field/result type mismatch",
                           func_name(func));
    }
    return true;
}

static bool verify_call(const anvil_module_t *mod,
                        const anvil_func_t *func,
                        const anvil_instr_t *instr,
                        char *error,
                        size_t error_len)
{
    if (instr->num_operands < 1) {
        return verify_fail(error, error_len,
                           "call in function %s must have a callee operand",
                           func_name(func));
    }
    anvil_value_t *callee = instr->operands[0];
    if (!verify_value_ref(mod, func, callee, error, error_len)) return false;

    anvil_type_t *fn_type = callee_func_type(callee);
    if (!fn_type) {
        return verify_fail(error, error_len,
                           "call in function %s targets a non-function value",
                           func_name(func));
    }

    size_t num_args = instr->num_operands - 1;
    size_t num_fixed = fn_type->data.func.num_params;
    if ((!fn_type->data.func.variadic && num_args != num_fixed) ||
        (fn_type->data.func.variadic && num_args < num_fixed)) {
        return verify_fail(error, error_len,
                           "call in function %s has wrong argument count",
                           func_name(func));
    }

    for (size_t i = 0; i < num_args; i++) {
        anvil_value_t *arg = instr->operands[i + 1];
        if (!verify_value_ref(mod, func, arg, error, error_len)) return false;
        if (i < num_fixed &&
            !type_equal(arg->type, fn_type->data.func.params[i])) {
            return verify_fail(error, error_len,
                               "call in function %s has argument type mismatch",
                               func_name(func));
        }
    }

    anvil_type_t *ret_type = fn_type->data.func.ret;
    if (type_is_void(ret_type)) {
        if (!instr->result) return true;
    } else if (instr->result && type_equal(instr->result->type, ret_type)) {
        return true;
    }

    return verify_fail(error, error_len,
                       "call in function %s has return type mismatch",
                       func_name(func));
}

static bool verify_select(const anvil_module_t *mod,
                          const anvil_func_t *func,
                          const anvil_instr_t *instr,
                          char *error,
                          size_t error_len)
{
    if (instr->num_operands != 3 || !instr->result) {
        return verify_fail(error, error_len,
                           "select in function %s must have cond/then/else operands and a result",
                           func_name(func));
    }
    anvil_value_t *cond = instr->operands[0];
    anvil_value_t *then_val = instr->operands[1];
    anvil_value_t *else_val = instr->operands[2];
    if (!verify_value_ref(mod, func, cond, error, error_len) ||
        !verify_value_ref(mod, func, then_val, error, error_len) ||
        !verify_value_ref(mod, func, else_val, error, error_len)) {
        return false;
    }
    if (!type_is_bool_like(cond->type) ||
        !type_equal(then_val->type, else_val->type) ||
        !type_equal(instr->result->type, then_val->type)) {
        return verify_fail(error, error_len,
                           "select in function %s has operand/result type mismatch",
                           func_name(func));
    }
    return true;
}

static bool verify_phi(const anvil_module_t *mod,
                       const anvil_func_t *func,
                       const anvil_instr_t *instr,
                       char *error,
                       size_t error_len)
{
    if (!instr->result || instr->num_phi_incoming == 0 ||
        instr->num_operands != instr->num_phi_incoming ||
        !instr->phi_blocks) {
        return verify_fail(error, error_len,
                           "PHI in function %s is malformed",
                           func_name(func));
    }

    for (size_t i = 0; i < instr->num_phi_incoming; i++) {
        anvil_value_t *incoming = instr->operands[i];
        anvil_block_t *incoming_block = instr->phi_blocks[i];
        if (!verify_value_ref(mod, func, incoming, error, error_len)) {
            return false;
        }
        if (!type_equal(incoming->type, instr->result->type)) {
            return verify_fail(error, error_len,
                               "PHI in function %s has incoming value type mismatch",
                               func_name(func));
        }
        if (!block_belongs_to_func(func, incoming_block) ||
            !block_has_successor(incoming_block, instr->parent)) {
            return verify_fail(error, error_len,
                               "PHI in function %s references a non-predecessor block",
                               func_name(func));
        }
    }

    for (const anvil_block_t *pred = func->blocks; pred; pred = pred->next) {
        if (block_has_successor(pred, instr->parent) &&
            !phi_has_incoming_from(instr, pred)) {
            return verify_fail(error, error_len,
                               "PHI in function %s is missing an incoming predecessor",
                               func_name(func));
        }
    }

    return true;
}

static bool verify_branch(const anvil_func_t *func,
                          const anvil_instr_t *instr,
                          char *error,
                          size_t error_len)
{
    if (instr->op == ANVIL_OP_BR) {
        if (instr->num_operands == 0 &&
            !instr->result &&
            block_belongs_to_func(func, instr->true_block)) {
            return true;
        }
        return verify_fail(error, error_len,
                           "branch in function %s has invalid target",
                           func_name(func));
    }

    if (instr->op == ANVIL_OP_BR_COND) {
        if (instr->num_operands != 1 || instr->result) {
            return verify_fail(error, error_len,
                               "conditional branch in function %s is malformed",
                               func_name(func));
        }
        anvil_value_t *cond = instr->operands[0];
        if (!verify_value_ref(func->parent, func, cond, error, error_len)) {
            return false;
        }
        if (!type_is_bool_like(cond->type)) {
            return verify_fail(error, error_len,
                               "conditional branch in function %s requires a boolean condition",
                               func_name(func));
        }
        if (block_belongs_to_func(func, instr->true_block) &&
            block_belongs_to_func(func, instr->false_block)) {
            return true;
        }
        return verify_fail(error, error_len,
                           "conditional branch in function %s has invalid target",
                           func_name(func));
    }

    return verify_fail(error, error_len,
                       "internal verifier error in function %s",
                       func_name(func));
}

static bool verify_switch(const anvil_module_t *mod,
                          const anvil_func_t *func,
                          const anvil_instr_t *instr,
                          char *error,
                          size_t error_len)
{
    if (instr->num_operands != instr->num_switch_cases + 1 ||
        instr->num_operands < 1 ||
        instr->result ||
        instr->false_block ||
        !block_belongs_to_func(func, instr->true_block)) {
        return verify_fail(error, error_len,
                           "switch in function %s is malformed",
                           func_name(func));
    }

    anvil_value_t *selector = instr->operands[0];
    if (!verify_value_ref(mod, func, selector, error, error_len)) return false;
    if (!type_is_integer(selector->type)) {
        return verify_fail(error, error_len,
                           "switch in function %s requires an integer selector",
                           func_name(func));
    }

    for (size_t i = 0; i < instr->num_switch_cases; i++) {
        anvil_value_t *case_value = instr->operands[i + 1];
        anvil_block_t *case_block = instr->switch_blocks
            ? instr->switch_blocks[i]
            : NULL;
        if (!verify_value_ref(mod, func, case_value, error, error_len)) {
            return false;
        }
        if (case_value->kind != ANVIL_VAL_CONST_INT ||
            !type_equal(case_value->type, selector->type)) {
            return verify_fail(error, error_len,
                               "switch in function %s has case type mismatch",
                               func_name(func));
        }
        if (!block_belongs_to_func(func, case_block)) {
            return verify_fail(error, error_len,
                               "switch in function %s has a case with invalid target",
                               func_name(func));
        }
        for (size_t j = 0; j < i; j++) {
            if (instr->operands[j + 1] &&
                instr->operands[j + 1]->data.u == case_value->data.u) {
                return verify_fail(error, error_len,
                                   "switch in function %s has duplicate case value",
                                   func_name(func));
            }
        }
    }

    return true;
}

static bool verify_ret(const anvil_func_t *func,
                       const anvil_instr_t *instr,
                       char *error,
                       size_t error_len)
{
    anvil_type_t *ret_type = func->type->data.func.ret;
    if (type_is_void(ret_type)) {
        if (instr->num_operands == 0 && !instr->result) return true;
        return verify_fail(error, error_len,
                           "return in function %s returns a value from void function",
                           func_name(func));
    }

    if (instr->num_operands != 1 || instr->result) {
        return verify_fail(error, error_len,
                           "return in function %s must return one value",
                           func_name(func));
    }
    anvil_value_t *value = instr->operands[0];
    if (!verify_value_ref(func->parent, func, value, error, error_len)) {
        return false;
    }
    if (!value || !type_equal(value->type, ret_type)) {
        return verify_fail(error, error_len,
                           "return in function %s has result type mismatch",
                           func_name(func));
    }
    return true;
}

static bool verify_alloca(const anvil_module_t *mod,
                          const anvil_func_t *func,
                          const anvil_instr_t *instr,
                          char *error,
                          size_t error_len)
{
    if (!instr->result || instr->result->type->kind != ANVIL_TYPE_PTR) {
        return verify_fail(error, error_len,
                           "alloca in function %s must produce a pointer",
                           func_name(func));
    }
    if (instr->num_operands == 0) return true;
    if (instr->num_operands == 1) {
        if (!verify_value_ref(mod, func, instr->operands[0], error, error_len)) {
            return false;
        }
        if (type_is_integer(instr->operands[0]->type)) return true;
    }
    return verify_fail(error, error_len,
                       "dynamic alloca in function %s requires one integer count",
                       func_name(func));
}

static bool verify_instr(const anvil_module_t *mod,
                         const anvil_func_t *func,
                         const anvil_instr_t *instr,
                         char *error,
                         size_t error_len)
{
    if (!instr) {
        return verify_fail(error, error_len,
                           "function %s contains a null instruction",
                           func_name(func));
    }
    if (instr->op < 0 || instr->op >= ANVIL_OP_COUNT) {
        return verify_fail(error, error_len,
                           "function %s contains an invalid opcode",
                           func_name(func));
    }
    if (instr->result) {
        if (instr->result->kind != ANVIL_VAL_INSTR ||
            instr->result->data.instr != instr ||
            !instr->result->type) {
            return verify_fail(error, error_len,
                               "function %s contains malformed instruction result",
                               func_name(func));
        }
    }

    switch (instr->op) {
        case ANVIL_OP_ADD:
        case ANVIL_OP_SUB:
        case ANVIL_OP_MUL:
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
        case ANVIL_OP_SMOD:
        case ANVIL_OP_UMOD:
        case ANVIL_OP_AND:
        case ANVIL_OP_OR:
        case ANVIL_OP_XOR:
        case ANVIL_OP_FADD:
        case ANVIL_OP_FSUB:
        case ANVIL_OP_FMUL:
        case ANVIL_OP_FDIV:
            return verify_binop(mod, func, instr, error, error_len);

        case ANVIL_OP_SHL:
        case ANVIL_OP_SHR:
        case ANVIL_OP_SAR:
            return verify_shift(mod, func, instr, error, error_len);

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
        case ANVIL_OP_FCMP:
            return verify_cmp(mod, func, instr, error, error_len);

        case ANVIL_OP_NEG:
        case ANVIL_OP_NOT:
        case ANVIL_OP_FNEG:
        case ANVIL_OP_FABS:
            return verify_unop(mod, func, instr, error, error_len);

        case ANVIL_OP_TRUNC:
        case ANVIL_OP_ZEXT:
        case ANVIL_OP_SEXT:
        case ANVIL_OP_FPTRUNC:
        case ANVIL_OP_FPEXT:
        case ANVIL_OP_FPTOSI:
        case ANVIL_OP_FPTOUI:
        case ANVIL_OP_SITOFP:
        case ANVIL_OP_UITOFP:
        case ANVIL_OP_PTRTOINT:
        case ANVIL_OP_INTTOPTR:
        case ANVIL_OP_BITCAST:
            return verify_cast(mod, func, instr, error, error_len);

        case ANVIL_OP_LOAD:
        case ANVIL_OP_STORE:
            return verify_memory(mod, func, instr, error, error_len);

        case ANVIL_OP_ALLOCA:
            return verify_alloca(mod, func, instr, error, error_len);

        case ANVIL_OP_GEP:
            return verify_gep(mod, func, instr, error, error_len);

        case ANVIL_OP_STRUCT_GEP:
            return verify_struct_gep(mod, func, instr, error, error_len);

        case ANVIL_OP_CALL:
            return verify_call(mod, func, instr, error, error_len);

        case ANVIL_OP_SELECT:
            return verify_select(mod, func, instr, error, error_len);

        case ANVIL_OP_PHI:
            return verify_phi(mod, func, instr, error, error_len);

        case ANVIL_OP_BR:
        case ANVIL_OP_BR_COND:
            return verify_branch(func, instr, error, error_len);

        case ANVIL_OP_SWITCH:
            return verify_switch(mod, func, instr, error, error_len);

        case ANVIL_OP_RET:
            return verify_ret(func, instr, error, error_len);

        case ANVIL_OP_NOP:
            return instr->num_operands == 0 && !instr->result;

        case ANVIL_OP_DIV:
        case ANVIL_OP_MOD:
            return verify_fail(error, error_len,
                               "function %s uses unsupported source opcode",
                               func_name(func));

        case ANVIL_OP_COUNT:
            break;
    }

    return verify_fail(error, error_len,
                       "function %s uses invalid source opcode",
                       func_name(func));
}

static bool verify_function_shape(const anvil_func_t *func,
                                  char *error,
                                  size_t error_len)
{
    if (!func) {
        return verify_fail(error, error_len, "module contains a null function");
    }
    if (!func->parent) {
        return verify_fail(error, error_len,
                           "function %s has no parent module",
                           func_name(func));
    }
    if (!func->type || func->type->kind != ANVIL_TYPE_FUNC) {
        return verify_fail(error, error_len,
                           "function %s has non-function type",
                           func_name(func));
    }
    if (func->num_params != func->type->data.func.num_params) {
        return verify_fail(error, error_len,
                           "function %s parameter count does not match its type",
                           func_name(func));
    }
    if (func->num_params > 0 && !func->type->data.func.params) {
        return verify_fail(error, error_len,
                           "function %s has a malformed function type",
                           func_name(func));
    }
    if (func->is_declaration) {
        return true;
    }
    for (size_t i = 0; i < func->num_params; i++) {
        anvil_value_t *param = func->params ? func->params[i] : NULL;
        if (!param ||
            param->kind != ANVIL_VAL_PARAM ||
            param->data.param.func != func ||
            param->data.param.index != i ||
            !type_equal(param->type, func->type->data.func.params[i])) {
            return verify_fail(error, error_len,
                               "function %s has malformed parameter %zu",
                               func_name(func), i);
        }
    }
    return true;
}

bool anvil_func_verify(const anvil_func_t *func, char *error, size_t error_len)
{
    if (error && error_len > 0) error[0] = '\0';
    if (!verify_function_shape(func, error, error_len)) return false;

    if (func->is_declaration) {
        if (!func->entry && !func->blocks) return true;
        return verify_fail(error, error_len,
                           "declaration %s must not have a body",
                           func_name(func));
    }

    if (!func->entry || !func->blocks) {
        return verify_fail(error, error_len,
                           "function %s must have an entry block",
                           func_name(func));
    }
    if (!block_belongs_to_func(func, func->entry)) {
        return verify_fail(error, error_len,
                           "function %s entry block is not in the function",
                           func_name(func));
    }

    const anvil_module_t *mod = func->parent;
    for (const anvil_block_t *block = func->blocks; block; block = block->next) {
        if (block->parent != func) {
            return verify_fail(error, error_len,
                               "function %s has a block with wrong parent",
                               func_name(func));
        }
        if (!block->first || !block->last) {
            return verify_fail(error, error_len,
                               "block %s in function %s is missing a terminator",
                               block_name(block), func_name(func));
        }

        bool saw_non_phi = false;
        bool saw_terminator = false;
        for (const anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->parent != block) {
                return verify_fail(error, error_len,
                                   "block %s in function %s has instruction with wrong parent",
                                   block_name(block), func_name(func));
            }
            if (saw_terminator) {
                return verify_fail(error, error_len,
                                   "block %s in function %s has instructions after a terminator",
                                   block_name(block), func_name(func));
            }
            if (instr->op == ANVIL_OP_PHI && saw_non_phi) {
                return verify_fail(error, error_len,
                                   "PHI in function %s must appear before non-PHI instructions",
                                   func_name(func));
            }
            if (instr->op != ANVIL_OP_PHI) saw_non_phi = true;

            if (!verify_instr(mod, func, instr, error, error_len)) return false;

            if (op_is_terminator(instr->op)) saw_terminator = true;
        }

        if (!saw_terminator) {
            return verify_fail(error, error_len,
                               "block %s in function %s is missing a terminator",
                               block_name(block), func_name(func));
        }
    }

    return true;
}

bool anvil_module_verify(const anvil_module_t *mod, char *error, size_t error_len)
{
    if (error && error_len > 0) error[0] = '\0';
    if (!mod) return verify_fail(error, error_len, "module is null");
    if (!mod->ctx) {
        return verify_fail(error, error_len,
                           "module %s has no context",
                           mod->name ? mod->name : "<anon>");
    }

    for (const anvil_global_t *global = mod->globals; global; global = global->next) {
        if (!global->value || !global->value->type ||
            global->value->kind != ANVIL_VAL_GLOBAL) {
            return verify_fail(error, error_len,
                               "module %s contains a malformed global",
                               mod->name ? mod->name : "<anon>");
        }
        if (global->value->data.global.init &&
            !type_equal(global->value->data.global.init->type,
                        global->value->type)) {
            return verify_fail(error, error_len,
                               "module %s has a global initializer type mismatch",
                               mod->name ? mod->name : "<anon>");
        }
    }

    for (const anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (func->parent != mod) {
            return verify_fail(error, error_len,
                               "module %s contains a function with wrong parent",
                               mod->name ? mod->name : "<anon>");
        }
        if (!anvil_func_verify(func, error, error_len)) return false;
    }

    return true;
}
