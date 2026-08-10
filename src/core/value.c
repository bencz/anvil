/*
 * ANVIL - Value and instruction implementation
 */

#include "anvil/anvil_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

anvil_value_t *anvil_value_create(anvil_ctx_t *ctx, anvil_val_kind_t kind,
                                   anvil_type_t *type, const char *name)
{
    if (!ctx || (type && type->owner_ctx != ctx)) return NULL;
    if (!ctx->target_configured) {
        anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                        "Select a target before creating IR values");
        return NULL;
    }
    if (ctx->next_value_id == UINT32_MAX) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_OP,
                        "IR value ID space is exhausted");
        return NULL;
    }

    anvil_value_t *val = anvil_ctx_calloc(ctx, 1, sizeof(anvil_value_t));
    if (!val) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Out of memory creating value");
        return NULL;
    }

    val->kind = kind;
    val->type = type;
    val->owner_ctx = ctx;
    val->name = name ? anvil_ctx_strdup(ctx, name) : NULL;
    if (name && !val->name) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                        "Out of memory copying value name");
        free(val);
        return NULL;
    }
    val->id = ctx->next_value_id++;
    anvil_ctx_freeze_target(ctx);
    val->ctx_next_owned = ctx->owned_values;
    ctx->owned_values = val;

    return val;
}

static bool value_is_constant(const anvil_value_t *value)
{
    if (!value) return false;
    return value->kind >= ANVIL_VAL_CONST_INT &&
           value->kind <= ANVIL_VAL_CONST_ARRAY;
}

static anvil_value_t *constant_register(anvil_ctx_t *ctx, anvil_value_t *value)
{
    if (!ctx || !value || value->owner_ctx != ctx || !value_is_constant(value)) {
        return NULL;
    }
    return value;
}

static void value_clear_payload(anvil_value_t *value)
{
    if (!value) return;
    if (value->kind == ANVIL_VAL_CONST_STRING) {
        free((void *)value->data.str);
        value->data.str = NULL;
    } else if (value->kind == ANVIL_VAL_CONST_DECIMAL) {
        free(value->data.decimal);
        value->data.decimal = NULL;
    } else if (value->kind == ANVIL_VAL_CONST_ARRAY) {
        free(value->data.array.elements);
        value->data.array.elements = NULL;
        value->data.array.num_elements = 0;
    }
}

static void value_free(anvil_value_t *value)
{
    if (!value) return;
    value_clear_payload(value);
    free(value->name);
    free(value);
}

void anvil_value_free_all(anvil_ctx_t *ctx)
{
    if (!ctx) return;
    anvil_value_t *value = ctx->owned_values;
    ctx->owned_values = NULL;
    while (value) {
        anvil_value_t *next = value->ctx_next_owned;
        value_free(value);
        value = next;
    }
}

void anvil_ir_free_all(anvil_ctx_t *ctx)
{
    if (!ctx) return;
    anvil_instr_t *instr = ctx->owned_instrs;
    ctx->owned_instrs = NULL;
    while (instr) {
        anvil_instr_t *next = instr->ctx_next_owned;
        free(instr->operands);
        free(instr->phi_blocks);
        free(instr->switch_blocks);
        free(instr);
        instr = next;
    }

    anvil_block_t *block = ctx->owned_blocks;
    ctx->owned_blocks = NULL;
    while (block) {
        anvil_block_t *next = block->ctx_next_owned;
        free(block->name);
        free(block->preds);
        free(block->succs);
        free(block);
        block = next;
    }
}

anvil_instr_t *anvil_instr_create(anvil_ctx_t *ctx, anvil_op_t op,
                                   anvil_type_t *type, const char *name)
{
    if (!ctx) return NULL;
    
    anvil_instr_t *instr = anvil_ctx_calloc(ctx, 1, sizeof(anvil_instr_t));
    if (!instr) return NULL;
    
    instr->op = op;
    instr->owner_ctx = ctx;
    
    /* Create result value if not void */
    if (type && type->kind != ANVIL_TYPE_VOID) {
        instr->result = anvil_value_create(ctx, ANVIL_VAL_INSTR, type, name);
        if (!instr->result) {
            free(instr);
            return NULL;
        }
        instr->result->data.instr = instr;
    }
    instr->ctx_next_owned = ctx->owned_instrs;
    ctx->owned_instrs = instr;
    return instr;
}

