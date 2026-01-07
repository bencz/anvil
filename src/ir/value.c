#include "value.h"
#include "../core/str.h"
#include <string.h>

static uint32_t next_value_id = 1;

AnvilValue* anvil_value_const_i8(AnvilArena* arena, int8_t val) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_INT;
    v->type = anvil_type_i8();
    v->id = next_value_id++;
    v->i64 = val;
    return v;
}

AnvilValue* anvil_value_const_i16(AnvilArena* arena, int16_t val) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_INT;
    v->type = anvil_type_i16();
    v->id = next_value_id++;
    v->i64 = val;
    return v;
}

AnvilValue* anvil_value_const_i32(AnvilArena* arena, int32_t val) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_INT;
    v->type = anvil_type_i32();
    v->id = next_value_id++;
    v->i64 = val;
    return v;
}

AnvilValue* anvil_value_const_i64(AnvilArena* arena, int64_t val) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_INT;
    v->type = anvil_type_i64();
    v->id = next_value_id++;
    v->i64 = val;
    return v;
}

AnvilValue* anvil_value_const_u8(AnvilArena* arena, uint8_t val) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_INT;
    v->type = anvil_type_u8();
    v->id = next_value_id++;
    v->u64 = val;
    return v;
}

AnvilValue* anvil_value_const_u16(AnvilArena* arena, uint16_t val) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_INT;
    v->type = anvil_type_u16();
    v->id = next_value_id++;
    v->u64 = val;
    return v;
}

AnvilValue* anvil_value_const_u32(AnvilArena* arena, uint32_t val) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_INT;
    v->type = anvil_type_u32();
    v->id = next_value_id++;
    v->u64 = val;
    return v;
}

AnvilValue* anvil_value_const_u64(AnvilArena* arena, uint64_t val) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_INT;
    v->type = anvil_type_u64();
    v->id = next_value_id++;
    v->u64 = val;
    return v;
}

AnvilValue* anvil_value_const_f32(AnvilArena* arena, float val) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_FLOAT;
    v->type = anvil_type_f32();
    v->id = next_value_id++;
    v->f64 = val;
    return v;
}

AnvilValue* anvil_value_const_f64(AnvilArena* arena, double val) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_FLOAT;
    v->type = anvil_type_f64();
    v->id = next_value_id++;
    v->f64 = val;
    return v;
}

AnvilValue* anvil_value_const_bool(AnvilArena* arena, bool val) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_INT;
    v->type = anvil_type_bool();
    v->id = next_value_id++;
    v->u64 = val ? 1 : 0;
    return v;
}

AnvilValue* anvil_value_const_null(AnvilArena* arena, AnvilType* ptr_type) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_NULL;
    v->type = ptr_type;
    v->id = next_value_id++;
    v->u64 = 0;
    return v;
}

AnvilValue* anvil_value_const_string(AnvilArena* arena, const char* str) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_CONST_STRING;
    v->type = anvil_type_ptr_create(arena, anvil_type_i8());
    v->id = next_value_id++;
    v->str = anvil_arena_strdup(arena, str);
    return v;
}

AnvilVar* anvil_var_create(AnvilArena* arena, const char* name, AnvilType* type, int index, bool is_param) {
    AnvilVar* var = (AnvilVar*)anvil_arena_alloc(arena, sizeof(AnvilVar));
    var->name = anvil_arena_strdup(arena, name);
    var->type = type;
    var->index = index;
    var->is_param = is_param;
    var->value = NULL;
    return var;
}

AnvilValue* anvil_value_from_var(AnvilArena* arena, AnvilVar* var) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = var->is_param ? ANVIL_VALUE_PARAM : ANVIL_VALUE_VAR;
    v->type = var->type;
    v->id = next_value_id++;
    v->var.name = var->name;
    v->var.index = var->index;
    v->var.is_param = var->is_param;
    var->value = v;
    return v;
}

AnvilValue* anvil_value_temp(AnvilArena* arena, AnvilType* type, uint32_t id) {
    AnvilValue* v = (AnvilValue*)anvil_arena_alloc(arena, sizeof(AnvilValue));
    v->kind = ANVIL_VALUE_TEMP;
    v->type = type;
    v->id = id ? id : next_value_id++;
    return v;
}

bool anvil_value_is_const(const AnvilValue* val) {
    return val->kind == ANVIL_VALUE_CONST_INT ||
           val->kind == ANVIL_VALUE_CONST_FLOAT ||
           val->kind == ANVIL_VALUE_CONST_NULL ||
           val->kind == ANVIL_VALUE_CONST_STRING;
}

bool anvil_value_is_const_int(const AnvilValue* val) {
    return val->kind == ANVIL_VALUE_CONST_INT;
}

bool anvil_value_is_const_zero(const AnvilValue* val) {
    if (val->kind == ANVIL_VALUE_CONST_INT) return val->i64 == 0;
    if (val->kind == ANVIL_VALUE_CONST_FLOAT) return val->f64 == 0.0;
    if (val->kind == ANVIL_VALUE_CONST_NULL) return true;
    return false;
}
