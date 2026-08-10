/*
 * ANVIL - Module implementation
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include <stdlib.h>
#include <string.h>

static uint64_t symbol_hash(const char *name)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    while (*name) {
        hash ^= (unsigned char)*name++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool anvil_symbol_linkage_compatible(anvil_linkage_t existing,
                                     bool existing_is_declaration,
                                     anvil_linkage_t incoming,
                                     bool incoming_is_declaration,
                                     bool is_function)
{
    if ((unsigned)existing > (unsigned)ANVIL_LINK_COMMON ||
        (unsigned)incoming > (unsigned)ANVIL_LINK_COMMON ||
        (is_function && (existing == ANVIL_LINK_COMMON ||
                         incoming == ANVIL_LINK_COMMON))) return false;
    if (existing_is_declaration == incoming_is_declaration)
        return existing == incoming;

    anvil_linkage_t declaration = existing_is_declaration ? existing : incoming;
    anvil_linkage_t definition = existing_is_declaration ? incoming : existing;
    if (declaration == definition) return true;
    if (declaration != ANVIL_LINK_EXTERNAL) return false;
    return definition == ANVIL_LINK_WEAK ||
           (!is_function && definition == ANVIL_LINK_COMMON);
}

anvil_value_t *anvil_module_lookup_symbol(const anvil_module_t *mod,
                                           const char *name)
{
    if (!mod || !name || !mod->symbol_bucket_count) return NULL;
    size_t slot = (size_t)(symbol_hash(name) &
                           (uint64_t)(mod->symbol_bucket_count - 1));
    for (anvil_value_t *value = mod->symbol_buckets[slot]; value;
         value = value->symbol_next) {
        if (value->name && strcmp(value->name, name) == 0) return value;
    }
    return NULL;
}

size_t anvil_module_symbol_count(const anvil_module_t *mod)
{
    return mod ? mod->num_symbols : 0;
}

anvil_value_t *anvil_module_symbol_at(const anvil_module_t *mod, size_t index)
{
    return mod && index < mod->num_symbols ? mod->symbols[index] : NULL;
}

bool anvil_module_symbol_prepare(anvil_module_t *mod, size_t additional)
{
    if (!mod || additional > SIZE_MAX - mod->num_symbols) return false;
    size_t needed = mod->num_symbols + additional;
    size_t vector_cap = mod->symbol_capacity ? mod->symbol_capacity : 16;
    while (vector_cap < needed) {
        if (vector_cap > SIZE_MAX / 2) { vector_cap = needed; break; }
        vector_cap *= 2;
    }
    if (vector_cap > SIZE_MAX / sizeof(*mod->symbols)) return false;

    size_t bucket_cap = mod->symbol_bucket_count ? mod->symbol_bucket_count : 32;
    while (needed > bucket_cap - bucket_cap / 4) {
        if (bucket_cap > SIZE_MAX / 2) return false;
        bucket_cap *= 2;
    }
    if (bucket_cap > SIZE_MAX / sizeof(*mod->symbol_buckets)) return false;
    if (vector_cap == mod->symbol_capacity &&
        bucket_cap == mod->symbol_bucket_count) return true;

    anvil_value_t **new_symbols = mod->symbols;
    anvil_value_t **allocated_symbols = NULL;
    if (vector_cap != mod->symbol_capacity) {
        allocated_symbols = anvil_ctx_malloc(
            mod->ctx, vector_cap * sizeof(*allocated_symbols));
        if (!allocated_symbols) return false;
        if (mod->num_symbols)
            memcpy(allocated_symbols, mod->symbols,
                   mod->num_symbols * sizeof(*allocated_symbols));
        new_symbols = allocated_symbols;
    }

    anvil_value_t **new_buckets = mod->symbol_buckets;
    if (bucket_cap != mod->symbol_bucket_count) {
        new_buckets = anvil_ctx_calloc(mod->ctx, bucket_cap,
                                       sizeof(*new_buckets));
        if (!new_buckets) { free(allocated_symbols); return false; }
        for (size_t i = 0; i < mod->num_symbols; i++) {
            anvil_value_t *value = new_symbols[i];
            size_t slot = (size_t)(symbol_hash(value->name) &
                                   (uint64_t)(bucket_cap - 1));
            value->symbol_next = new_buckets[slot];
            new_buckets[slot] = value;
        }
    }

    if (allocated_symbols) {
        free(mod->symbols);
        mod->symbols = new_symbols;
        mod->symbol_capacity = vector_cap;
    }
    if (new_buckets != mod->symbol_buckets) {
        free(mod->symbol_buckets);
        mod->symbol_buckets = new_buckets;
        mod->symbol_bucket_count = bucket_cap;
    }
    return true;
}

void anvil_module_symbol_register(anvil_module_t *mod, anvil_value_t *value)
{
    size_t slot = (size_t)(symbol_hash(value->name) &
                           (uint64_t)(mod->symbol_bucket_count - 1));
    value->symbol_next = mod->symbol_buckets[slot];
    mod->symbol_buckets[slot] = value;
    mod->symbols[mod->num_symbols++] = value;
}

static anvil_error_t verify_codegen_stage(anvil_module_t *mod,
                                          const char *stage)
{
    char verify_error[256] = { 0 };
    if (anvil_module_verify(mod, verify_error, sizeof(verify_error))) {
        return ANVIL_OK;
    }

    anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_OP,
                    "IR verification failed %s: %s",
                    stage,
                    verify_error[0] ? verify_error : "invalid IR");
    return ANVIL_ERR_INVALID_OP;
}

anvil_module_t *anvil_module_create(anvil_ctx_t *ctx, const char *name)
{
    if (!ctx) return NULL;
    if (!ctx->target_configured || !ctx->backend) {
        anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                        "Select a target before creating a module");
        return NULL;
    }
    
    anvil_module_t *mod = anvil_ctx_calloc(ctx, 1, sizeof(anvil_module_t));
    if (!mod) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                        "Out of memory creating module");
        return NULL;
    }
    
    mod->name = anvil_ctx_strdup(ctx, name ? name : "module");
    if (!mod->name) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                        "Out of memory copying module name");
        free(mod);
        return NULL;
    }
    mod->ctx = ctx;
    anvil_ctx_freeze_target(ctx);
    
    /* Add to context's module list */
    mod->next = ctx->modules;
    ctx->modules = mod;
    
    return mod;
}

