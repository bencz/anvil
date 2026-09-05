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
           value->kind <= ANVIL_VAL_CONST_GEP;
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
    } else if (value->kind == ANVIL_VAL_CONST_ARRAY ||
               value->kind == ANVIL_VAL_CONST_STRUCT) {
        free(value->data.aggregate.elements);
        value->data.aggregate.elements = NULL;
        value->data.aggregate.num_elements = 0;
    } else if (value->kind == ANVIL_VAL_CONST_GEP) {
        free(value->data.reloc.indices);
        value->data.reloc.indices = NULL;
        value->data.reloc.num_indices = 0;
    }
}

static void value_free(anvil_value_t *value)
{
    if (!value) return;
    value_clear_payload(value);
    free(value->name);
    free(value);
}

/* Constructors publish values to the context registry immediately so every
 * partial object remains destroyable.  A failed constructor still owns the
 * newest registry node exclusively; unlink and destroy it instead of leaving
 * an unreachable malformed/tombstone constant behind. */
static void value_discard_new(anvil_ctx_t *ctx, anvil_value_t *value)
{
    if (!ctx || !value || ctx->owned_values != value) return;
    ctx->owned_values = value->ctx_next_owned;
    value->ctx_next_owned = NULL;
    value_free(value);
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

void anvil_instr_discard_new(anvil_ctx_t *ctx, anvil_instr_t *instr)
{
    if (!ctx || !instr || instr->parent || ctx->owned_instrs != instr) return;
    ctx->owned_instrs = instr->ctx_next_owned;
    instr->ctx_next_owned = NULL;
    if (instr->result) value_discard_new(ctx, instr->result);
    free(instr->operands);
    free(instr->phi_blocks);
    free(instr->switch_blocks);
    free(instr);
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

void anvil_block_append_prepared(anvil_block_t *block, anvil_instr_t *instr)
{
    instr->parent = block;
    instr->owner_module = block->owner_module;
    instr->prev = block->last;
    instr->next = NULL;
    if (block->last)
        block->last->next = instr;
    else
        block->first = instr;

    block->last = instr;
    if (instr->result)
        instr->result->owner_module = block->owner_module;
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

static bool constant_symbol_addr_is_well_typed(const anvil_value_t *value)
{
    const anvil_value_t *symbol = value->data.reloc.symbol;
    if (!symbol || !value->owner_module ||
        symbol->owner_module != value->owner_module ||
        symbol->owner_ctx != value->owner_ctx ||
        !symbol->name ||
        anvil_module_lookup_symbol(value->owner_module, symbol->name) != symbol ||
        value->data.reloc.base || value->data.reloc.indices ||
        value->data.reloc.num_indices != 0 ||
        value->data.reloc.addend != 0 || !value->data.reloc.source_type ||
        value->type->kind != ANVIL_TYPE_PTR) return false;
    if (symbol->kind == ANVIL_VAL_FUNC) {
        return symbol->data.func &&
               symbol->data.func->parent == value->owner_module &&
               symbol->type && symbol->type->kind == ANVIL_TYPE_PTR &&
               symbol->type->data.pointee == value->data.reloc.source_type &&
               anvil_types_equal(value->type, symbol->type);
    }
    return symbol->kind == ANVIL_VAL_GLOBAL &&
           symbol->type == value->data.reloc.source_type &&
           anvil_types_equal(value->type->data.pointee, symbol->type);
}

static bool reloc_addend_fits_target(const anvil_ctx_t *ctx, int64_t addend);

static bool constant_gep_is_well_typed(const anvil_value_t *value)
{
    const anvil_value_t *base = value->data.reloc.base;
    if (!base || !value->owner_module ||
        (base->kind != ANVIL_VAL_CONST_SYMBOL_ADDR &&
         base->kind != ANVIL_VAL_CONST_GEP) ||
        base->owner_module != value->owner_module ||
        base->owner_ctx != value->owner_ctx ||
        base->data.reloc.symbol != value->data.reloc.symbol ||
        !value->data.reloc.source_type ||
        !anvil_sem_type_is_sized(value->data.reloc.source_type) ||
        value->data.reloc.source_type->owner_ctx != value->owner_ctx ||
        !base->type || base->type->kind != ANVIL_TYPE_PTR ||
        !anvil_types_equal(base->type->data.pointee,
                           value->data.reloc.source_type) ||
        !value->type || value->type->kind != ANVIL_TYPE_PTR ||
        value->data.reloc.num_indices == 0 ||
        !value->data.reloc.indices ||
        value->data.reloc.num_indices >
            SIZE_MAX / sizeof(*value->data.reloc.indices)) return false;

    anvil_type_t *current = value->data.reloc.source_type;
    int64_t addend = base->data.reloc.addend;
    for (size_t i = 0; i < value->data.reloc.num_indices; i++) {
        anvil_value_t *index = value->data.reloc.indices[i];
        anvil_gep_step_t step;
        int64_t step_offset;
        if (!index || index->owner_ctx != value->owner_ctx ||
            index->owner_module || index->kind != ANVIL_VAL_CONST_INT ||
            !constant_leaf_is_well_typed(index) ||
            !anvil_gep_analyze_step(&current, index, i, &step) ||
            !anvil_gep_const_step_offset(&step, index, &step_offset) ||
            !anvil_gep_accumulate_offset(&addend, step_offset)) return false;
    }
    return reloc_addend_fits_target(value->owner_ctx, addend) &&
           anvil_types_equal(value->type->data.pointee, current) &&
           addend == value->data.reloc.addend;
}

static anvil_const_dag_status_t check_constant_dag_impl(
    const anvil_value_t *value, anvil_ctx_t *ctx,
    const anvil_module_t *required_module)
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
                (cur->owner_module && required_module &&
                 cur->owner_module != required_module) ||
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

            if (cur->kind >= ANVIL_VAL_CONST_INT &&
                cur->kind <= ANVIL_VAL_CONST_STRING) {
                if (cur->owner_module || !constant_leaf_is_well_typed(cur)) {
                    valid = false;
                    break;
                }
                mark->state = 2;
                frames_count--;
                continue;
            }

            if (cur->kind == ANVIL_VAL_CONST_SYMBOL_ADDR) {
                if (!constant_symbol_addr_is_well_typed(cur)) valid = false;
                mark->state = 2;
                frames_count--;
                continue;
            }

            if (cur->kind == ANVIL_VAL_CONST_GEP) {
                if (!constant_gep_is_well_typed(cur)) {
                    valid = false;
                    break;
                }
                frame->entered = true;
                continue;
            }

            bool is_array = cur->kind == ANVIL_VAL_CONST_ARRAY;
            bool is_struct = cur->kind == ANVIL_VAL_CONST_STRUCT;
            size_t expected_count = is_array
                ? (cur->type->kind == ANVIL_TYPE_ARRAY
                       ? cur->type->data.array.count : SIZE_MAX)
                : (is_struct && cur->type->kind == ANVIL_TYPE_STRUCT &&
                   cur->type->data.struc.complete
                       ? cur->type->data.struc.num_fields : SIZE_MAX);
            if ((!is_array && !is_struct) ||
                cur->data.aggregate.num_elements != expected_count ||
                (cur->data.aggregate.num_elements > 0 &&
                 !cur->data.aggregate.elements) ||
                cur->data.aggregate.num_elements >
                    SIZE_MAX / sizeof(anvil_value_t *)) {
                valid = false;
                break;
            }

            const anvil_module_t *aggregate_module = NULL;
            for (size_t i = 0; i < cur->data.aggregate.num_elements; i++) {
                const anvil_value_t *child = cur->data.aggregate.elements[i];
                if (!child) { valid = false; break; }
                if (child->owner_module) {
                    if (aggregate_module &&
                        aggregate_module != child->owner_module) {
                        valid = false;
                        break;
                    }
                    aggregate_module = child->owner_module;
                }
            }
            if (!valid || cur->owner_module != aggregate_module) {
                valid = false;
                break;
            }
            frame->entered = true;
        }

        size_t child_count = cur->kind == ANVIL_VAL_CONST_GEP
                                 ? 1 : cur->data.aggregate.num_elements;
        if (frame->next_child == child_count) {
            constant_mark_t *mark = constant_mark_find(marks, marks_capacity, cur);
            mark->state = 2;
            frames_count--;
            continue;
        }

        size_t child_index = frame->next_child++;
        const anvil_value_t *child = cur->kind == ANVIL_VAL_CONST_GEP
            ? cur->data.reloc.base
            : cur->data.aggregate.elements[child_index];
        const anvil_type_t *expected_type;
        if (cur->kind == ANVIL_VAL_CONST_GEP) {
            expected_type = child ? child->type : NULL;
        } else if (cur->kind == ANVIL_VAL_CONST_ARRAY) {
            expected_type = cur->type->data.array.elem;
        } else {
            expected_type = cur->type->data.struc.fields[child_index];
        }
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
            child, expected_type, 0, false
        };
    }

    free(frames);
    free(marks);
    return valid ? ANVIL_CONST_DAG_VALID
                 : (oom ? ANVIL_CONST_DAG_NOMEM
                        : ANVIL_CONST_DAG_INVALID);
}