bool anvil_instr_reserve_operands(anvil_instr_t *instr, size_t needed)
{
    if (!instr || !instr->owner_ctx) return false;
    if (needed <= instr->operands_capacity) return true;
    if (needed > SIZE_MAX / sizeof(*instr->operands)) {
        anvil_set_error(instr->owner_ctx, ANVIL_ERR_NOMEM,
                        "Instruction operand count overflow");
        return false;
    }
    size_t capacity = instr->operands_capacity ? instr->operands_capacity : 4;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    anvil_value_t **new_ops = anvil_ctx_realloc(
        instr->owner_ctx, instr->operands, capacity * sizeof(*new_ops));
    if (!new_ops) return false;
    instr->operands = new_ops;
    instr->operands_capacity = capacity;
    return true;
}

bool anvil_instr_add_operands(anvil_instr_t *instr,
                              anvil_value_t *const *values,
                              size_t count)
{
    if (!instr || (count > 0 && !values) ||
        count > SIZE_MAX - instr->num_operands) {
        if (instr && instr->owner_ctx) {
            anvil_set_error(instr->owner_ctx, ANVIL_ERR_INVALID_ARG,
                            "Invalid instruction operands");
        }
        return false;
    }
    size_t needed = instr->num_operands + count;
    if (!anvil_instr_reserve_operands(instr, needed)) return false;
    if (count > 0) {
        memcpy(&instr->operands[instr->num_operands], values,
               count * sizeof(*values));
    }
    instr->num_operands = needed;
    return true;
}

bool anvil_instr_add_operand(anvil_instr_t *instr, anvil_value_t *val)
{
    return anvil_instr_add_operands(instr, &val, 1);
}

bool anvil_instr_insert(anvil_ctx_t *ctx, anvil_instr_t *instr)
{
    if (!ctx || !instr || instr->owner_ctx != ctx || !ctx->insert_block) {
        if (ctx) {
            anvil_set_error(ctx, ANVIL_ERR_INVALID_OP,
                            "IR builder has no valid insertion block");
        }
        return false;
    }
    if (instr->parent) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_OP,
                        "Instruction is already inserted");
        return false;
    }
    
    anvil_block_t *block = ctx->insert_block;
    if (!block->owner_module || block->owner_module->ctx != ctx ||
        !block->parent || block->parent->parent != block->owner_module) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                        "Insertion block belongs to another context");
        return false;
    }
    instr->parent = block;
    instr->owner_module = block->owner_module;

    anvil_instr_t *point = ctx->insert_point;
    if (point && point->parent != block) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_OP,
                        "Insertion cursor is outside the insertion block");
        instr->parent = NULL;
        instr->owner_module = NULL;
        return false;
    }
    /* Optimizations may unlink the old cursor while retaining ownership.
     * Public insertion semantics are append-at-current-block, so recover in
     * O(1) from a tombstoned cursor using the live tail. */
    if (point != block->last) {
        point = block->last;
        ctx->insert_point = point;
    }
    if (!point) {
        instr->next = block->first;
        if (block->first) block->first->prev = instr;
        block->first = instr;
        if (!block->last) block->last = instr;
    } else {
        instr->prev = point;
        instr->next = point->next;
        if (point->next) point->next->prev = instr;
        point->next = instr;
        if (block->last == point) block->last = instr;
    }
    ctx->insert_point = instr;
    if (instr->result)
        instr->result->owner_module = block->parent->parent;
    return true;
}

anvil_module_t *anvil_value_get_module(const anvil_value_t *val)
{
    return val ? val->owner_module : NULL;
}