void anvil_module_destroy(anvil_module_t *mod)
{
    if (!mod) return;

    /* The context historically stored a global insertion point.  Never leave
     * it pointing into a module that is about to be destroyed. */
    if (mod->ctx && mod->ctx->insert_block &&
        mod->ctx->insert_block->owner_module == mod) {
        mod->ctx->insert_block = NULL;
        mod->ctx->insert_point = NULL;
    }

    /* Remove from context's module list to prevent double-free */
    if (mod->ctx) {
        anvil_module_t **pp = &mod->ctx->modules;
        while (*pp) {
            if (*pp == mod) {
                *pp = mod->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }
    
    /* Invalidate module ownership on context-owned values before the module
     * and function records disappear. */
    for (anvil_value_t *value = mod->ctx ? mod->ctx->owned_values : NULL;
         value; value = value->ctx_next_owned) {
        if (value->owner_module == mod) value->owner_module = NULL;
    }
    for (anvil_block_t *block = mod->ctx ? mod->ctx->owned_blocks : NULL;
         block; block = block->ctx_next_owned)
        if (block->owner_module == mod) block->owner_module = NULL;
    for (anvil_instr_t *instr = mod->ctx ? mod->ctx->owned_instrs : NULL;
         instr; instr = instr->ctx_next_owned)
        if (instr->owner_module == mod) instr->owner_module = NULL;

    /* Function handles are context-owned/tombstoned so checked APIs can
     * diagnose use after module destruction without dereferencing freed
     * storage. */
    anvil_func_t *func = mod->funcs;
    while (func) {
        anvil_func_t *next = func->next;
        func->parent = NULL;
        func = next;
    }
    
    /* Destroy globals */
    anvil_global_t *global = mod->globals;
    while (global) {
        anvil_global_t *next = global->next;
        free(global);
        global = next;
    }
    
    /* Destroy string table */
    free(mod->strings.strings);
    free(mod->symbol_buckets);
    free(mod->symbols);
    
    free(mod->name);
    free(mod);
}

static anvil_value_t *module_add_global_impl(anvil_module_t *mod,
                                             const char *name,
                                             anvil_type_t *type,
                                             anvil_linkage_t linkage,
                                             bool declaration)
{
    if (!mod) return NULL;
    if (!name || !*name || !type ||
        (unsigned)linkage > (unsigned)ANVIL_LINK_COMMON) {
        anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_ARG,
                        "Global declaration/definition has invalid arguments");
        return NULL;
    }
    if (type->owner_ctx != mod->ctx || type->kind == ANVIL_TYPE_FUNC) {
        anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_TYPE,
                        "Global type must be a non-function type from its module context");
        return NULL;
    }

    anvil_value_t *existing = anvil_module_lookup_symbol(mod, name);
    if (existing) {
        if (existing->kind != ANVIL_VAL_GLOBAL ||
            !anvil_types_equal(existing->type, type)) {
            anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_TYPE,
                            "Symbol '%s' redeclared with a different kind or type",
                            name);
            return NULL;
        }
        if (declaration && anvil_symbol_linkage_compatible(
                existing->data.global.linkage,
                existing->data.global.is_declaration,
                linkage, true, false)) return existing;
        if (!existing->data.global.is_declaration) {
            if (!declaration && linkage == ANVIL_LINK_COMMON &&
                existing->data.global.linkage == ANVIL_LINK_COMMON)
                return existing; /* compatible repeated tentative definition */
            anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_OP,
                            "Global '%s' is already defined", name);
            return NULL;
        }
        if (declaration || !anvil_symbol_linkage_compatible(
                existing->data.global.linkage, true,
                linkage, false, false)) {
            anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_OP,
                            "Symbol '%s' redeclared with incompatible linkage", name);
            return NULL;
        }
        existing->data.global.linkage = linkage;
        existing->data.global.is_declaration = false;
        return existing;
    }
    if (!anvil_module_symbol_prepare(mod, 1)) {
        anvil_set_error(mod->ctx, ANVIL_ERR_NOMEM,
                        "Out of memory growing module symbol table");
        return NULL;
    }
    
    anvil_global_t *global = anvil_ctx_calloc(mod->ctx, 1, sizeof(*global));
    if (!global) return NULL;
    
    anvil_value_t *val = anvil_value_create(mod->ctx, ANVIL_VAL_GLOBAL, type, name);
    if (!val) {
        free(global);
        return NULL;
    }
    
    val->data.global.linkage = linkage;
    val->data.global.init = NULL;
    val->data.global.is_declaration = declaration;
    val->owner_module = mod;
    
    global->value = val;
    global->next = mod->globals;
    mod->globals = global;
    mod->num_globals++;
    anvil_module_symbol_register(mod, val);
    
    return val;
}

