/*
 * MCC - Micro C Compiler
 * Semantic Analysis - Initializer Resolution (C11 6.7.9)
 *
 * A brace initializer is flattened into an ordered list of typed leaves
 * stored on the outermost AST_INIT_LIST.  Designators, brace elision, unions
 * and arrays of unknown size are resolved here once, so codegen only walks
 * explicit subobject paths.  Subobjects without an entry are zero, which
 * codegen implements by zeroing the whole object before storing leaves.
 */

#include "sema_internal.h"
#include <string.h>

#define INIT_UNBOUNDED SIZE_MAX

typedef struct {
    mcc_init_entry_t *entries;
    size_t count;
    size_t capacity;
} init_sink_t;

/* One aggregate being filled positionally.  levels[0] is the object of the
 * braced list; deeper levels come from brace elision and designators. */
typedef struct {
    mcc_type_t *type;
    size_t index;               /* next position to initialize */
    size_t count;               /* positions, INIT_UNBOUNDED if unknown */
} init_level_t;

typedef struct {
    mcc_sema_t *sema;
    init_sink_t *sink;
    const mcc_init_step_t *base;    /* path to the braced list's object */
    size_t base_len;
    init_level_t *levels;
    mcc_init_step_t *steps;         /* steps[i]: levels[i] -> levels[i+1] */
    size_t depth;
    size_t capacity;
    size_t max_index;               /* top-level elements used (unbounded) */
} init_state_t;

static void resolve_braced(mcc_sema_t *sema, init_sink_t *sink,
                           mcc_type_t *type, const mcc_init_step_t *base,
                           size_t base_len, mcc_ast_node_t *list,
                           size_t *array_length_out);

static mcc_type_t *unwrap(mcc_type_t *type)
{
    while (type && type->kind == TYPE_TYPEDEF)
        type = type->data.typedef_ref.underlying;
    return type;
}

static bool is_aggregate(mcc_type_t *type)
{
    type = unwrap(type);
    return type && (type->kind == TYPE_ARRAY || type->kind == TYPE_STRUCT ||
                    type->kind == TYPE_UNION);
}

static bool is_char_array(mcc_type_t *type)
{
    type = unwrap(type);
    if (!type || type->kind != TYPE_ARRAY) return false;
    mcc_type_t *element = unwrap(type->data.array.element);
    return element && element->kind == TYPE_CHAR;
}

/* Must match the member numbering of codegen_record_type/codegen_lvalue. */
static bool is_physical_member(const mcc_struct_field_t *field)
{
    return field->name ||
        (field->type && mcc_type_is_record(field->type) &&
         field->bitfield_width == 0);
}

static size_t member_count(mcc_type_t *type)
{
    type = unwrap(type);
    if (type->kind == TYPE_ARRAY) return type->data.array.length;
    size_t count = 0;
    for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next)
        if (is_physical_member(f)) count++;
    return count;
}

/* Returns the type of member `index` of an aggregate and the step to it. */
static mcc_type_t *member_at(mcc_type_t *type, size_t index,
                             mcc_init_step_t *step)
{
    type = unwrap(type);
    if (type->kind == TYPE_ARRAY) {
        step->kind = MCC_INIT_STEP_ELEMENT;
        step->index = index;
        step->type = type->data.array.element;
        return step->type;
    }
    size_t ordinal = 0;
    for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
        if (!is_physical_member(f)) continue;
        if (ordinal == index) {
            step->kind = type->kind == TYPE_UNION
                ? MCC_INIT_STEP_UNION_MEMBER : MCC_INIT_STEP_FIELD;
            step->index = ordinal;
            step->type = f->type;
            return f->type;
        }
        ordinal++;
    }
    return NULL;
}

static bool sink_append(init_state_t *st, const mcc_init_step_t *path,
                        size_t path_len, mcc_type_t *type,
                        mcc_ast_node_t *value)
{
    init_sink_t *sink = st->sink;
    mcc_context_t *ctx = st->sema->ctx;
    if (sink->count == sink->capacity) {
        size_t capacity = sink->capacity ? sink->capacity * 2 : 8;
        mcc_init_entry_t *grown = mcc_realloc_array(
            ctx, sink->entries, sink->capacity, capacity, sizeof(*grown));
        if (!grown) return false;
        sink->entries = grown;
        sink->capacity = capacity;
    }
    mcc_init_step_t *copy = NULL;
    if (path_len) {
        copy = mcc_alloc_array(ctx, path_len, sizeof(*copy));
        if (!copy) return false;
        memcpy(copy, path, path_len * sizeof(*copy));
    }
    sink->entries[sink->count++] = (mcc_init_entry_t) {
        .path = copy, .path_len = path_len, .type = type, .value = value
    };
    return true;
}