anvil_const_dag_status_t
anvil_value_check_constant_dag(const anvil_value_t *value, anvil_ctx_t *ctx)
{
    return check_constant_dag_impl(value, ctx, NULL);
}

anvil_const_dag_status_t anvil_value_check_constant_dag_for_module(
    const anvil_value_t *value, anvil_ctx_t *ctx,
    const anvil_module_t *module)
{
    if (!module || module->ctx != ctx) return ANVIL_CONST_DAG_INVALID;
    return check_constant_dag_impl(value, ctx, module);
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
            !value_is_constant(elements[i]) ||
            !anvil_types_equal(elements[i]->type, elem_type)) {
            return NULL;
        }
    }

    anvil_module_t *owner_module = NULL;
    for (size_t i = 0; i < num_elements; i++) {
        if (!elements[i]->owner_module) continue;
        if (owner_module && owner_module != elements[i]->owner_module) {
            anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                            "Array constant mixes relocations from different modules");
            return NULL;
        }
        owner_module = elements[i]->owner_module;
    }
    
    anvil_type_t *arr_type = anvil_type_array(ctx, elem_type, num_elements);
    if (!arr_type) return NULL;
    anvil_value_t *v = anvil_value_create(ctx, ANVIL_VAL_CONST_ARRAY, arr_type, NULL);
    if (!v) return NULL;
    
    if (num_elements > 0) {
        v->data.aggregate.elements =
            anvil_ctx_malloc(ctx, num_elements * sizeof(anvil_value_t *));
        if (!v->data.aggregate.elements) {
            anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                            "Out of memory creating array constant");
            value_discard_new(ctx, v);
            return NULL;
        }
        memcpy(v->data.aggregate.elements, elements,
               num_elements * sizeof(anvil_value_t *));
    } else {
        v->data.aggregate.elements = NULL;
    }
    v->data.aggregate.num_elements = num_elements;
    v->owner_module = owner_module;

    anvil_const_dag_status_t dag = anvil_value_check_constant_dag(v, ctx);
    if (dag != ANVIL_CONST_DAG_VALID) {
        if (dag == ANVIL_CONST_DAG_INVALID)
            anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                            "Array constant is not a well-typed acyclic constant DAG");
        value_discard_new(ctx, v);
        return NULL;
    }
    return constant_register(ctx, v);
}