/* Constants */
anvil_value_t *anvil_const_i1(anvil_ctx_t *ctx, bool val)
{
    if (!ctx) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT,
                                          ctx->type_i1, NULL);
    if (v) v->data.u = val ? 1 : 0;
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_i8(anvil_ctx_t *ctx, int8_t val)
{
    if (!ctx) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_i8, NULL);
    if (v) v->data.i = val;
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_i16(anvil_ctx_t *ctx, int16_t val)
{
    if (!ctx) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_i16, NULL);
    if (v) v->data.i = val;
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_i32(anvil_ctx_t *ctx, int32_t val)
{
    if (!ctx) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_i32, NULL);
    if (v) v->data.i = val;
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_i64(anvil_ctx_t *ctx, int64_t val)
{
    if (!ctx) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_i64, NULL);
    if (v) v->data.i = val;
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_u8(anvil_ctx_t *ctx, uint8_t val)
{
    if (!ctx) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_u8, NULL);
    if (v) v->data.u = val;
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_u16(anvil_ctx_t *ctx, uint16_t val)
{
    if (!ctx) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_u16, NULL);
    if (v) v->data.u = val;
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_u32(anvil_ctx_t *ctx, uint32_t val)
{
    if (!ctx) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_u32, NULL);
    if (v) v->data.u = val;
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_u64(anvil_ctx_t *ctx, uint64_t val)
{
    if (!ctx) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_INT, ctx->type_u64, NULL);
    if (v) v->data.u = val;
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_f32(anvil_ctx_t *ctx, float val)
{
    if (!ctx) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_FLOAT, ctx->type_f32, NULL);
    if (v) v->data.f = val;
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_f64(anvil_ctx_t *ctx, double val)
{
    if (!ctx) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_FLOAT, ctx->type_f64, NULL);
    if (v) v->data.f = val;
    return constant_register(ctx, v);
}

static bool decimal_literal_is_valid(const char *digits, unsigned precision,
                                     unsigned scale)
{
    if (!digits || !*digits) return false;

    const char *p = digits;
    if (*p == '+' || *p == '-') p++;

    size_t digit_count = 0;
    size_t integer_digits = 0;
    size_t fractional_digits = 0;
    bool saw_digit = false;
    bool saw_dot = false;
    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            saw_digit = true;
            digit_count++;
            if (saw_dot) {
                fractional_digits++;
            } else {
                integer_digits++;
            }
            if (digit_count > precision) return false;
            continue;
        }
        if (*p == '.' && !saw_dot) {
            saw_dot = true;
            continue;
        }
        return false;
    }

    return saw_digit && fractional_digits <= scale &&
           integer_digits <= precision - scale;
}

typedef struct {
    const anvil_value_t *value;
    unsigned char state; /* 1 = visiting, 2 = complete */
} constant_mark_t;

typedef struct {
    const anvil_value_t *value;
    const anvil_type_t *expected_type;
    size_t next_child;
    bool entered;
} constant_frame_t;

static size_t constant_hash(const anvil_value_t *value)
{
    uintptr_t bits = (uintptr_t)value;
    bits ^= bits >> 17;
    bits *= (uintptr_t)0xed5ad4bbU;
    bits ^= bits >> 11;
    return (size_t)bits;
}

static constant_mark_t *constant_mark_find(constant_mark_t *table,
                                           size_t capacity,
                                           const anvil_value_t *value)
{
    size_t index = constant_hash(value) & (capacity - 1);
    while (table[index].value && table[index].value != value) {
        index = (index + 1) & (capacity - 1);
    }
    return &table[index];
}