static bool same_step(const mcc_init_step_t *a, const mcc_init_step_t *b)
{
    return a->kind == b->kind && a->index == b->index;
}

/* True when an existing entry initializes `path` or something inside it. */
static bool sink_has_entry_within(const init_sink_t *sink,
                                  const mcc_init_step_t *path, size_t len)
{
    for (size_t i = 0; i < sink->count; i++) {
        const mcc_init_entry_t *entry = &sink->entries[i];
        if (entry->path_len < len) continue;
        size_t s = 0;
        while (s < len && same_step(&entry->path[s], &path[s])) s++;
        if (s == len) return true;
    }
    return false;
}

/* Builds base + steps[0..depth-1) + last into a temporary path. */
static mcc_init_step_t *slot_path(init_state_t *st, const mcc_init_step_t *last,
                                  size_t *len_out)
{
    size_t len = st->base_len + (st->depth - 1) + (last ? 1 : 0);
    mcc_init_step_t *path = len
        ? mcc_alloc_array(st->sema->ctx, len, sizeof(*path)) : NULL;
    if (len && !path) return NULL;
    if (st->base_len)
        memcpy(path, st->base, st->base_len * sizeof(*path));
    if (st->depth > 1)
        memcpy(path + st->base_len, st->steps,
               (st->depth - 1) * sizeof(*path));
    if (last) path[len - 1] = *last;
    *len_out = len;
    return path;
}

static bool push_level(init_state_t *st, const mcc_init_step_t *step,
                       mcc_type_t *type)
{
    if (st->depth == st->capacity) {
        size_t capacity = st->capacity * 2;
        init_level_t *levels = mcc_realloc_array(st->sema->ctx, st->levels,
            st->capacity, capacity, sizeof(*levels));
        mcc_init_step_t *steps = levels ? mcc_realloc_array(st->sema->ctx,
            st->steps, st->capacity, capacity, sizeof(*steps)) : NULL;
        if (!levels || !steps) return false;
        st->levels = levels;
        st->steps = steps;
        st->capacity = capacity;
    }
    st->steps[st->depth - 1] = *step;
    st->levels[st->depth++] = (init_level_t) {
        .type = unwrap(type), .index = 0, .count = member_count(type)
    };
    return true;
}

static void advance(init_level_t *level)
{
    /* Only one member of a union is initialized positionally. */
    if (level->type->kind == TYPE_UNION) level->index = level->count;
    else level->index++;
}

static void note_top_level_use(init_state_t *st)
{
    if (st->depth == 1 && st->levels[0].count == INIT_UNBOUNDED &&
        st->levels[0].index + 1 > st->max_index)
        st->max_index = st->levels[0].index + 1;
}

/* Type-checks a scalar leaf and folds integer constant expressions, so
 * static-storage initializers reach codegen as literals. */
static void check_scalar_leaf(init_state_t *st, mcc_type_t *slot_type,
                              mcc_ast_node_t *value, mcc_type_t *value_type)
{
    if (value_type)
        sema_check_assignment_compat(st->sema, slot_type, value_type,
                                     value->location);
    if (value->kind != AST_INT_LIT && value->kind != AST_CHAR_LIT) {
        int64_t folded;
        if (sema_eval_const_expr(st->sema, value, &folded)) {
            value->kind = AST_INT_LIT;
            value->data.int_lit.value = (uint64_t)folded;
            value->data.int_lit.suffix = INT_SUFFIX_NONE;
        }
    }
}

/* Places `value` at the current subobject, descending with brace elision
 * when an aggregate subobject receives a non-brace expression.  Unless
 * `designated`, the current position first skips completed aggregates. */