anvil_value_t *anvil_const_struct(anvil_ctx_t *ctx,
                                   anvil_type_t *struct_type,
                                   anvil_value_t **fields,
                                   size_t num_fields)
{
    if (!ctx || !struct_type || struct_type->owner_ctx != ctx ||
        struct_type->kind != ANVIL_TYPE_STRUCT ||
        !struct_type->data.struc.complete ||
        num_fields != struct_type->data.struc.num_fields ||
        (num_fields > 0 && !fields) ||
        num_fields > SIZE_MAX / sizeof(*fields)) {
        if (ctx) anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE,
                                 "Struct constant requires the complete exact struct type");
        return NULL;
    }

    anvil_module_t *owner_module = NULL;
    for (size_t i = 0; i < num_fields; i++) {
        if (!fields[i] || fields[i]->owner_ctx != ctx ||
            !value_is_constant(fields[i]) ||
            !anvil_types_equal(fields[i]->type,
                               struct_type->data.struc.fields[i])) {
            anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE,
                            "Struct constant field type does not match its declaration");
            return NULL;
        }
        if (fields[i]->owner_module) {
            if (owner_module && owner_module != fields[i]->owner_module) {
                anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                                "Struct constant mixes relocations from different modules");
                return NULL;
            }
            owner_module = fields[i]->owner_module;
        }
    }

    anvil_value_t *value = anvil_value_create(
        ctx, ANVIL_VAL_CONST_STRUCT, struct_type, NULL);
    if (!value) return NULL;
    if (num_fields > 0) {
        value->data.aggregate.elements = anvil_ctx_malloc(
            ctx, num_fields * sizeof(*value->data.aggregate.elements));
        if (!value->data.aggregate.elements) {
            anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                            "Out of memory creating struct constant");
            value_discard_new(ctx, value);
            return NULL;
        }
        memcpy(value->data.aggregate.elements, fields,
               num_fields * sizeof(*fields));
    }
    value->data.aggregate.num_elements = num_fields;
    value->owner_module = owner_module;

    anvil_const_dag_status_t dag = anvil_value_check_constant_dag(value, ctx);
    if (dag != ANVIL_CONST_DAG_VALID) {
        if (dag == ANVIL_CONST_DAG_INVALID)
            anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                            "Struct constant is not a well-typed acyclic constant DAG");
        value_discard_new(ctx, value);
        return NULL;
    }
    return constant_register(ctx, value);
}