static bool constant_marks_grow(anvil_ctx_t *ctx,
                                constant_mark_t **table,
                                size_t *capacity)
{
    if (*capacity > SIZE_MAX / 2 ||
        *capacity * 2 > SIZE_MAX / sizeof(constant_mark_t)) {
        return false;
    }
    size_t new_capacity = *capacity * 2;
    constant_mark_t *new_table =
        anvil_ctx_calloc(ctx, new_capacity, sizeof(*new_table));
    if (!new_table) return false;

    for (size_t i = 0; i < *capacity; i++) {
        if ((*table)[i].value) {
            constant_mark_t *slot = constant_mark_find(new_table, new_capacity,
                                                        (*table)[i].value);
            *slot = (*table)[i];
        }
    }
    free(*table);
    *table = new_table;
    *capacity = new_capacity;
    return true;
}

static bool constant_leaf_is_well_typed(const anvil_value_t *value)
{
    switch (value->kind) {
        case ANVIL_VAL_CONST_INT:
            return anvil_type_is_integer(value->type) &&
                   (value->type->kind != ANVIL_TYPE_I1 ||
                    value->data.u <= 1);
        case ANVIL_VAL_CONST_FLOAT:
            return anvil_type_is_floating(value->type);
        case ANVIL_VAL_CONST_DECIMAL:
            return value->type->kind == ANVIL_TYPE_DECIMAL &&
                   decimal_literal_is_valid(
                       value->data.decimal,
                       value->type->data.decimal.precision,
                       value->type->data.decimal.scale);
        case ANVIL_VAL_CONST_NULL:
            return value->type->kind == ANVIL_TYPE_PTR;
        case ANVIL_VAL_CONST_STRING:
            return value->data.str && value->type->kind == ANVIL_TYPE_PTR &&
                   value->type->data.pointee &&
                   value->type->data.pointee->kind == ANVIL_TYPE_I8;
        default:
            return false;
    }
}

anvil_const_dag_status_t
anvil_value_check_constant_dag(const anvil_value_t *value, anvil_ctx_t *ctx)
{
    if (!value || !ctx) return ANVIL_CONST_DAG_INVALID;

    size_t marks_capacity = 16;
    size_t marks_count = 0;
    constant_mark_t *marks =
        anvil_ctx_calloc(ctx, marks_capacity, sizeof(*marks));
    constant_frame_t *frames = anvil_ctx_malloc(ctx, 16 * sizeof(*frames));
    size_t frames_capacity = frames ? 16 : 0;
    size_t frames_count = 0;
    bool valid = marks && frames;
    bool oom = !valid;

    if (valid) {
        frames[frames_count++] = (constant_frame_t){ value, value->type, 0, false };
    }

    while (valid && frames_count > 0) {
        constant_frame_t *frame = &frames[frames_count - 1];
        const anvil_value_t *cur = frame->value;

        if (!frame->entered) {
            if (!cur || !value_is_constant(cur) || !cur->type ||
                cur->owner_ctx != ctx || cur->type->owner_ctx != ctx ||
                !anvil_types_equal(cur->type, frame->expected_type)) {
                valid = false;
                break;
            }

            constant_mark_t *mark = constant_mark_find(marks, marks_capacity, cur);
            if (mark->value) {
                if (mark->state == 1) valid = false;
                frames_count--;
                continue;
            }
            if ((marks_count + 1) * 2 >= marks_capacity) {
                if (!constant_marks_grow(ctx, &marks, &marks_capacity)) {
                    valid = false;
                    oom = true;
                    break;
                }
                mark = constant_mark_find(marks, marks_capacity, cur);
            }
            mark->value = cur;
            mark->state = 1;
            marks_count++;

            if (cur->kind != ANVIL_VAL_CONST_ARRAY) {
                if (!constant_leaf_is_well_typed(cur)) {
                    valid = false;
                    break;
                }
                mark->state = 2;
                frames_count--;
                continue;
            }

            if (cur->type->kind != ANVIL_TYPE_ARRAY ||
                cur->data.array.num_elements != cur->type->data.array.count ||
                (cur->data.array.num_elements > 0 &&
                 !cur->data.array.elements) ||
                cur->data.array.num_elements >
                    SIZE_MAX / sizeof(anvil_value_t *)) {
                valid = false;
                break;
            }
            frame->entered = true;
        }

        if (frame->next_child == cur->data.array.num_elements) {
            constant_mark_t *mark = constant_mark_find(marks, marks_capacity, cur);
            mark->state = 2;
            frames_count--;
            continue;
        }

        const anvil_value_t *child =
            cur->data.array.elements[frame->next_child++];
        if (frames_count == frames_capacity) {
            if (frames_capacity > SIZE_MAX / 2 ||
                frames_capacity * 2 > SIZE_MAX / sizeof(*frames)) {
                valid = false;
                break;
            }
            size_t new_capacity = frames_capacity * 2;
            constant_frame_t *grown =
                anvil_ctx_realloc(ctx, frames,
                                  new_capacity * sizeof(*frames));
            if (!grown) {
                valid = false;
                oom = true;
                break;
            }
            frames = grown;
            frames_capacity = new_capacity;
        }
        frames[frames_count++] = (constant_frame_t){
            child, cur->type->data.array.elem, 0, false
        };
    }

    free(frames);
    free(marks);
    return valid ? ANVIL_CONST_DAG_VALID
                 : (oom ? ANVIL_CONST_DAG_NOMEM
                        : ANVIL_CONST_DAG_INVALID);
}