static bool place_value(init_state_t *st, mcc_ast_node_t *value,
                        bool designated)
{
    mcc_type_t *value_type = NULL;
    if (value->kind != AST_INIT_LIST)
        value_type = sema_analyze_expr(st->sema, value);

    for (;;) {
        if (!designated) {
            while (st->levels[st->depth - 1].index >=
                   st->levels[st->depth - 1].count) {
                if (st->depth == 1) {
                    mcc_warning_at(st->sema->ctx, value->location,
                                   "excess elements in initializer");
                    return true;
                }
                st->depth--;
                advance(&st->levels[st->depth - 1]);
            }
        }
        designated = false;

        init_level_t *top = &st->levels[st->depth - 1];
        mcc_init_step_t step;
        mcc_type_t *slot = member_at(top->type, top->index, &step);
        if (!slot) return false;
        note_top_level_use(st);
        mcc_type_t *unwrapped = unwrap(slot);

        size_t len;
        mcc_init_step_t *path = NULL;
        if (value->kind == AST_INIT_LIST) {
            path = slot_path(st, &step, &len);
            if (len && !path) return false;
            /* A braced list initializes the whole subobject (6.7.9p19):
             * anything an earlier designator stored inside it is reset. */
            if (sink_has_entry_within(st->sink, path, len) &&
                !sink_append(st, path, len, slot, NULL))
                return false;
            resolve_braced(st->sema, st->sink, slot, path, len, value, NULL);
            advance(top);
            return true;
        }

        bool whole = !is_aggregate(unwrapped) ||
            (is_char_array(unwrapped) && value->kind == AST_STRING_LIT) ||
            (mcc_type_is_record(unwrapped) && value_type &&
             mcc_type_is_compatible(unwrap(value_type), unwrapped));
        if (whole) {
            if (!is_aggregate(unwrapped))
                check_scalar_leaf(st, slot, value, value_type);
            path = slot_path(st, &step, &len);
            if ((len && !path) || !sink_append(st, path, len, slot, value))
                return false;
            advance(top);
            return true;
        }

        /* Brace elision: the aggregate's members take this and the
         * following values until it is complete. */
        if (!push_level(st, &step, slot)) return false;
    }
}

/* Moves the current position to the designated subobject. */
static bool apply_designators(init_state_t *st, mcc_ast_node_t *designated)
{
    /* The parser links designators last-to-first. */
    mcc_ast_node_t *chain[64];
    size_t count = 0;
    for (mcc_ast_node_t *d = designated->data.designated_init.designator; d;
         d = d->kind == AST_FIELD_DESIGNATOR ? d->data.field_designator.next
                                             : d->data.index_designator.next) {
        if (count == sizeof(chain) / sizeof(chain[0])) {
            mcc_error_at(st->sema->ctx, designated->location,
                         "designator chain is too long");
            return false;
        }
        chain[count++] = d;
    }

    st->depth = 1;
    for (size_t i = count; i-- > 0;) {
        mcc_ast_node_t *d = chain[i];
        init_level_t *top = &st->levels[st->depth - 1];
        if (d->kind == AST_INDEX_DESIGNATOR) {
            int64_t index;
            if (top->type->kind != TYPE_ARRAY) {
                mcc_error_at(st->sema->ctx, d->location,
                             "array index in non-array initializer");
                return false;
            }
            sema_analyze_expr(st->sema, d->data.index_designator.index);
            if (!sema_eval_const_expr(st->sema,
                                      d->data.index_designator.index,
                                      &index)) {
                mcc_error_at(st->sema->ctx, d->location,
                             "array designator is not an integer constant");
                return false;
            }
            if (index < 0 || (top->count != INIT_UNBOUNDED &&
                              (uint64_t)index >= top->count)) {
                mcc_error_at(st->sema->ctx, d->location,
                             "array designator index %lld exceeds array bounds",
                             (long long)index);
                return false;
            }
            top->index = (size_t)index;
        } else {
            const char *name = d->data.field_designator.name;
            if (top->type->kind != TYPE_STRUCT &&
                top->type->kind != TYPE_UNION) {
                mcc_error_at(st->sema->ctx, d->location,
                             "field designator '%s' in non-record initializer",
                             name);
                return false;
            }
            /* Descend through C11 anonymous members to the named field. */
            for (;;) {
                size_t ordinal = 0;
                mcc_struct_field_t *anonymous = NULL;
                bool found = false;
                for (mcc_struct_field_t *f = top->type->data.record.fields; f;
                     f = f->next) {
                    if (!is_physical_member(f)) continue;
                    if (f->name && strcmp(f->name, name) == 0) {
                        found = true;
                        break;
                    }
                    if (!f->name && mcc_type_find_field(f->type, name)) {
                        anonymous = f;
                        break;
                    }
                    ordinal++;
                }
                if (!found && !anonymous) {
                    mcc_error_at(st->sema->ctx, d->location,
                                 "unknown field '%s' in initializer", name);
                    return false;
                }
                top->index = ordinal;
                if (found) break;
                mcc_init_step_t step;
                member_at(top->type, ordinal, &step);
                if (!push_level(st, &step, anonymous->type)) return false;
                top = &st->levels[st->depth - 1];
            }
        }
        note_top_level_use(st);

        if (i > 0) {
            mcc_init_step_t step;
            mcc_type_t *member = member_at(top->type, top->index, &step);
            if (!member || !is_aggregate(member)) {
                mcc_error_at(st->sema->ctx, d->location,
                             "designator does not name an aggregate subobject");
                return false;
            }
            if (!push_level(st, &step, member)) return false;
        }
    }
    return true;
}