anvil_value_t *anvil_const_symbol_addr(anvil_value_t *symbol)
{
    if (!symbol || !symbol->owner_ctx || !symbol->owner_module ||
        (symbol->kind != ANVIL_VAL_GLOBAL &&
         symbol->kind != ANVIL_VAL_FUNC)) {
        if (symbol && symbol->owner_ctx)
            anvil_set_error(symbol->owner_ctx, ANVIL_ERR_INVALID_ARG,
                            "Relocatable address requires a live module symbol");
        return NULL;
    }
    anvil_ctx_t *ctx = symbol->owner_ctx;
    anvil_type_t *source_type;
    anvil_type_t *pointer_type;
    if (symbol->kind == ANVIL_VAL_FUNC) {
        if (!symbol->type || symbol->type->kind != ANVIL_TYPE_PTR ||
            !symbol->type->data.pointee ||
            symbol->type->data.pointee->kind != ANVIL_TYPE_FUNC) {
            anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE,
                            "Function symbol has no callable pointer type");
            return NULL;
        }
        pointer_type = symbol->type;
        source_type = symbol->type->data.pointee;
    } else {
        source_type = symbol->type;
        pointer_type = anvil_type_ptr(ctx, source_type);
        if (!pointer_type) return NULL;
    }

    anvil_value_t *value = anvil_value_create(
        ctx, ANVIL_VAL_CONST_SYMBOL_ADDR, pointer_type, NULL);
    if (!value) return NULL;
    value->owner_module = symbol->owner_module;
    value->data.reloc.symbol = symbol;
    value->data.reloc.source_type = source_type;
    anvil_const_dag_status_t dag = anvil_value_check_constant_dag_for_module(
        value, ctx, symbol->owner_module);
    if (dag != ANVIL_CONST_DAG_VALID) {
        if (dag == ANVIL_CONST_DAG_INVALID)
            anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                            "Malformed relocatable symbol address");
        value_discard_new(ctx, value);
        return NULL;
    }
    return constant_register(ctx, value);
}