anvil_value_t *anvil_module_add_global(anvil_module_t *mod, const char *name,
                                        anvil_type_t *type, anvil_linkage_t linkage)
{
    return module_add_global_impl(mod, name, type, linkage, false);
}

anvil_value_t *anvil_module_declare_global(anvil_module_t *mod,
                                            const char *name,
                                            anvil_type_t *type,
                                            anvil_linkage_t linkage)
{
    return module_add_global_impl(mod, name, type, linkage, true);
}

anvil_value_t *anvil_module_add_extern(anvil_module_t *mod, const char *name,
                                        anvil_type_t *type)
{
    if (mod && type && type->kind == ANVIL_TYPE_FUNC) {
        anvil_func_t *func = anvil_func_declare(mod, name, type);
        return anvil_func_get_value(func);
    }
    return anvil_module_declare_global(mod, name, type, ANVIL_LINK_EXTERNAL);
}

anvil_error_t anvil_module_codegen(anvil_module_t *mod, char **output, size_t *len)
{
    if (!mod || !output) return ANVIL_ERR_INVALID_ARG;
    if (len) *len = 0;
    *output = NULL;
    
    anvil_ctx_t *ctx = mod->ctx;
    if (!ctx->backend) {
        anvil_set_error(ctx, ctx->target_configured ? ANVIL_ERR_NO_BACKEND
                                                    : ANVIL_ERR_NO_TARGET,
                        ctx->target_configured ? "No backend configured"
                                               : "No target configured");
        return ctx->target_configured ? ANVIL_ERR_NO_BACKEND
                                      : ANVIL_ERR_NO_TARGET;
    }

    anvil_error_t verify_err = verify_codegen_stage(mod, "before optimization");
    if (verify_err != ANVIL_OK) return verify_err;

    anvil_error_t opt_err = anvil_module_optimize(mod);
    if (opt_err != ANVIL_OK) return opt_err;

    verify_err = verify_codegen_stage(mod, "after optimization");
    if (verify_err != ANVIL_OK) return verify_err;
    
    /* Call prepare_ir if the backend provides it */
    if (ctx->backend->ops->prepare_ir) {
        anvil_error_t err = ctx->backend->ops->prepare_ir(ctx->backend, mod);
        if (err != ANVIL_OK) return err;

        verify_err = verify_codegen_stage(mod, "after backend preparation");
        if (verify_err != ANVIL_OK) return verify_err;
    }
    
    return ctx->backend->ops->codegen_module(ctx->backend, mod, output, len);
}

anvil_error_t anvil_module_write(anvil_module_t *mod, const char *filename)
{
    if (!mod || !filename) return ANVIL_ERR_INVALID_ARG;
    
    char *output = NULL;
    size_t len = 0;
    
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    if (err != ANVIL_OK) return err;
    
    FILE *f = fopen(filename, "w");
    if (!f) {
        free(output);
        anvil_set_error(mod->ctx, ANVIL_ERR_IO, "Cannot open file: %s", filename);
        return ANVIL_ERR_IO;
    }
    
    size_t written = fwrite(output, 1, len, f);
    int close_result = fclose(f);
    free(output);

    if (written != len || close_result != 0) {
        anvil_set_error(mod->ctx, ANVIL_ERR_IO,
                        "Failed to write complete output file: %s", filename);
        return ANVIL_ERR_IO;
    }
    
    return ANVIL_OK;
}
