#ifndef ANVIL_MODULE_H
#define ANVIL_MODULE_H

#include "func.h"
#include "types_internal.h"
#include "../core/arena.h"
#include "../core/hash.h"
#include "../core/vec.h"
#include "../core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnvilGlobal {
    const char* name;
    AnvilType* type;
    AnvilValue* init_value;
    bool is_const;
    bool is_external;
    struct AnvilGlobal* next;
} AnvilGlobal;

typedef struct AnvilModule {
    const char* name;
    AnvilArena* arena;
    AnvilErrorCtx errors;
    
    AnvilFunc* first_func;
    AnvilFunc* last_func;
    int func_count;
    AnvilHash func_map;
    
    AnvilGlobal* first_global;
    AnvilGlobal* last_global;
    int global_count;
    AnvilHash global_map;
    
    AnvilHash type_map;
    AnvilVec string_pool;
} AnvilModule;

AnvilModule* anvil_module_create(const char* name);
void anvil_module_destroy(AnvilModule* mod);

AnvilFunc* anvil_module_add_func(AnvilModule* mod, const char* name, AnvilType* ret_type);
AnvilFunc* anvil_module_get_func(AnvilModule* mod, const char* name);

AnvilGlobal* anvil_module_add_global(AnvilModule* mod, const char* name, AnvilType* type, AnvilValue* init);
AnvilGlobal* anvil_module_get_global(AnvilModule* mod, const char* name);

AnvilType* anvil_module_get_struct(AnvilModule* mod, const char* name);
AnvilType* anvil_module_add_struct(AnvilModule* mod, const char* name);

const char* anvil_module_intern_string(AnvilModule* mod, const char* str);

#ifdef __cplusplus
}
#endif

#endif
