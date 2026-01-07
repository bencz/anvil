#include "types_internal.h"
#include "../core/str.h"
#include <string.h>

AnvilType anvil_type_void_instance = { .kind = ANVIL_TYPE_VOID, .size = 0, .align = 1 };
AnvilType anvil_type_bool_instance = { .kind = ANVIL_TYPE_BOOL, .size = 1, .align = 1 };
AnvilType anvil_type_i8_instance = { .kind = ANVIL_TYPE_I8, .size = 1, .align = 1 };
AnvilType anvil_type_i16_instance = { .kind = ANVIL_TYPE_I16, .size = 2, .align = 2 };
AnvilType anvil_type_i32_instance = { .kind = ANVIL_TYPE_I32, .size = 4, .align = 4 };
AnvilType anvil_type_i64_instance = { .kind = ANVIL_TYPE_I64, .size = 8, .align = 8 };
AnvilType anvil_type_u8_instance = { .kind = ANVIL_TYPE_U8, .size = 1, .align = 1 };
AnvilType anvil_type_u16_instance = { .kind = ANVIL_TYPE_U16, .size = 2, .align = 2 };
AnvilType anvil_type_u32_instance = { .kind = ANVIL_TYPE_U32, .size = 4, .align = 4 };
AnvilType anvil_type_u64_instance = { .kind = ANVIL_TYPE_U64, .size = 8, .align = 8 };
AnvilType anvil_type_f32_instance = { .kind = ANVIL_TYPE_F32, .size = 4, .align = 4 };
AnvilType anvil_type_f64_instance = { .kind = ANVIL_TYPE_F64, .size = 8, .align = 8 };

AnvilType* anvil_type_void(void) { return &anvil_type_void_instance; }
AnvilType* anvil_type_bool(void) { return &anvil_type_bool_instance; }
AnvilType* anvil_type_i8(void) { return &anvil_type_i8_instance; }
AnvilType* anvil_type_i16(void) { return &anvil_type_i16_instance; }
AnvilType* anvil_type_i32(void) { return &anvil_type_i32_instance; }
AnvilType* anvil_type_i64(void) { return &anvil_type_i64_instance; }
AnvilType* anvil_type_u8(void) { return &anvil_type_u8_instance; }
AnvilType* anvil_type_u16(void) { return &anvil_type_u16_instance; }
AnvilType* anvil_type_u32(void) { return &anvil_type_u32_instance; }
AnvilType* anvil_type_u64(void) { return &anvil_type_u64_instance; }
AnvilType* anvil_type_f32(void) { return &anvil_type_f32_instance; }
AnvilType* anvil_type_f64(void) { return &anvil_type_f64_instance; }

AnvilType* anvil_type_ptr_create(AnvilArena* arena, AnvilType* pointee) {
    AnvilType* type = (AnvilType*)anvil_arena_alloc(arena, sizeof(AnvilType));
    type->kind = ANVIL_TYPE_PTR;
    type->size = 8;
    type->align = 8;
    type->ptr.pointee = pointee;
    return type;
}

AnvilType* anvil_type_array_create(AnvilArena* arena, AnvilType* elem, size_t count) {
    AnvilType* type = (AnvilType*)anvil_arena_alloc(arena, sizeof(AnvilType));
    type->kind = ANVIL_TYPE_ARRAY;
    type->size = elem->size * count;
    type->align = elem->align;
    type->array.elem = elem;
    type->array.count = count;
    return type;
}

AnvilType* anvil_type_struct_create(AnvilArena* arena, const char* name) {
    AnvilType* type = (AnvilType*)anvil_arena_alloc(arena, sizeof(AnvilType));
    type->kind = ANVIL_TYPE_STRUCT;
    type->size = 0;
    type->align = 1;
    type->struct_type.name = anvil_arena_strdup(arena, name);
    anvil_vec_init(&type->struct_type.fields, sizeof(AnvilStructField));
    type->struct_type.total_size = 0;
    type->struct_type.total_align = 1;
    type->struct_type.is_packed = false;
    return type;
}

void anvil_type_struct_add_field(AnvilType* struct_type, AnvilArena* arena,
                                  const char* name, AnvilType* field_type) {
    if (struct_type->kind != ANVIL_TYPE_STRUCT) return;
    
    AnvilStructField* field = (AnvilStructField*)anvil_vec_push(&struct_type->struct_type.fields);
    field->name = anvil_arena_strdup(arena, name);
    field->type = field_type;
    
    size_t align = field_type->align;
    if (!struct_type->struct_type.is_packed) {
        size_t padding = (align - (struct_type->struct_type.total_size % align)) % align;
        struct_type->struct_type.total_size += padding;
    }
    field->offset = struct_type->struct_type.total_size;
    struct_type->struct_type.total_size += field_type->size;
    
    if (field_type->align > struct_type->struct_type.total_align) {
        struct_type->struct_type.total_align = field_type->align;
    }
    
    struct_type->size = struct_type->struct_type.total_size;
    struct_type->align = struct_type->struct_type.total_align;
}

