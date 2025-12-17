/*
 * MCC - Micro C Compiler
 * Semantic Analysis Dump Module
 * 
 * This file contains functions to dump semantic analysis information
 * including the symbol table, scopes, and type information.
 */

#include "sema_internal.h"

/* Forward declarations */
static void dump_scope(mcc_scope_t *scope, FILE *out, int indent);
static void dump_type_info(mcc_type_t *type, FILE *out);

/* Symbol kind names */
static const char *sym_kind_names[] = {
    [SYM_VAR]        = "Variable",
    [SYM_FUNC]       = "Function",
    [SYM_PARAM]      = "Parameter",
    [SYM_TYPEDEF]    = "Typedef",
    [SYM_STRUCT]     = "Struct",
    [SYM_UNION]      = "Union",
    [SYM_ENUM]       = "Enum",
    [SYM_ENUM_CONST] = "EnumConst",
    [SYM_LABEL]      = "Label",
};

/* Storage class names */
static const char *storage_names[] = {
    [STORAGE_NONE]     = "",
    [STORAGE_AUTO]     = "auto",
    [STORAGE_STATIC]   = "static",
    [STORAGE_EXTERN]   = "extern",
    [STORAGE_REGISTER] = "register",
    [STORAGE_TYPEDEF]  = "typedef",
};

/*
 * Print indentation
 */
static void print_indent(FILE *out, int indent)
{
    for (int i = 0; i < indent; i++) {
        fprintf(out, "  ");
    }
}

/*
 * Dump type information
 */
static void dump_type_info(mcc_type_t *type, FILE *out)
{
    if (!type) {
        fprintf(out, "<null>");
        return;
    }
    fprintf(out, "'%s'", mcc_type_to_string(type));
}

/*
 * Dump a single symbol with scope context
 */
static void dump_symbol_in_scope(mcc_symbol_t *sym, mcc_scope_t *scope, FILE *out, int indent)
{
    print_indent(out, indent);
    
    /* Kind and name */
    const char *kind_name = (sym->kind < SYM_COUNT) ? sym_kind_names[sym->kind] : "Unknown";
    fprintf(out, "%s '%s'", kind_name, sym->name);
    
    /* Type */
    if (sym->type) {
        fprintf(out, " ");
        dump_type_info(sym->type, out);
    }
    
    /* Storage class */
    if (sym->storage != STORAGE_NONE && sym->storage < 6) {
        fprintf(out, " %s", storage_names[sym->storage]);
    }
    
    /* Flags */
    if (sym->is_defined) fprintf(out, " defined");
    if (sym->is_used) fprintf(out, " used");
    if (sym->is_parameter) fprintf(out, " param");
    
    /* Location */
    if (sym->location.filename) {
        fprintf(out, " <%s:%d:%d>",
                sym->location.filename, sym->location.line, sym->location.column);
    }
    
    /* Kind-specific data - use scope to determine if global */
    bool is_global = scope && scope->is_file_scope;
    
    switch (sym->kind) {
        case SYM_VAR:
            if (is_global || sym->storage == STORAGE_STATIC || sym->storage == STORAGE_EXTERN) {
                /* Global/static/extern variable - no stack offset */
            } else if (sym->is_parameter) {
                fprintf(out, " [param offset: %d]", sym->data.stack_offset);
            } else {
                fprintf(out, " [stack offset: %d]", sym->data.stack_offset);
            }
            break;
            
        case SYM_PARAM:
            fprintf(out, " [param offset: %d]", sym->data.stack_offset);
            break;
            
        case SYM_FUNC:
            /* Function - name is already shown */
            break;
            
        case SYM_ENUM_CONST:
            fprintf(out, " [value: %d]", sym->data.enum_value);
            break;
            
        default:
            break;
    }
    
    fprintf(out, "\n");
}

/*
 * Dump all symbols in a hash table (with scope context)
 */
static void dump_symbol_table_in_scope(mcc_symbol_t **table, size_t table_size, 
                                        size_t num_symbols, const char *name,
                                        mcc_scope_t *scope, FILE *out, int indent)
{
    if (num_symbols == 0) return;
    
    print_indent(out, indent);
    fprintf(out, "%s: (%zu symbols)\n", name, num_symbols);
    
    for (size_t i = 0; i < table_size; i++) {
        for (mcc_symbol_t *sym = table[i]; sym; sym = sym->next) {
            dump_symbol_in_scope(sym, scope, out, indent + 1);
        }
    }
}

/*
 * Dump a scope and its contents
 */
static void dump_scope(mcc_scope_t *scope, FILE *out, int indent)
{
    if (!scope) return;
    
    print_indent(out, indent);
    fprintf(out, "Scope (depth=%d", scope->depth);
    if (scope->is_file_scope) fprintf(out, ", file");
    if (scope->is_function_scope) fprintf(out, ", function");
    if (scope->is_block_scope) fprintf(out, ", block");
    fprintf(out, ", stack_offset=%d)\n", scope->stack_offset);
    
    /* Dump symbols */
    dump_symbol_table_in_scope(scope->symbols, scope->table_size, scope->num_symbols,
                               "Symbols", scope, out, indent + 1);
    
    /* Dump tags */
    dump_symbol_table_in_scope(scope->tags, scope->tag_table_size, scope->num_tags,
                               "Tags", scope, out, indent + 1);
    
    /* Dump labels (only in function scope) */
    if (scope->is_function_scope) {
        dump_symbol_table_in_scope(scope->labels, scope->label_table_size, scope->num_labels,
                                   "Labels", scope, out, indent + 1);
    }
}

/*
 * Recursively dump scope chain
 */