static bool reloc_addend_fits_target(const anvil_ctx_t *ctx, int64_t addend)
{
    const anvil_arch_info_t *info = ctx ? anvil_arch_get_info(ctx->arch) : NULL;
    unsigned bits = info ? (unsigned)info->addr_bits : 0;
    if (bits == 0 || bits > 64) return false;
    if (bits == 64) return true;
    int64_t limit = INT64_C(1) << (bits - 1);
    return addend >= -limit && addend < limit;
}

anvil_value_t *anvil_const_gep(anvil_value_t *base,
                                anvil_type_t *source_type,
                                anvil_value_t **indices,
                                size_t num_indices)
{
    if (!base) return NULL;
    anvil_ctx_t *ctx = base->owner_ctx;
    if (!ctx || !base->owner_module ||
        (base->kind != ANVIL_VAL_CONST_SYMBOL_ADDR &&
         base->kind != ANVIL_VAL_CONST_GEP)) {
        if (ctx) anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                                 "Constant GEP requires a live relocatable base");
        return NULL;
    }
    if (!source_type || source_type->owner_ctx != ctx ||
        !base->type || base->type->kind != ANVIL_TYPE_PTR ||
        !anvil_types_equal(base->type->data.pointee, source_type) ||
        !anvil_sem_type_is_sized(source_type) ||
        num_indices == 0 || !indices ||
        num_indices > SIZE_MAX / sizeof(*indices)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE,
                        "Constant GEP source type does not match its base pointer");
        return NULL;
    }

    anvil_type_t *current = source_type;
    int64_t addend = base->data.reloc.addend;
    for (size_t i = 0; i < num_indices; i++) {
        anvil_gep_step_t step;
        int64_t step_offset;
        if (!indices[i] || indices[i]->owner_ctx != ctx ||
            indices[i]->owner_module ||
            indices[i]->kind != ANVIL_VAL_CONST_INT ||
            !anvil_gep_analyze_step(&current, indices[i], i, &step) ||
            !anvil_gep_const_step_offset(&step, indices[i], &step_offset) ||
            !anvil_gep_accumulate_offset(&addend, step_offset)) {
            anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                            "Constant GEP requires valid constant integer indices and a representable addend");
            return NULL;
        }
    }
    if (!reloc_addend_fits_target(ctx, addend)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                        "Constant GEP addend does not fit the target address width");
        return NULL;
    }

    anvil_value_t **saved_indices = NULL;
    if (num_indices > 0) {
        saved_indices = anvil_ctx_malloc(ctx,
                                         num_indices * sizeof(*saved_indices));
        if (!saved_indices) return NULL;
        memcpy(saved_indices, indices, num_indices * sizeof(*saved_indices));
    }
    anvil_type_t *result_type = anvil_type_ptr(ctx, current);
    if (!result_type) { free(saved_indices); return NULL; }
    anvil_value_t *value = anvil_value_create(
        ctx, ANVIL_VAL_CONST_GEP, result_type, NULL);
    if (!value) { free(saved_indices); return NULL; }
    value->owner_module = base->owner_module;
    value->data.reloc.symbol = base->data.reloc.symbol;
    value->data.reloc.base = base;
    value->data.reloc.source_type = source_type;
    value->data.reloc.indices = saved_indices;
    value->data.reloc.num_indices = num_indices;
    value->data.reloc.addend = addend;
    anvil_const_dag_status_t dag = anvil_value_check_constant_dag_for_module(
        value, ctx, base->owner_module);
    if (dag != ANVIL_CONST_DAG_VALID) {
        if (dag == ANVIL_CONST_DAG_INVALID)
            anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                            "Malformed typed constant GEP");
        value_discard_new(ctx, value);
        return NULL;
    }
    return constant_register(ctx, value);
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
    anvil_const_dag_status_t dag = anvil_value_check_constant_dag_for_module(
        init, global->owner_ctx, global->owner_module);
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