static void resolve_braced(mcc_sema_t *sema, init_sink_t *sink,
                           mcc_type_t *type, const mcc_init_step_t *base,
                           size_t base_len, mcc_ast_node_t *list,
                           size_t *array_length_out)
{
    mcc_type_t *object = unwrap(type);
    size_t num = list->data.init_list.num_exprs;
    mcc_ast_node_t **exprs = list->data.init_list.exprs;
    init_state_t st = {
        .sema = sema, .sink = sink, .base = base, .base_len = base_len
    };

    if (!object) return;

    /* A brace-enclosed scalar, or a brace-enclosed string for a char array. */
    if (!is_aggregate(object) ||
        (is_char_array(object) && num >= 1 &&
         exprs[0]->kind == AST_STRING_LIT)) {
        if (num == 0) return;
        if (num > 1)
            mcc_warning_at(sema->ctx, exprs[1]->location,
                           "excess elements in initializer");
        mcc_ast_node_t *value = exprs[0];
        if (value->kind == AST_DESIGNATED_INIT) {
            mcc_error_at(sema->ctx, value->location,
                         "designator in a scalar initializer");
            return;
        }
        if (value->kind == AST_INIT_LIST) {
            resolve_braced(sema, sink, type, base, base_len, value, NULL);
            return;
        }
        mcc_type_t *value_type = sema_analyze_expr(sema, value);
        if (value->kind == AST_STRING_LIT) {
            if (array_length_out)
                *array_length_out = strlen(value->data.string_lit.value) + 1;
        } else {
            st.depth = 1;
            check_scalar_leaf(&st, type, value, value_type);
        }
        st.depth = 1;
        sink_append(&st, base, base_len, type, value);
        return;
    }

    st.capacity = 4;
    st.levels = mcc_alloc_array(sema->ctx, st.capacity, sizeof(*st.levels));
    st.steps = mcc_alloc_array(sema->ctx, st.capacity, sizeof(*st.steps));
    if (!st.levels || !st.steps) return;
    st.depth = 1;
    st.levels[0] = (init_level_t) {
        .type = object, .index = 0,
        .count = array_length_out && object->kind == TYPE_ARRAY
            ? INIT_UNBOUNDED : member_count(object)
    };

    for (size_t i = 0; i < num; i++) {
        mcc_ast_node_t *expr = exprs[i];
        if (expr->kind == AST_DESIGNATED_INIT) {
            if (!apply_designators(&st, expr) ||
                !place_value(&st, expr->data.designated_init.value, true))
                return;
        } else if (!place_value(&st, expr, false)) {
            return;
        }
    }
    if (array_length_out)
        *array_length_out = st.max_index;
}

mcc_type_t *sema_resolve_initializer(mcc_sema_t *sema, mcc_type_t *type,
                                     mcc_ast_node_t *init)
{
    init_sink_t sink = {0};
    if (!type || !init || init->kind != AST_INIT_LIST) return type;

    /* An array of unknown size takes its length from the initializer.  A
     * fresh array type is built so a shared (typedef) type is not mutated. */
    mcc_type_t *object = unwrap(type);
    bool infer = object && object->kind == TYPE_ARRAY &&
        object->data.array.length == 0 && !object->data.array.is_vla;
    size_t length = 0;
    resolve_braced(sema, &sink, type, NULL, 0, init, infer ? &length : NULL);

    mcc_type_t *result = type;
    if (infer && length > 0) {
        result = mcc_type_array(sema->types, object->data.array.element,
                                length);
        if (!result) result = type;
        /* Whole-object leaves (a braced string) must see the final size. */
        for (size_t i = 0; i < sink.count; i++)
            if (sink.entries[i].path_len == 0) sink.entries[i].type = result;
    }
    init->data.init_list.entries = sink.entries;
    init->data.init_list.num_entries = sink.count;
    init->data.init_list.resolved = true;
    return result;
}
