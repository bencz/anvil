#include "module.h"
#include "../core/str.h"
#include <string.h>

AnvilModule* anvil_module_create(const char* name) {
    AnvilArena* arena = anvil_arena_new();
    if (!arena) return NULL;
    
    AnvilModule* mod = (AnvilModule*)anvil_arena_alloc(arena, sizeof(AnvilModule));
    mod->name = anvil_arena_strdup(arena, name);
    mod->arena = arena;
    anvil_error_ctx_init(&mod->errors);
    
    mod->first_func = NULL;
    mod->last_func = NULL;
    mod->func_count = 0;
    anvil_hash_init(&mod->func_map);
    
    mod->first_global = NULL;
    mod->last_global = NULL;
    mod->global_count = 0;
    anvil_hash_init(&mod->global_map);
    
    anvil_hash_init(&mod->type_map);
    anvil_vec_init(&mod->string_pool, sizeof(const char*));
    
    return mod;
}

void anvil_module_destroy(AnvilModule* mod) {
    if (!mod) return;
    anvil_error_ctx_free(&mod->errors);
    anvil_hash_free(&mod->func_map);
    anvil_hash_free(&mod->global_map);
    anvil_hash_free(&mod->type_map);
    anvil_vec_free(&mod->string_pool);
    anvil_arena_free(mod->arena);
}

AnvilFunc* anvil_module_add_func(AnvilModule* mod, const char* name, AnvilType* ret_type) {
    AnvilFunc* func = anvil_func_create(mod->arena, name, ret_type);
    func->module = mod;
    
    if (mod->last_func) {
        mod->last_func->next = func;
        mod->last_func = func;
    } else {
        mod->first_func = mod->last_func = func;
    }
    mod->func_count++;
    
    anvil_hash_insert(&mod->func_map, func->name, func);
    return func;
}

AnvilFunc* anvil_module_get_func(AnvilModule* mod, const char* name) {
    return (AnvilFunc*)anvil_hash_get(&mod->func_map, name);
}

AnvilGlobal* anvil_module_add_global(AnvilModule* mod, const char* name, AnvilType* type, AnvilValue* init) {
    AnvilGlobal* global = (AnvilGlobal*)anvil_arena_alloc(mod->arena, sizeof(AnvilGlobal));
    global->name = anvil_arena_strdup(mod->arena, name);
    global->type = type;
    global->init_value = init;
    global->is_const = false;
    global->is_external = false;
    global->next = NULL;
    
    if (mod->last_global) {
        mod->last_global->next = global;
        mod->last_global = global;
    } else {
        mod->first_global = mod->last_global = global;
    }
    mod->global_count++;
    
    anvil_hash_insert(&mod->global_map, global->name, global);
    return global;
}

AnvilGlobal* anvil_module_get_global(AnvilModule* mod, const char* name) {
    return (AnvilGlobal*)anvil_hash_get(&mod->global_map, name);
}

AnvilType* anvil_module_get_struct(AnvilModule* mod, const char* name) {
    return (AnvilType*)anvil_hash_get(&mod->type_map, name);
}

AnvilType* anvil_module_add_struct(AnvilModule* mod, const char* name) {
    AnvilType* existing = anvil_module_get_struct(mod, name);
    if (existing) return existing;
    
    AnvilType* type = anvil_type_struct_create(mod->arena, name);
    anvil_hash_insert(&mod->type_map, type->struct_type.name, type);
    return type;
}

const char* anvil_module_intern_string(AnvilModule* mod, const char* str) {
    for (size_t i = 0; i < anvil_vec_len(&mod->string_pool); i++) {
        const char* s = *(const char**)anvil_vec_get(&mod->string_pool, i);
        if (anvil_str_eq(s, str)) return s;
    }
    const char* interned = anvil_arena_strdup(mod->arena, str);
    const char** slot = (const char**)anvil_vec_push(&mod->string_pool);
    *slot = interned;
    return interned;
}
