/*
 * MCC - Micro C Compiler
 * Code Generator - Object Initialization
 *
 * Sema flattens brace initializers into typed leaves (sema_init.c).  Objects
 * with automatic storage are zeroed and then receive each leaf through an
 * explicit subobject path; objects with static storage get one constant
 * built from the same leaves, with every unlisted subobject zero.
 */

#include "codegen_internal.h"
#include <string.h>

static mcc_type_t *unwrap(mcc_type_t *type)
{
    while (type && type->kind == TYPE_TYPEDEF)
        type = type->data.typedef_ref.underlying;
    return type;
}

static bool is_char_array(mcc_type_t *type)
{
    type = unwrap(type);
    if (!type || type->kind != TYPE_ARRAY) return false;
    mcc_type_t *element = unwrap(type->data.array.element);
    return element && element->kind == TYPE_CHAR;
}

/* Must match the member numbering of codegen_record_type. */
static bool is_physical_member(const mcc_struct_field_t *field)
{
    return field->name ||
        (field->type && mcc_type_is_record(field->type) &&
         field->bitfield_width == 0);
}

/* ============================================================
 * Automatic storage
 * ============================================================ */

static anvil_value_t *subobject_pointer(mcc_codegen_t *cg, mcc_type_t *type,
                                        anvil_value_t *pointer,
                                        const mcc_init_step_t *path,
                                        size_t path_len)
{
    for (size_t i = 0; pointer && i < path_len; i++) {
        mcc_type_t *current = unwrap(type);
        const mcc_init_step_t *step = &path[i];
        switch (step->kind) {
            case MCC_INIT_STEP_ELEMENT: {
                anvil_value_t *indices[] = {
                    anvil_const_i64(cg->anvil_ctx, 0),
                    anvil_const_i64(cg->anvil_ctx, (int64_t)step->index)
                };
                pointer = anvil_build_gep(cg->anvil_ctx,
                                          codegen_type(cg, current), pointer,
                                          indices, 2, "init.elem");
                break;
            }
            case MCC_INIT_STEP_FIELD:
                pointer = anvil_build_struct_gep(cg->anvil_ctx,
                                                 codegen_type(cg, current),
                                                 pointer, (unsigned)step->index,
                                                 "init.field");
                break;
            case MCC_INIT_STEP_UNION_MEMBER:
                pointer = anvil_build_struct_gep(cg->anvil_ctx,
                                                 codegen_type(cg, current),
                                                 pointer, (unsigned)step->index,
                                                 "init.member");
                break;
        }
        type = step->type;
    }
    return pointer;
}

/* Stores one leaf. With `zeroed`, bytes not written stay as already zero;
 * otherwise a string initializer zeroes its array first. */
static bool store_leaf(mcc_codegen_t *cg, mcc_type_t *type,
                       anvil_value_t *destination, mcc_ast_node_t *value,
                       bool zeroed)
{
    mcc_type_t *object = unwrap(type);
    if (!destination || !object) return false;

    if (is_char_array(object) && value->kind == AST_STRING_LIT) {
        const char *bytes = value->data.string_lit.value;
        size_t length = strlen(bytes);
        size_t capacity = object->data.array.length;
        if (!zeroed && !codegen_zero_object(cg, object, destination))
            return false;
        anvil_type_t *array = codegen_type(cg, object);
        anvil_type_t *element = codegen_type(cg, object->data.array.element);
        /* The terminating NUL is already stored by the zero fill. */
        for (size_t i = 0; i < length && i < capacity; i++) {
            anvil_value_t *indices[] = {
                anvil_const_i64(cg->anvil_ctx, 0),
                anvil_const_i64(cg->anvil_ctx, (int64_t)i)
            };
            anvil_value_t *slot = anvil_build_gep(cg->anvil_ctx, array,
                                                  destination, indices, 2,
                                                  "init.char");
            anvil_value_t *byte = codegen_const_int_for_type(
                cg, element, (int64_t)(unsigned char)bytes[i]);
            if (!slot || !byte ||
                !anvil_build_store(cg->anvil_ctx, byte, slot))
                return false;
        }
        return true;
    }

    anvil_value_t *result = codegen_expr(cg, value);
    if (!codegen_type_is_record(object))
        result = codegen_convert_value(cg, result, value->type, type,
                                       "init.cast");
    if (!result) {
        mcc_error_at(cg->mcc_ctx, value->location,
                     "cannot generate code for this initializer");
        return false;
    }
    return codegen_store_object(cg, type, result, destination);
}