static void dump_scope_chain(mcc_scope_t *scope, FILE *out, int indent)
{
    if (!scope) return;
    
    /* Dump parent first (to show from global to local) */
    if (scope->parent) {
        dump_scope_chain(scope->parent, out, indent);
    }
    
    dump_scope(scope, out, indent);
}

/*
 * Dump the entire symbol table
 */
void mcc_sema_dump_symtab(mcc_sema_t *sema, FILE *out)
{
    if (!sema || !sema->symtab) {
        fprintf(out, "(no symbol table)\n");
        return;
    }
    
    fprintf(out, "=== Symbol Table Dump ===\n\n");
    
    /* Dump global scope */
    fprintf(out, "Global Scope:\n");
    dump_scope(sema->symtab->global, out, 1);
    
    /* If current scope is different from global, show the scope chain */
    if (sema->symtab->current && sema->symtab->current != sema->symtab->global) {
        fprintf(out, "\nCurrent Scope Chain:\n");
        dump_scope_chain(sema->symtab->current, out, 1);
    }
    
    fprintf(out, "\n");
}

/*
 * Dump semantic analyzer state
 */
void mcc_sema_dump(mcc_sema_t *sema, FILE *out)
{
    if (!sema) {
        fprintf(out, "(null sema)\n");
        return;
    }
    
    fprintf(out, "=== Semantic Analyzer Dump ===\n\n");
    
    /* Current function */
    fprintf(out, "Current Function: ");
    if (sema->current_func) {
        fprintf(out, "'%s'", sema->current_func->name);
        if (sema->current_return_type) {
            fprintf(out, " -> ");
            dump_type_info(sema->current_return_type, out);
        }
    } else {
        fprintf(out, "(none)");
    }
    fprintf(out, "\n");
    
    /* Loop/switch depth */
    fprintf(out, "Loop Depth: %d\n", sema->loop_depth);
    fprintf(out, "Switch Depth: %d\n", sema->switch_depth);
    
    /* C Standard info */
    mcc_c_std_t std = mcc_ctx_get_std(sema->ctx);
    const mcc_c_std_info_t *std_info = mcc_c_std_get_info(std);
    fprintf(out, "C Standard: %s", std_info ? std_info->name : "unknown");
    if (std_info && std_info->iso_name) {
        fprintf(out, " (%s)", std_info->iso_name);
    }
    fprintf(out, "\n\n");
    
    /* Dump symbol table */
    mcc_sema_dump_symtab(sema, out);
}

/*
 * Dump only global symbols (functions and global variables)
 */
void mcc_sema_dump_globals(mcc_sema_t *sema, FILE *out)
{
    if (!sema || !sema->symtab || !sema->symtab->global) {
        fprintf(out, "(no globals)\n");
        return;
    }
    
    mcc_scope_t *global = sema->symtab->global;
    
    fprintf(out, "=== Global Symbols ===\n\n");
    
    /* Count by kind */
    int num_funcs = 0, num_vars = 0, num_typedefs = 0, num_enums = 0;
    
    for (size_t i = 0; i < global->table_size; i++) {
        for (mcc_symbol_t *sym = global->symbols[i]; sym; sym = sym->next) {
            switch (sym->kind) {
                case SYM_FUNC: num_funcs++; break;
                case SYM_VAR: num_vars++; break;
                case SYM_TYPEDEF: num_typedefs++; break;
                case SYM_ENUM_CONST: num_enums++; break;
                default: break;
            }
        }
    }
    
    fprintf(out, "Summary: %d functions, %d variables, %d typedefs, %d enum constants\n\n",
            num_funcs, num_vars, num_typedefs, num_enums);
    
    /* Functions */
    if (num_funcs > 0) {
        fprintf(out, "Functions:\n");
        for (size_t i = 0; i < global->table_size; i++) {
            for (mcc_symbol_t *sym = global->symbols[i]; sym; sym = sym->next) {
                if (sym->kind == SYM_FUNC) {
                    dump_symbol_in_scope(sym, global, out, 1);
                }
            }
        }
        fprintf(out, "\n");
    }
    
    /* Global variables */
    if (num_vars > 0) {
        fprintf(out, "Global Variables:\n");
        for (size_t i = 0; i < global->table_size; i++) {
            for (mcc_symbol_t *sym = global->symbols[i]; sym; sym = sym->next) {
                if (sym->kind == SYM_VAR) {
                    dump_symbol_in_scope(sym, global, out, 1);
                }
            }
        }
        fprintf(out, "\n");
    }
    
    /* Typedefs */
    if (num_typedefs > 0) {
        fprintf(out, "Typedefs:\n");
        for (size_t i = 0; i < global->table_size; i++) {
            for (mcc_symbol_t *sym = global->symbols[i]; sym; sym = sym->next) {
                if (sym->kind == SYM_TYPEDEF) {
                    dump_symbol_in_scope(sym, global, out, 1);
                }
            }
        }
        fprintf(out, "\n");
    }
    
    /* Enum constants */
    if (num_enums > 0) {
        fprintf(out, "Enum Constants:\n");
        for (size_t i = 0; i < global->table_size; i++) {
            for (mcc_symbol_t *sym = global->symbols[i]; sym; sym = sym->next) {
                if (sym->kind == SYM_ENUM_CONST) {
                    dump_symbol_in_scope(sym, global, out, 1);
                }
            }
        }
        fprintf(out, "\n");
    }
    
    /* Tags (struct/union/enum) */
    if (global->num_tags > 0) {
        fprintf(out, "Tags (struct/union/enum):\n");
        for (size_t i = 0; i < global->tag_table_size; i++) {
            for (mcc_symbol_t *sym = global->tags[i]; sym; sym = sym->next) {
                dump_symbol_in_scope(sym, global, out, 1);
            }
        }
        fprintf(out, "\n");
    }
}
