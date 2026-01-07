#ifndef ANVIL_TYPES_H
#define ANVIL_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnvilModule AnvilModule;
typedef struct AnvilFunc AnvilFunc;
typedef struct AnvilBlock AnvilBlock;
typedef struct AnvilVar AnvilVar;
typedef struct AnvilValue AnvilValue;
typedef struct AnvilType AnvilType;
typedef struct AnvilIf AnvilIf;
typedef struct AnvilLoop AnvilLoop;

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
AnvilType* anvil_type_ptr(AnvilModule* mod, AnvilType* pointee);
AnvilType* anvil_type_array(AnvilModule* mod, AnvilType* elem, int count);
AnvilType* anvil_type_struct(AnvilModule* mod, const char* name);
void anvil_struct_add_field(AnvilType* struct_type, const char* name, AnvilType* type);

#ifdef __cplusplus
}
#endif

#endif
