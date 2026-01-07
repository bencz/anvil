#ifndef ANVIL_TYPES_INTERNAL_H
#define ANVIL_TYPES_INTERNAL_H

#include "../core/arena.h"
#include "../core/vec.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnvilTypeKind {
    ANVIL_TYPE_VOID,
    ANVIL_TYPE_BOOL,
    ANVIL_TYPE_I8,
    ANVIL_TYPE_I16,
    ANVIL_TYPE_I32,
    ANVIL_TYPE_I64,
    ANVIL_TYPE_U8,
    ANVIL_TYPE_U16,
    ANVIL_TYPE_U32,
    ANVIL_TYPE_U64,
    ANVIL_TYPE_F32,
    ANVIL_TYPE_F64,
    ANVIL_TYPE_PTR,
    ANVIL_TYPE_ARRAY,
    ANVIL_TYPE_STRUCT,
    ANVIL_TYPE_FUNC,
} AnvilTypeKind;

typedef struct AnvilStructField {
    const char* name;
    struct AnvilType* type;
    size_t offset;
} AnvilStructField;

typedef struct AnvilType {
    AnvilTypeKind kind;
    size_t size;
    size_t align;
    
    union {
        struct {
            struct AnvilType* pointee;
        } ptr;
        
        struct {
            struct AnvilType* elem;
            size_t count;
        } array;
        
        struct {
            const char* name;
            AnvilVec fields;
            size_t total_size;
            size_t total_align;
            bool is_packed;
        } struct_type;
        
        struct {
            struct AnvilType* ret_type;
            AnvilVec param_types;
            bool is_vararg;
        } func;
    };
} AnvilType;

extern AnvilType anvil_type_void_instance;
extern AnvilType anvil_type_bool_instance;
extern AnvilType anvil_type_i8_instance;
extern AnvilType anvil_type_i16_instance;
extern AnvilType anvil_type_i32_instance;
extern AnvilType anvil_type_i64_instance;
extern AnvilType anvil_type_u8_instance;
extern AnvilType anvil_type_u16_instance;
extern AnvilType anvil_type_u32_instance;
extern AnvilType anvil_type_u64_instance;
extern AnvilType anvil_type_f32_instance;
extern AnvilType anvil_type_f64_instance;

AnvilType* anvil_type_void(void);
AnvilType* anvil_type_bool(void);
AnvilType* anvil_type_i8(void);
AnvilType* anvil_type_i16(void);
AnvilType* anvil_type_i32(void);
AnvilType* anvil_type_i64(void);
AnvilType* anvil_type_u8(void);
AnvilType* anvil_type_u16(void);
AnvilType* anvil_type_u32(void);
AnvilType* anvil_type_u64(void);
AnvilType* anvil_type_f32(void);
AnvilType* anvil_type_f64(void);

AnvilType* anvil_type_ptr_create(AnvilArena* arena, AnvilType* pointee);
AnvilType* anvil_type_array_create(AnvilArena* arena, AnvilType* elem, size_t count);
AnvilType* anvil_type_struct_create(AnvilArena* arena, const char* name);
void anvil_type_struct_add_field(AnvilType* struct_type, AnvilArena* arena,
                                  const char* name, AnvilType* field_type);
AnvilType* anvil_type_func_create(AnvilArena* arena, AnvilType* ret_type, bool is_vararg);
void anvil_type_func_add_param(AnvilType* func_type, AnvilType* param_type);

bool anvil_type_is_integer(const AnvilType* type);
bool anvil_type_is_signed(const AnvilType* type);
bool anvil_type_is_unsigned(const AnvilType* type);
bool anvil_type_is_float(const AnvilType* type);
bool anvil_type_is_ptr(const AnvilType* type);
bool anvil_type_is_void(const AnvilType* type);
bool anvil_type_is_aggregate(const AnvilType* type);
bool anvil_type_eq(const AnvilType* a, const AnvilType* b);
size_t anvil_type_size(const AnvilType* type);
size_t anvil_type_align(const AnvilType* type);

#ifdef __cplusplus
}
#endif

#endif