AnvilType* anvil_type_func_create(AnvilArena* arena, AnvilType* ret_type, bool is_vararg) {
    AnvilType* type = (AnvilType*)anvil_arena_alloc(arena, sizeof(AnvilType));
    type->kind = ANVIL_TYPE_FUNC;
    type->size = 0;
    type->align = 1;
    type->func.ret_type = ret_type;
    anvil_vec_init(&type->func.param_types, sizeof(AnvilType*));
    type->func.is_vararg = is_vararg;
    return type;
}

void anvil_type_func_add_param(AnvilType* func_type, AnvilType* param_type) {
    if (func_type->kind != ANVIL_TYPE_FUNC) return;
    AnvilType** slot = (AnvilType**)anvil_vec_push(&func_type->func.param_types);
    *slot = param_type;
}

bool anvil_type_is_integer(const AnvilType* type) {
    switch (type->kind) {
        case ANVIL_TYPE_BOOL:
        case ANVIL_TYPE_I8: case ANVIL_TYPE_I16: case ANVIL_TYPE_I32: case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U8: case ANVIL_TYPE_U16: case ANVIL_TYPE_U32: case ANVIL_TYPE_U64:
            return true;
        default:
            return false;
    }
}

bool anvil_type_is_signed(const AnvilType* type) {
    switch (type->kind) {
        case ANVIL_TYPE_I8: case ANVIL_TYPE_I16: case ANVIL_TYPE_I32: case ANVIL_TYPE_I64:
            return true;
        default:
            return false;
    }
}

bool anvil_type_is_unsigned(const AnvilType* type) {
    switch (type->kind) {
        case ANVIL_TYPE_BOOL:
        case ANVIL_TYPE_U8: case ANVIL_TYPE_U16: case ANVIL_TYPE_U32: case ANVIL_TYPE_U64:
            return true;
        default:
            return false;
    }
}

bool anvil_type_is_float(const AnvilType* type) {
    return type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64;
}

bool anvil_type_is_ptr(const AnvilType* type) {
    return type->kind == ANVIL_TYPE_PTR;
}

bool anvil_type_is_void(const AnvilType* type) {
    return type->kind == ANVIL_TYPE_VOID;
}

bool anvil_type_is_aggregate(const AnvilType* type) {
    return type->kind == ANVIL_TYPE_STRUCT || type->kind == ANVIL_TYPE_ARRAY;
}

bool anvil_type_is_vector(const AnvilType* type) {
    return type->kind == ANVIL_TYPE_VECTOR;
}

AnvilType* anvil_type_vector_create(AnvilArena* arena, AnvilType* elem, size_t lanes) {
    AnvilType* type = anvil_arena_alloc(arena, sizeof(AnvilType));
    type->kind = ANVIL_TYPE_VECTOR;
    type->vector.elem = elem;
    type->vector.lanes = lanes;
    type->size = elem->size * lanes;
    type->align = type->size >= 32 ? 32 : (type->size >= 16 ? 16 : 8);
    return type;
}

bool anvil_type_eq(const AnvilType* a, const AnvilType* b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    
    switch (a->kind) {
        case ANVIL_TYPE_PTR:
            return anvil_type_eq(a->ptr.pointee, b->ptr.pointee);
        case ANVIL_TYPE_ARRAY:
            return a->array.count == b->array.count &&
                   anvil_type_eq(a->array.elem, b->array.elem);
        case ANVIL_TYPE_STRUCT:
            return anvil_str_eq(a->struct_type.name, b->struct_type.name);
        case ANVIL_TYPE_FUNC: {
            if (!anvil_type_eq(a->func.ret_type, b->func.ret_type)) return false;
            if (a->func.is_vararg != b->func.is_vararg) return false;
            if (anvil_vec_len(&a->func.param_types) != anvil_vec_len(&b->func.param_types)) return false;
            for (size_t i = 0; i < anvil_vec_len(&a->func.param_types); i++) {
                AnvilType* pa = *(AnvilType**)anvil_vec_get(&a->func.param_types, i);
                AnvilType* pb = *(AnvilType**)anvil_vec_get(&b->func.param_types, i);
                if (!anvil_type_eq(pa, pb)) return false;
            }
            return true;
        }
        default:
            return true;
    }
}

size_t anvil_type_size(const AnvilType* type) {
    return type->size;
}

size_t anvil_type_align(const AnvilType* type) {
    return type->align;
}