bool codegen_init_object(mcc_codegen_t *cg, mcc_type_t *type,
                         anvil_value_t *destination, mcc_ast_node_t *init)
{
    if (!init) return true;
    if (init->kind != AST_INIT_LIST)
        return store_leaf(cg, type, destination, init, false);

    if (!init->data.init_list.resolved) {
        mcc_error_at(cg->mcc_ctx, init->location,
                     "initializer was not resolved by semantic analysis");
        return false;
    }
    if (!codegen_zero_object(cg, type, destination))
        return false;
    for (size_t i = 0; i < init->data.init_list.num_entries; i++) {
        const mcc_init_entry_t *entry = &init->data.init_list.entries[i];
        anvil_value_t *slot = subobject_pointer(cg, type, destination,
                                                entry->path, entry->path_len);
        if (!entry->value) {
            /* A later braced list re-initializes this whole subobject. */
            if (!codegen_zero_object(cg, entry->type, slot))
                return false;
            continue;
        }
        if (!store_leaf(cg, entry->type, slot, entry->value, true))
            return false;
    }
    return true;
}

/* ============================================================
 * Static storage
 * ============================================================ */

static anvil_value_t *zero_constant(mcc_codegen_t *cg, mcc_type_t *type);

static anvil_value_t *zero_of_anvil_type(mcc_codegen_t *cg, anvil_type_t *type)
{
    if (anvil_type_is_integer(type))
        return codegen_const_int_for_type(cg, type, 0);
    if (anvil_type_is_floating(type))
        return anvil_type_size(type) == 4
            ? anvil_const_f32(cg->anvil_ctx, 0.0f)
            : anvil_const_f64(cg->anvil_ctx, 0.0);
    if (anvil_type_is_pointer(type))
        return anvil_const_null(cg->anvil_ctx, type);
    return NULL;
}

/* A constant tree under construction. `value` is set for a leaf (with the
 * expression it came from); otherwise children are created on demand and
 * missing ones are zero. */
typedef struct const_node {
    anvil_value_t *value;
    mcc_ast_node_t *expr;
    struct const_node *children;
    size_t count;
    size_t union_member;        /* chosen member for unions */
    bool has_union_member;
} const_node_t;

static size_t physical_member_count(mcc_type_t *type)
{
    size_t count = 0;
    for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next)
        if (is_physical_member(f)) count++;
    return count;
}

static mcc_struct_field_t *physical_member(mcc_type_t *type, size_t index)
{
    size_t ordinal = 0;
    for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
        if (!is_physical_member(f)) continue;
        if (ordinal++ == index) return f;
    }
    return NULL;
}

static mcc_type_t *physical_member_type(mcc_type_t *type, size_t index)
{
    mcc_struct_field_t *field = physical_member(type, index);
    return field ? field->type : NULL;
}

static anvil_value_t *materialize(mcc_codegen_t *cg, mcc_type_t *type,
                                  const const_node_t *node);

/* A union constant holds its initialized member (the first one when the
 * whole union is zero); the remaining bytes are zero. */
static anvil_value_t *union_constant(mcc_codegen_t *cg, mcc_type_t *type,
                                     const const_node_t *node)
{
    mcc_type_t *object = unwrap(type);
    size_t member = node && node->children ? node->union_member : 0;
    mcc_type_t *member_type = physical_member_type(object, member);
    anvil_value_t *value = member_type
        ? (node && node->children ? materialize(cg, member_type, &node->children[0])
                                  : zero_constant(cg, member_type))
        : NULL;
    return value ? anvil_const_union(cg->anvil_ctx, codegen_type(cg, object),
                                     member, value)
                 : NULL;
}

