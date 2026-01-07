#ifndef ANVIL_VALUE_H
#define ANVIL_VALUE_H

#include "types_internal.h"
#include "../core/arena.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnvilValueKind {
    ANVIL_VALUE_CONST_INT,
    ANVIL_VALUE_CONST_FLOAT,
    ANVIL_VALUE_CONST_NULL,
    ANVIL_VALUE_CONST_STRING,
    ANVIL_VALUE_VAR,
    ANVIL_VALUE_PARAM,
    ANVIL_VALUE_TEMP,
    ANVIL_VALUE_GLOBAL,
    ANVIL_VALUE_FUNC_REF,
} AnvilValueKind;

typedef struct AnvilValue {
    AnvilValueKind kind;
    AnvilType* type;
    uint32_t id;
    
    union {
        int64_t i64;
        uint64_t u64;
        double f64;
        const char* str;
        struct {
            const char* name;
            int index;
            bool is_param;
        } var;
        struct {
            const char* name;
        } global;
        struct {
            const char* name;
        } func_ref;
    };
} AnvilValue;

typedef struct AnvilVar {
    const char* name;
    AnvilType* type;
    int index;
    bool is_param;
    AnvilValue* value;
} AnvilVar;

AnvilValue* anvil_value_const_i8(AnvilArena* arena, int8_t val);
AnvilValue* anvil_value_const_i16(AnvilArena* arena, int16_t val);
AnvilValue* anvil_value_const_i32(AnvilArena* arena, int32_t val);
AnvilValue* anvil_value_const_i64(AnvilArena* arena, int64_t val);
AnvilValue* anvil_value_const_u8(AnvilArena* arena, uint8_t val);
AnvilValue* anvil_value_const_u16(AnvilArena* arena, uint16_t val);
AnvilValue* anvil_value_const_u32(AnvilArena* arena, uint32_t val);
AnvilValue* anvil_value_const_u64(AnvilArena* arena, uint64_t val);
AnvilValue* anvil_value_const_f32(AnvilArena* arena, float val);
AnvilValue* anvil_value_const_f64(AnvilArena* arena, double val);
AnvilValue* anvil_value_const_bool(AnvilArena* arena, bool val);
AnvilValue* anvil_value_const_null(AnvilArena* arena, AnvilType* ptr_type);
AnvilValue* anvil_value_const_string(AnvilArena* arena, const char* str);

AnvilVar* anvil_var_create(AnvilArena* arena, const char* name, AnvilType* type, int index, bool is_param);
AnvilValue* anvil_value_from_var(AnvilArena* arena, AnvilVar* var);
AnvilValue* anvil_value_temp(AnvilArena* arena, AnvilType* type, uint32_t id);

bool anvil_value_is_const(const AnvilValue* val);
bool anvil_value_is_const_int(const AnvilValue* val);
bool anvil_value_is_const_zero(const AnvilValue* val);

#ifdef __cplusplus
}
#endif

#endif
