/*
 * MCC - Micro C Compiler
 * Symbol table implementation
 */

#include "mcc.h"

#define SYMBOL_TABLE_SIZE 256
#define TAG_TABLE_SIZE 64
#define LABEL_TABLE_SIZE 32

static const char *sym_kind_names[] = {
    [SYM_VAR]        = "variable",
    [SYM_FUNC]       = "function",
    [SYM_PARAM]      = "parameter",
    [SYM_TYPEDEF]    = "typedef",
    [SYM_STRUCT]     = "struct",
    [SYM_UNION]      = "union",
    [SYM_ENUM]       = "enum",
    [SYM_ENUM_CONST] = "enum constant",
    [SYM_LABEL]      = "label",
};

static unsigned hash_string(const char *s)
{
    unsigned h = 0;
    while (*s) {
        h = h * 31 + (unsigned char)*s++;
    }
    return h;
}

mcc_symtab_t *mcc_symtab_create(mcc_context_t *ctx, mcc_type_context_t *types)
{
    mcc_symtab_t *symtab = mcc_alloc(ctx, sizeof(mcc_symtab_t));
    symtab->ctx = ctx;
    symtab->types = types;
    
    /* Create global scope */
    mcc_scope_t *global = mcc_alloc(ctx, sizeof(mcc_scope_t));
    global->table_size = SYMBOL_TABLE_SIZE;
    global->symbols = mcc_alloc(ctx, SYMBOL_TABLE_SIZE * sizeof(mcc_symbol_t*));
    global->tag_table_size = TAG_TABLE_SIZE;
    global->tags = mcc_alloc(ctx, TAG_TABLE_SIZE * sizeof(mcc_symbol_t*));
    global->is_file_scope = true;
    global->depth = 0;
    
    symtab->global = global;
    symtab->current = global;
    
    return symtab;
}

void mcc_symtab_destroy(mcc_symtab_t *symtab)
{
    (void)symtab; /* Arena allocated */
}

void mcc_symtab_push_scope(mcc_symtab_t *symtab)
{
    mcc_scope_t *scope = mcc_alloc(symtab->ctx, sizeof(mcc_scope_t));
    scope->parent = symtab->current;
    scope->table_size = SYMBOL_TABLE_SIZE;
    scope->symbols = mcc_alloc(symtab->ctx, SYMBOL_TABLE_SIZE * sizeof(mcc_symbol_t*));
    scope->tag_table_size = TAG_TABLE_SIZE;
    scope->tags = mcc_alloc(symtab->ctx, TAG_TABLE_SIZE * sizeof(mcc_symbol_t*));
    scope->is_block_scope = true;
    scope->depth = symtab->current->depth + 1;
    scope->stack_offset = symtab->current->stack_offset;
    
    symtab->current = scope;
}

void mcc_symtab_push_function_scope(mcc_symtab_t *symtab)
{
    mcc_scope_t *scope = mcc_alloc(symtab->ctx, sizeof(mcc_scope_t));
    scope->parent = symtab->current;
    scope->table_size = SYMBOL_TABLE_SIZE;
    scope->symbols = mcc_alloc(symtab->ctx, SYMBOL_TABLE_SIZE * sizeof(mcc_symbol_t*));
    scope->tag_table_size = TAG_TABLE_SIZE;
    scope->tags = mcc_alloc(symtab->ctx, TAG_TABLE_SIZE * sizeof(mcc_symbol_t*));
    scope->label_table_size = LABEL_TABLE_SIZE;
    scope->labels = mcc_alloc(symtab->ctx, LABEL_TABLE_SIZE * sizeof(mcc_symbol_t*));
    scope->is_function_scope = true;
    scope->depth = symtab->current->depth + 1;
    scope->stack_offset = 0;
    
    symtab->current = scope;
}

void mcc_symtab_pop_scope(mcc_symtab_t *symtab)
{
    if (symtab->current->parent) {
        symtab->current = symtab->current->parent;
    }
}