static anvil_value_t *materialize(mcc_codegen_t *cg, mcc_type_t *type,
                                  const const_node_t *node)
{
    if (!node || (!node->value && !node->children))
        return zero_constant(cg, type);
    if (node->value) return node->value;

    mcc_type_t *object = unwrap(type);
    if (object->kind == TYPE_UNION)
        return union_constant(cg, object, node);

    anvil_value_t **values = node->count
        ? mcc_alloc_array(cg->mcc_ctx, node->count, sizeof(*values)) : NULL;
    if (node->count && !values) return NULL;
    for (size_t i = 0; i < node->count; i++) {
        mcc_type_t *member = object->kind == TYPE_ARRAY
            ? object->data.array.element
            : physical_member_type(object, i);
        values[i] = materialize(cg, member, &node->children[i]);
        if (!values[i]) return NULL;
    }
    if (object->kind == TYPE_ARRAY)
        return anvil_const_array(cg->anvil_ctx,
                                 codegen_type(cg, object->data.array.element),
                                 values, node->count);
    return anvil_const_struct(cg->anvil_ctx, codegen_type(cg, object),
                              values, node->count);
}

static anvil_value_t *zero_constant(mcc_codegen_t *cg, mcc_type_t *type)
{
    mcc_type_t *object = unwrap(type);
    if (!object) return NULL;
    if (object->kind == TYPE_UNION)
        return union_constant(cg, object, NULL);
    if (object->kind == TYPE_ARRAY || object->kind == TYPE_STRUCT) {
        const_node_t node = {0};
        node.count = object->kind == TYPE_ARRAY ? object->data.array.length
                                                : physical_member_count(object);
        node.children = node.count
            ? mcc_alloc_array(cg->mcc_ctx, node.count, sizeof(*node.children))
            : NULL;
        if (node.count && !node.children) return NULL;
        if (!node.count) {
            return object->kind == TYPE_ARRAY
                ? anvil_const_array(cg->anvil_ctx,
                      codegen_type(cg, object->data.array.element), NULL, 0)
                : anvil_const_struct(cg->anvil_ctx, codegen_type(cg, object),
                                     NULL, 0);
        }
        return materialize(cg, object, &node);
    }
    return zero_of_anvil_type(cg, codegen_type(cg, object));
}

/* ------------------------------------------------------------
 * Address constants (C11 6.6p9): &object, functions, decayed arrays,
 * member and element designators, pointer +/- integer constant and casts,
 * all relative to an object or function with static storage.
 * ------------------------------------------------------------ */

static bool integer_constant(mcc_ast_node_t *e, int64_t *out)
{
    int64_t a, b;
    if (!e) return false;
    switch (e->kind) {
        case AST_INT_LIT: *out = (int64_t)e->data.int_lit.value; return true;
        case AST_CHAR_LIT: *out = (int64_t)e->data.char_lit.value; return true;
        case AST_UNARY_EXPR:
            if (!integer_constant(e->data.unary_expr.operand, &a)) return false;
            switch (e->data.unary_expr.op) {
                case UNOP_POS: *out = a; return true;
                case UNOP_NEG: *out = (int64_t)(0 - (uint64_t)a); return true;
                case UNOP_BIT_NOT: *out = ~a; return true;
                default: return false;
            }
        case AST_BINARY_EXPR:
            if (!integer_constant(e->data.binary_expr.lhs, &a) ||
                !integer_constant(e->data.binary_expr.rhs, &b)) return false;
            switch (e->data.binary_expr.op) {
                case BINOP_ADD: *out = (int64_t)((uint64_t)a + (uint64_t)b); return true;
                case BINOP_SUB: *out = (int64_t)((uint64_t)a - (uint64_t)b); return true;
                case BINOP_MUL: *out = (int64_t)((uint64_t)a * (uint64_t)b); return true;
                case BINOP_DIV: if (!b) return false; *out = a / b; return true;
                case BINOP_MOD: if (!b) return false; *out = a % b; return true;
                default: return false;
            }
        case AST_CAST_EXPR:
            return mcc_type_is_integer(unwrap(e->data.cast_expr.target_type)) &&
                   integer_constant(e->data.cast_expr.expr, out);
        case AST_SIZEOF_EXPR: {
            mcc_type_t *arg = e->data.sizeof_expr.type_arg;
            if (!arg && e->data.sizeof_expr.expr_arg)
                arg = e->data.sizeof_expr.expr_arg->type;
            if (!arg) return false;
            *out = (int64_t)mcc_type_sizeof(arg);
            return true;
        }
        default:
            return false;
    }
}

static anvil_value_t *address_value(mcc_codegen_t *cg, mcc_ast_node_t *e);