bool anvil_value_is_constant_dag(const anvil_value_t *value,
                                 anvil_ctx_t *ctx)
{
    return anvil_value_check_constant_dag(value, ctx) ==
           ANVIL_CONST_DAG_VALID;
}

anvil_value_t *anvil_const_decimal(anvil_ctx_t *ctx, anvil_type_t *type,
                                    const char *digits)
{
    if (!ctx) return NULL;
    if (!type || type->owner_ctx != ctx || type->kind != ANVIL_TYPE_DECIMAL) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE,
                        "Decimal constant requires a decimal type from its context");
        return NULL;
    }
    if (!decimal_literal_is_valid(digits, type->data.decimal.precision,
                                  type->data.decimal.scale)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                        "Invalid decimal literal for the requested precision and scale");
        return NULL;
    }

    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_DECIMAL,
                                          type, NULL);
    if (!v) return NULL;
    v->data.decimal = anvil_ctx_strdup(ctx, digits);
    if (!v->data.decimal) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                        "Out of memory copying decimal constant");
        value_clear_payload(v);
        return NULL;
    }
    return constant_register(ctx, v);
}

const char *anvil_const_decimal_digits(anvil_value_t *value)
{
    if (!value || value->kind != ANVIL_VAL_CONST_DECIMAL) return NULL;
    return value->data.decimal;
}

anvil_value_t *anvil_const_null(anvil_ctx_t *ctx, anvil_type_t *ptr_type)
{
    if (!ctx || !ptr_type || ptr_type->owner_ctx != ctx ||
        ptr_type->kind != ANVIL_TYPE_PTR) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_NULL, ptr_type, NULL);
    if (v) v->data.u = 0;
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_string(anvil_ctx_t *ctx, const char *str)
{
    if (!ctx || !str) return NULL;
    anvil_type_t *type = anvil_type_ptr(ctx, ctx->type_i8);
    if (!type) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_STRING, type, NULL);
    if (!v) return NULL;
    v->data.str = anvil_ctx_strdup(ctx, str);
    if (!v->data.str) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                        "Out of memory copying string constant");
        value_clear_payload(v);
        return NULL;
    }
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_array(anvil_ctx_t *ctx, anvil_type_t *elem_type,
                                  anvil_value_t **elements, size_t num_elements)
{
    if (!ctx || !elem_type || elem_type->owner_ctx != ctx ||
        (num_elements > 0 && !elements) ||
        num_elements > SIZE_MAX / sizeof(anvil_value_t *)) {
        return NULL;
    }
    for (size_t i = 0; i < num_elements; i++) {
        if (!elements[i] || elements[i]->owner_ctx != ctx ||
            !anvil_types_equal(elements[i]->type, elem_type)) {
            return NULL;
        }
    }
    
    anvil_type_t *arr_type = anvil_type_array(ctx, elem_type, num_elements);
    if (!arr_type) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_ARRAY, arr_type, NULL);
    if (!v) return NULL;
    
    if (num_elements > 0) {
        v->data.array.elements =
            anvil_ctx_malloc(ctx, num_elements * sizeof(anvil_value_t *));
        if (!v->data.array.elements) {
            anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                            "Out of memory creating array constant");
            value_clear_payload(v);
            return NULL;
        }
        memcpy(v->data.array.elements, elements, num_elements * sizeof(anvil_value_t *));
    } else {
        v->data.array.elements = NULL;
    }
    v->data.array.num_elements = num_elements;

    anvil_const_dag_status_t dag = anvil_value_check_constant_dag(v, ctx);
    if (dag != ANVIL_CONST_DAG_VALID) {
        value_clear_payload(v);
        if (dag == ANVIL_CONST_DAG_INVALID)
            anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                            "Array constant is not a well-typed acyclic constant DAG");
        return NULL;
    }
    return constant_register(ctx, v);
}