mcc_scope_t *mcc_symtab_current_scope(mcc_symtab_t *symtab)
{
    return symtab->current;
}

bool mcc_symtab_is_global_scope(mcc_symtab_t *symtab)
{
    return symtab->current == symtab->global;
}

mcc_symbol_t *mcc_symtab_define(mcc_symtab_t *symtab, const char *name,
                                 mcc_sym_kind_t kind, mcc_type_t *type,
                                 mcc_location_t loc)
{
    /* Check for redefinition in current scope */
    mcc_symbol_t *existing = mcc_symtab_lookup_current(symtab, name);
    if (existing) {
        /* Allow function redeclaration */
        if (kind == SYM_FUNC && existing->kind == SYM_FUNC) {
            return existing;
        }
        mcc_error_at(symtab->ctx, loc, "Redefinition of '%s'", name);
        mcc_note(symtab->ctx, "Previous definition at %s:%d",
                 existing->location.filename, existing->location.line);
        return NULL;
    }
    
    mcc_symbol_t *sym = mcc_alloc(symtab->ctx, sizeof(mcc_symbol_t));
    sym->kind = kind;
    sym->name = mcc_strdup(symtab->ctx, name);
    sym->type = type;
    sym->location = loc;
    
    /* Assign storage */
    if (kind == SYM_VAR || kind == SYM_PARAM) {
        if (mcc_symtab_is_global_scope(symtab)) {
            sym->data.global_name = sym->name;
        } else {
            /* Allocate stack slot */
            size_t size = type ? type->size : 4;
            size_t align = type ? type->align : 4;
            
            /* Align stack offset */
            symtab->current->stack_offset = 
                (symtab->current->stack_offset + align - 1) & ~(align - 1);
            sym->data.stack_offset = symtab->current->stack_offset;
            symtab->current->stack_offset += size;
        }
    } else if (kind == SYM_FUNC) {
        sym->data.global_name = sym->name;
    }
    
    /* Insert into hash table */
    unsigned h = hash_string(name) % symtab->current->table_size;
    sym->next = symtab->current->symbols[h];
    symtab->current->symbols[h] = sym;
    symtab->current->num_symbols++;
    
    return sym;
}

mcc_symbol_t *mcc_symtab_lookup(mcc_symtab_t *symtab, const char *name)
{
    for (mcc_scope_t *scope = symtab->current; scope; scope = scope->parent) {
        unsigned h = hash_string(name) % scope->table_size;
        for (mcc_symbol_t *sym = scope->symbols[h]; sym; sym = sym->next) {
            if (strcmp(sym->name, name) == 0) {
                return sym;
            }
        }
    }
    return NULL;
}

mcc_symbol_t *mcc_symtab_lookup_current(mcc_symtab_t *symtab, const char *name)
{
    unsigned h = hash_string(name) % symtab->current->table_size;
    for (mcc_symbol_t *sym = symtab->current->symbols[h]; sym; sym = sym->next) {
        if (strcmp(sym->name, name) == 0) {
            return sym;
        }
    }
    return NULL;
}

mcc_symbol_t *mcc_symtab_define_tag(mcc_symtab_t *symtab, const char *name,
                                     mcc_sym_kind_t kind, mcc_type_t *type,
                                     mcc_location_t loc)
{
    mcc_symbol_t *existing = mcc_symtab_lookup_tag_current(symtab, name);
    if (existing) {
        /* Allow incomplete type completion */
        if (!mcc_type_is_complete(existing->type) && type && mcc_type_is_complete(type)) {
            existing->type = type;
            existing->is_defined = true;
            return existing;
        }
        if (existing->is_defined) {
            mcc_error_at(symtab->ctx, loc, "Redefinition of tag '%s'", name);
            return NULL;
        }
        return existing;
    }
    
    mcc_symbol_t *sym = mcc_alloc(symtab->ctx, sizeof(mcc_symbol_t));
    sym->kind = kind;
    sym->name = mcc_strdup(symtab->ctx, name);
    sym->type = type;
    sym->location = loc;
    
    unsigned h = hash_string(name) % symtab->current->tag_table_size;
    sym->next = symtab->current->tags[h];
    symtab->current->tags[h] = sym;
    symtab->current->num_tags++;
    
    return sym;
}