/* Arithmetic constant expression of floating type (C11 6.6p8). */
static bool floating_constant(mcc_ast_node_t *e, double *out)
{
    double a, b;
    int64_t i;
    if (!e) return false;
    switch (e->kind) {
        case AST_FLOAT_LIT: *out = e->data.float_lit.value; return true;
        case AST_UNARY_EXPR:
            if (!floating_constant(e->data.unary_expr.operand, &a)) return false;
            if (e->data.unary_expr.op == UNOP_NEG) { *out = -a; return true; }
            if (e->data.unary_expr.op == UNOP_POS) { *out = a; return true; }
            return false;
        case AST_BINARY_EXPR:
            if (!floating_constant(e->data.binary_expr.lhs, &a) ||
                !floating_constant(e->data.binary_expr.rhs, &b)) return false;
            switch (e->data.binary_expr.op) {
                case BINOP_ADD: *out = a + b; return true;
                case BINOP_SUB: *out = a - b; return true;
                case BINOP_MUL: *out = a * b; return true;
                case BINOP_DIV: *out = a / b; return true;
                default: return false;
            }
        case AST_CAST_EXPR:
            if (!floating_constant(e->data.cast_expr.expr, &a)) return false;
            if (mcc_type_is_floating(unwrap(e->data.cast_expr.target_type))) {
                *out = unwrap(e->data.cast_expr.target_type)->size == 4 ? (float)a : a;
                return true;
            }
            return false;
        default:
            if (!integer_constant(e, &i)) return false;
            *out = e->type && unwrap(e->type)->is_unsigned
                ? (double)(uint64_t)i : (double)i;
            return true;
    }
}

static anvil_value_t *leaf_constant(mcc_codegen_t *cg, mcc_type_t *type,
                                    mcc_ast_node_t *init);

/* A string literal as a relocatable object: a private char array, so an
 * address such as "text" + 2 can carry an offset.  Returns the address of
 * its first character. */
static anvil_value_t *string_object(mcc_codegen_t *cg, mcc_ast_node_t *e)
{
    const char *text = e->data.string_lit.value;
    size_t count = strlen(text) + 1;
    mcc_type_t *array = mcc_type_array(cg->types, mcc_type_char(cg->types), count);
    anvil_type_t *lowered = array ? codegen_type(cg, array) : NULL;
    char name[48];
    snprintf(name, sizeof(name), "mcc.str.%d", cg->label_counter++);
    anvil_value_t *storage = lowered
        ? anvil_module_add_global(cg->anvil_mod, name, lowered, ANVIL_LINK_INTERNAL)
        : NULL;
    anvil_value_t *bytes = storage ? leaf_constant(cg, array, e) : NULL;
    if (!bytes || !anvil_global_set_initializer(storage, bytes)) return NULL;
    anvil_value_t *indices[2] = {
        anvil_const_i64(cg->anvil_ctx, 0), anvil_const_i64(cg->anvil_ctx, 0)
    };
    anvil_value_t *object = anvil_const_symbol_addr(storage);
    return object ? anvil_const_gep(object, lowered, indices, 2) : NULL;
}

/* Relocatable address of the object designated by lvalue `e`, typed as a
 * pointer to the object's lowered type. */