bool anvil_global_set_initializer(anvil_value_t *global, anvil_value_t *init)
{
    if (!global) return false;
    if (global->kind != ANVIL_VAL_GLOBAL || !global->owner_ctx ||
        !global->owner_module) {
        if (global->owner_ctx)
            anvil_set_error(global->owner_ctx, ANVIL_ERR_INVALID_ARG,
                            "Initializer target is not a live global symbol");
        return false;
    }
    if (!init) {
        global->data.global.init = NULL;
        return true;
    }
    if (global->data.global.is_declaration) {
        if (global->owner_ctx)
            anvil_set_error(global->owner_ctx, ANVIL_ERR_INVALID_OP,
                            "A global declaration cannot have an initializer; define it first");
        return false;
    }
    if (!global->owner_ctx || init->owner_ctx != global->owner_ctx ||
        !anvil_types_equal(global->type, init->type)) {
        if (global->owner_ctx) {
            anvil_set_error(global->owner_ctx, ANVIL_ERR_INVALID_ARG,
                            "Global initializer must be a same-context constant DAG of the global type");
        }
        return false;
    }
    anvil_const_dag_status_t dag =
        anvil_value_check_constant_dag(init, global->owner_ctx);
    if (dag != ANVIL_CONST_DAG_VALID) {
        if (dag == ANVIL_CONST_DAG_INVALID)
            anvil_set_error(global->owner_ctx, ANVIL_ERR_INVALID_ARG,
                            "Global initializer must be a well-typed acyclic constant DAG");
        return false;
    }
    global->data.global.init = init;
    return true;
}

anvil_type_t *anvil_value_get_type(anvil_value_t *val)
{
    return val ? val->type : NULL;
}

bool anvil_value_is_bool(anvil_value_t *val)
{
    return val && anvil_type_is_bool(val->type);
}

bool anvil_value_is_const_int(anvil_value_t *val)
{
    return val && val->kind == ANVIL_VAL_CONST_INT;
}

bool anvil_value_is_const_float(anvil_value_t *val)
{
    return val && val->kind == ANVIL_VAL_CONST_FLOAT;
}

int64_t anvil_const_int_signed_value(anvil_value_t *val)
{
    if (!anvil_value_is_const_int(val)) return 0;
    return val->type && !val->type->is_signed ? (int64_t)val->data.u
                                              : val->data.i;
}

uint64_t anvil_const_int_unsigned_value(anvil_value_t *val)
{
    if (!anvil_value_is_const_int(val)) return 0;
    return val->type && !val->type->is_signed ? val->data.u
                                              : (uint64_t)val->data.i;
}

double anvil_const_float_value(anvil_value_t *val)
{
    if (!anvil_value_is_const_float(val)) return 0.0;
    return val->data.f;
}