mcc_symbol_t *mcc_symtab_lookup_tag(mcc_symtab_t *symtab, const char *name)
{
    for (mcc_scope_t *scope = symtab->current; scope; scope = scope->parent) {
        unsigned h = hash_string(name) % scope->tag_table_size;
        for (mcc_symbol_t *sym = scope->tags[h]; sym; sym = sym->next) {
            if (strcmp(sym->name, name) == 0) {
                return sym;
            }
        }
    }
    return NULL;
}

mcc_symbol_t *mcc_symtab_lookup_tag_current(mcc_symtab_t *symtab, const char *name)
{
    unsigned h = hash_string(name) % symtab->current->tag_table_size;
    for (mcc_symbol_t *sym = symtab->current->tags[h]; sym; sym = sym->next) {
        if (strcmp(sym->name, name) == 0) {
            return sym;
        }
    }
    return NULL;
}

mcc_symbol_t *mcc_symtab_define_label(mcc_symtab_t *symtab, const char *name,
                                       mcc_location_t loc)
{
    /* Find function scope */
    mcc_scope_t *func_scope = symtab->current;
    while (func_scope && !func_scope->is_function_scope) {
        func_scope = func_scope->parent;
    }
    
    if (!func_scope) {
        mcc_error_at(symtab->ctx, loc, "Label '%s' outside of function", name);
        return NULL;
    }
    
    /* Check for redefinition */
    unsigned h = hash_string(name) % func_scope->label_table_size;
    for (mcc_symbol_t *sym = func_scope->labels[h]; sym; sym = sym->next) {
        if (strcmp(sym->name, name) == 0) {
            if (sym->is_defined) {
                mcc_error_at(symtab->ctx, loc, "Redefinition of label '%s'", name);
                return NULL;
            }
            sym->is_defined = true;
            sym->location = loc;
            return sym;
        }
    }
    
    mcc_symbol_t *sym = mcc_alloc(symtab->ctx, sizeof(mcc_symbol_t));
    sym->kind = SYM_LABEL;
    sym->name = mcc_strdup(symtab->ctx, name);
    sym->location = loc;
    sym->is_defined = true;
    
    sym->next = func_scope->labels[h];
    func_scope->labels[h] = sym;
    func_scope->num_labels++;
    
    return sym;
}

mcc_symbol_t *mcc_symtab_lookup_label(mcc_symtab_t *symtab, const char *name)
{
    /* Find function scope */
    mcc_scope_t *func_scope = symtab->current;
    while (func_scope && !func_scope->is_function_scope) {
        func_scope = func_scope->parent;
    }
    
    if (!func_scope || !func_scope->labels) {
        return NULL;
    }
    
    unsigned h = hash_string(name) % func_scope->label_table_size;
    for (mcc_symbol_t *sym = func_scope->labels[h]; sym; sym = sym->next) {
        if (strcmp(sym->name, name) == 0) {
            return sym;
        }
    }
    
    /* Create forward reference */
    mcc_symbol_t *sym = mcc_alloc(symtab->ctx, sizeof(mcc_symbol_t));
    sym->kind = SYM_LABEL;
    sym->name = mcc_strdup(symtab->ctx, name);
    sym->is_defined = false;
    
    sym->next = func_scope->labels[h];
    func_scope->labels[h] = sym;
    func_scope->num_labels++;
    
    return sym;
}

bool mcc_symtab_is_typedef(mcc_symtab_t *symtab, const char *name)
{
    mcc_symbol_t *sym = mcc_symtab_lookup(symtab, name);
    return sym && sym->kind == SYM_TYPEDEF;
}

const char *mcc_sym_kind_name(mcc_sym_kind_t kind)
{
    if (kind < SYM_COUNT) {
        return sym_kind_names[kind];
    }
    return "unknown";
}