static anvil_value_t *object_address(mcc_codegen_t *cg, mcc_ast_node_t *e)
{
    if (!e) return NULL;
    switch (e->kind) {
        case AST_IDENT_EXPR: {
            mcc_symbol_t *sym = e->data.ident_expr.symbol;
            const char *name = e->data.ident_expr.name;
            /* A static local is a module global known by its source name;
             * an automatic object has no constant address. */
            anvil_value_t *local = cg->current_func ? codegen_find_local(cg, name) : NULL;
            anvil_value_t *global = NULL;
            if (local) {
                if (anvil_value_is_global(local)) global = local;
            } else if (sym && sym->kind == SYM_VAR) {
                global = codegen_get_or_add_global(cg, codegen_link_name(sym, name),
                                                   codegen_type(cg, sym->type),
                                                   codegen_symbol_linkage(sym));
            }
            return global ? anvil_const_symbol_addr(global) : NULL;
        }
        case AST_SUBSCRIPT_EXPR: {
            mcc_ast_node_t *base = e->data.subscript_expr.array;
            mcc_type_t *base_type = unwrap(base->type);
            int64_t index;
            if (!integer_constant(e->data.subscript_expr.index, &index)) return NULL;
            anvil_value_t *indices[2] = {
                anvil_const_i64(cg->anvil_ctx, 0),
                anvil_const_i64(cg->anvil_ctx, index)
            };
            if (base_type && base_type->kind == TYPE_ARRAY) {
                anvil_value_t *array = object_address(cg, base);
                return array ? anvil_const_gep(array, codegen_type(cg, base_type),
                                               indices, 2) : NULL;
            }
            if (base_type && base_type->kind == TYPE_POINTER) {
                anvil_value_t *pointer = address_value(cg, base);
                return pointer ? anvil_const_gep(pointer,
                    codegen_type(cg, base_type->data.pointer.pointee),
                    &indices[1], 1) : NULL;
            }
            return NULL;
        }
        case AST_MEMBER_EXPR: {
            mcc_ast_node_t *object = e->data.member_expr.object;
            mcc_type_t *record = unwrap(object->type);
            anvil_value_t *address;
            if (e->data.member_expr.is_arrow) {
                address = address_value(cg, object);
                record = record && record->kind == TYPE_POINTER
                    ? unwrap(record->data.pointer.pointee) : NULL;
            } else {
                address = object_address(cg, object);
            }
            const char *member = e->data.member_expr.member;
            /* Same physical walk as codegen_lvalue, through anonymous
             * members to the named one. */
            while (address && record && mcc_type_is_record(record)) {
                size_t ordinal = 0;
                mcc_struct_field_t *match = NULL, *anonymous = NULL;
                for (mcc_struct_field_t *f = record->data.record.fields; f; f = f->next) {
                    if (!is_physical_member(f)) continue;
                    if (f->name && strcmp(f->name, member) == 0) { match = f; break; }
                    if (!f->name && mcc_type_find_field(f->type, member)) { anonymous = f; break; }
                    ordinal++;
                }
                mcc_struct_field_t *step = match ? match : anonymous;
                if (!step) return NULL;
                /* Struct fields and union members (offset 0) alike. */
                anvil_value_t *indices[2] = {
                    anvil_const_i64(cg->anvil_ctx, 0),
                    anvil_const_i32(cg->anvil_ctx, (int32_t)ordinal)
                };
                address = anvil_const_gep(address, codegen_type(cg, record),
                                          indices, 2);
                if (match) return address;
                record = unwrap(anonymous->type);
            }
            return NULL;
        }
        case AST_UNARY_EXPR:
            return e->data.unary_expr.op == UNOP_DEREF
                ? address_value(cg, e->data.unary_expr.operand) : NULL;
        default:
            return NULL;
    }
}

/* Relocatable value of a pointer-typed constant expression. */
static anvil_value_t *address_value(mcc_codegen_t *cg, mcc_ast_node_t *e)
{
    if (!e) return NULL;
    mcc_type_t *type = unwrap(e->type);
    switch (e->kind) {
        case AST_UNARY_EXPR:
            if (e->data.unary_expr.op != UNOP_ADDR) return NULL;
            {
                mcc_ast_node_t *operand = e->data.unary_expr.operand;
                mcc_type_t *operand_type = unwrap(operand->type);
                if (operand_type && operand_type->kind == TYPE_FUNCTION)
                    return address_value(cg, operand);
                return object_address(cg, operand);
            }
        case AST_IDENT_EXPR: {
            mcc_symbol_t *sym = e->data.ident_expr.symbol;
            if (sym && sym->kind == SYM_FUNC) {
                anvil_func_t *func = codegen_get_or_declare_func(cg, sym);
                return func ? anvil_const_symbol_addr(anvil_func_get_value(func)) : NULL;
            }
            if (type && type->kind == TYPE_ARRAY) {
                anvil_value_t *array = object_address(cg, e);
                anvil_value_t *indices[2] = {
                    anvil_const_i64(cg->anvil_ctx, 0), anvil_const_i64(cg->anvil_ctx, 0)
                };
                return array ? anvil_const_gep(array, codegen_type(cg, type),
                                               indices, 2) : NULL;
            }
            return NULL;
        }
        case AST_MEMBER_EXPR:
        case AST_SUBSCRIPT_EXPR:
            /* An array-typed designator decays to its first element. */
            if (type && type->kind == TYPE_ARRAY) {
                anvil_value_t *array = object_address(cg, e);
                anvil_value_t *indices[2] = {
                    anvil_const_i64(cg->anvil_ctx, 0), anvil_const_i64(cg->anvil_ctx, 0)
                };
                return array ? anvil_const_gep(array, codegen_type(cg, type),
                                               indices, 2) : NULL;
            }
            return NULL;
        case AST_BINARY_EXPR: {
            mcc_binop_t op = e->data.binary_expr.op;
            mcc_ast_node_t *pointer = e->data.binary_expr.lhs;
            mcc_ast_node_t *offset = e->data.binary_expr.rhs;
            if (op != BINOP_ADD && op != BINOP_SUB) return NULL;
            if (op == BINOP_ADD && mcc_type_is_integer(unwrap(pointer->type))) {
                pointer = e->data.binary_expr.rhs;
                offset = e->data.binary_expr.lhs;
            }
            int64_t steps;
            if (!integer_constant(offset, &steps)) return NULL;
            if (op == BINOP_SUB) steps = (int64_t)(0 - (uint64_t)steps);
            anvil_value_t *base = address_value(cg, pointer);
            mcc_type_t *base_type = unwrap(pointer->type);
            mcc_type_t *pointee = base_type && base_type->kind == TYPE_POINTER
                ? unwrap(base_type->data.pointer.pointee)
                : base_type && base_type->kind == TYPE_ARRAY
                ? unwrap(base_type->data.array.element) : NULL;
            if (!base || !pointee || codegen_type_has_vla(pointee)) return NULL;
            anvil_value_t *index = anvil_const_i64(cg->anvil_ctx, steps);
            anvil_type_t *element = pointee->kind == TYPE_VOID
                ? anvil_type_u8(cg->anvil_ctx) : codegen_type(cg, pointee);
            if (pointee->kind == TYPE_VOID)
                base = anvil_const_ptr_cast(base, anvil_type_ptr(cg->anvil_ctx, element));
            return base ? anvil_const_gep(base, element, &index, 1) : NULL;
        }
        case AST_STRING_LIT:
            return string_object(cg, e);
        case AST_CAST_EXPR: {
            mcc_type_t *target = unwrap(e->data.cast_expr.target_type);
            if (!target || target->kind != TYPE_POINTER) return NULL;
            anvil_value_t *inner = address_value(cg, e->data.cast_expr.expr);
            return inner ? anvil_const_ptr_cast(inner, codegen_type(cg, target)) : NULL;
        }
        default:
            return NULL;
    }
}

/* Constant for one leaf: arithmetic literals, null pointers, string
 * literals (as char arrays or as pointers to the string) and address
 * constants. */
static anvil_value_t *leaf_constant(mcc_codegen_t *cg, mcc_type_t *type,
                                    mcc_ast_node_t *init)
{
    mcc_type_t *object = unwrap(type);
    anvil_type_t *anvil_type = codegen_type(cg, object);
    if (!object || !init || !anvil_type) return NULL;

    if (is_char_array(object) && init->kind == AST_STRING_LIT) {
        const char *bytes = init->data.string_lit.value;
        size_t length = strlen(bytes);
        size_t count = object->data.array.length;
        anvil_type_t *element = codegen_type(cg, object->data.array.element);
        anvil_value_t **elements = count
            ? mcc_alloc_array(cg->mcc_ctx, count, sizeof(*elements)) : NULL;
        if (count && !elements) return NULL;
        for (size_t i = 0; i < count; i++) {
            elements[i] = codegen_const_int_for_type(
                cg, element, i < length ? (int64_t)(unsigned char)bytes[i] : 0);
            if (!elements[i]) return NULL;
        }
        return anvil_const_array(cg->anvil_ctx, element, elements, count);
    }

    if (mcc_type_is_integer(object)) {
        if (init->kind == AST_INT_LIT)
            return codegen_const_int_for_type(cg, anvil_type,
                                              (int64_t)init->data.int_lit.value);
        if (init->kind == AST_CHAR_LIT)
            return codegen_const_int_for_type(cg, anvil_type,
                                              (int64_t)init->data.char_lit.value);
        if (init->kind == AST_FLOAT_LIT)
            return codegen_const_int_for_type(cg, anvil_type,
                                              (int64_t)init->data.float_lit.value);
        if (init->kind == AST_SIZEOF_EXPR || init->kind == AST_ALIGNOF_EXPR) {
            bool size = init->kind == AST_SIZEOF_EXPR;
            mcc_type_t *arg = size ? init->data.sizeof_expr.type_arg
                                   : init->data.alignof_expr.type_arg;
            mcc_ast_node_t *expr = size ? init->data.sizeof_expr.expr_arg
                                        : init->data.alignof_expr.expr_arg;
            if (!arg && expr) arg = expr->type;
            if (arg)
                return codegen_const_int_for_type(cg, anvil_type, (int64_t)
                    (size ? mcc_type_sizeof(arg) : mcc_type_alignof(arg)));
        }
        return NULL;
    }
    if (mcc_type_is_floating(object)) {
        double value;
        if (!floating_constant(init, &value)) return NULL;
        return anvil_type_size(anvil_type) == 4
            ? anvil_const_f32(cg->anvil_ctx, (float)value)
            : anvil_const_f64(cg->anvil_ctx, value);
    }
    if (mcc_type_is_pointer(object)) {
        if (init->kind == AST_NULL_PTR ||
            (init->kind == AST_INT_LIT && init->data.int_lit.value == 0))
            return anvil_const_null(cg->anvil_ctx, anvil_type);
        if (init->kind == AST_STRING_LIT)
            return codegen_get_string_literal(cg, init->data.string_lit.value);
        anvil_value_t *address = address_value(cg, init);
        if (address && anvil_value_get_type(address) != anvil_type)
            address = anvil_const_ptr_cast(address, anvil_type);
        return address;
    }
    return NULL;
}

anvil_value_t *codegen_const_initializer(mcc_codegen_t *cg, mcc_type_t *type,
                                         mcc_ast_node_t *init)
{
    if (!init) return zero_constant(cg, type);
    if (init->kind != AST_INIT_LIST) {
        anvil_value_t *value = leaf_constant(cg, type, init);
        if (!value)
            mcc_error_at(cg->mcc_ctx, init->location,
                         "initializer element is not a supported constant");
        return value;
    }
    if (!init->data.init_list.resolved) {
        mcc_error_at(cg->mcc_ctx, init->location,
                     "initializer was not resolved by semantic analysis");
        return NULL;
    }

    const_node_t root = {0};
    for (size_t i = 0; i < init->data.init_list.num_entries; i++) {
        const mcc_init_entry_t *entry = &init->data.init_list.entries[i];
        const_node_t *node = &root;
        mcc_type_t *current = type;
        for (size_t s = 0; s < entry->path_len; s++) {
            const mcc_init_step_t *step = &entry->path[s];
            mcc_type_t *object = unwrap(current);
            size_t count = object->kind == TYPE_ARRAY
                ? object->data.array.length
                : object->kind == TYPE_UNION ? 1
                : physical_member_count(object);
            /* An earlier whole-object leaf is overridden member-wise. */
            node->value = NULL;
            if (object->kind == TYPE_UNION &&
                (!node->has_union_member || node->union_member != step->index)) {
                node->children = NULL;      /* another member replaces it */
                node->union_member = step->index;
                node->has_union_member = true;
            }
            if (!node->children) {
                node->children = count
                    ? mcc_alloc_array(cg->mcc_ctx, count,
                                      sizeof(*node->children))
                    : NULL;
                if (count && !node->children) return NULL;
                node->count = count;
            }
            size_t index = object->kind == TYPE_UNION ? 0 : step->index;
            if (index >= node->count) {
                mcc_error_at(cg->mcc_ctx, init->location,
                             "initializer path is outside its object");
                return NULL;
            }
            node = &node->children[index];
            current = step->type;
        }
        node->children = NULL;
        node->count = 0;
        node->has_union_member = false;
        node->value = NULL;
        node->expr = entry->value;
        if (!entry->value)
            continue;               /* the subobject is zero again */
        node->value = leaf_constant(cg, entry->type, entry->value);
        if (!node->value) {
            mcc_error_at(cg->mcc_ctx, entry->value->location,
                         "initializer element is not a supported constant");
            return NULL;
        }
    }
    return materialize(cg, type, &root);
}
