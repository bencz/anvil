/*
 * MCC - Micro C Compiler
 * Semantic Analysis Dump Module
 * 
 * This file contains functions to dump semantic analysis information
 * by traversing the AST and extracting detailed information about
 * declarations, types, symbols, and scopes.
 */

#include "sema_internal.h"

/* Forward declarations */
static void dump_ast_node_with_sema(mcc_ast_node_t *node, mcc_sema_t *sema, FILE *out, int indent);
static void dump_type_detailed(mcc_type_t *type, FILE *out, int indent);

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

/* Attribute kind names */
static const char *attr_kind_names[] = {
    [MCC_ATTR_NONE]         = "none",
    [MCC_ATTR_DEPRECATED]   = "deprecated",
    [MCC_ATTR_FALLTHROUGH]  = "fallthrough",
    [MCC_ATTR_NODISCARD]    = "nodiscard",
    [MCC_ATTR_MAYBE_UNUSED] = "maybe_unused",
    [MCC_ATTR_NORETURN]     = "noreturn",
    [MCC_ATTR_UNSEQUENCED]  = "unsequenced",
    [MCC_ATTR_REPRODUCIBLE] = "reproducible",
    [MCC_ATTR_GNU_PACKED]   = "packed",
    [MCC_ATTR_GNU_ALIGNED]  = "aligned",
    [MCC_ATTR_GNU_PURE]     = "pure",
    [MCC_ATTR_GNU_CONST]    = "const",
    [MCC_ATTR_GNU_UNUSED]   = "unused",
    [MCC_ATTR_UNKNOWN]      = "unknown",
};

/*
 * Dump attributes list
 */
static void dump_attributes(mcc_attribute_t *attrs, FILE *out, int indent)
{
    if (!attrs) return;
    
    print_indent(out, indent);
    fprintf(out, "Attributes:\n");
    
    for (mcc_attribute_t *attr = attrs; attr; attr = attr->next) {
        print_indent(out, indent + 1);
        fprintf(out, "[[%s]]", attr->name ? attr->name : attr_kind_names[attr->kind]);
        
        /* Show message for deprecated/nodiscard */
        if (attr->message) {
            fprintf(out, " \"%s\"", attr->message);
        }
        
        /* Show alignment for aligned attribute */
        if (attr->kind == MCC_ATTR_GNU_ALIGNED && attr->alignment > 0) {
            fprintf(out, "(%d)", attr->alignment);
        }
        
        fprintf(out, "\n");
    }
}

/*
 * Dump type string
 */
static void dump_type_str(mcc_type_t *type, FILE *out)
{
    if (!type) {
        fprintf(out, "<null>");
        return;
    }
    fprintf(out, "'%s'", mcc_type_to_string(type));
}

/*
 * Dump array type details
 */
static void dump_array_info(mcc_type_t *type, FILE *out)
{
    if (!type || type->kind != TYPE_ARRAY) return;
    
    fprintf(out, " [");
    if (type->data.array.is_vla) {
        fprintf(out, "VLA");
    } else if (type->data.array.is_flexible) {
        fprintf(out, "flexible");
    } else if (type->data.array.length > 0) {
        fprintf(out, "%zu elements", type->data.array.length);
    } else {
        fprintf(out, "incomplete");
    }
    if (type->size > 0) {
        fprintf(out, ", %zu bytes", type->size);
    }
    fprintf(out, "]");
}

/*
 * Dump detailed type information including struct fields
 */
static void dump_type_detailed(mcc_type_t *type, FILE *out, int indent)
{
    if (!type) return;
    
    switch (type->kind) {
        case TYPE_STRUCT:
        case TYPE_UNION:
            /* Show size/align of struct/union */
            if (type->size > 0) {
                print_indent(out, indent);
                fprintf(out, "Size: %zu bytes, Align: %zu\n", type->size, type->align);
            }
            if (type->data.record.fields && type->data.record.num_fields > 0) {
                print_indent(out, indent);
                fprintf(out, "Fields:\n");
                for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
                    print_indent(out, indent + 1);
                    if (f->name) {
                        fprintf(out, "'%s' ", f->name);
                    } else {
                        fprintf(out, "(anonymous) ");
                    }
                    dump_type_str(f->type, out);
                    fprintf(out, " [offset: %d", f->offset);
                    if (f->bitfield_width > 0) {
                        fprintf(out, ", bits: %d", f->bitfield_width);
                    }
                    if (f->type && f->type->size > 0) {
                        fprintf(out, ", size: %zu", f->type->size);
                    }
                    fprintf(out, "]\n");
                }
            }
            break;
            
        case TYPE_ENUM:
            if (type->data.enumeration.constants) {
                print_indent(out, indent);
                fprintf(out, "Constants:\n");
                for (mcc_enum_const_t *c = type->data.enumeration.constants; c; c = c->next) {
                    print_indent(out, indent + 1);
                    fprintf(out, "'%s' = %lld\n", c->name, (long long)c->value);
                }
            }
            break;
            
        case TYPE_FUNCTION:
            if (type->data.function.params && type->data.function.num_params > 0) {
                print_indent(out, indent);
                fprintf(out, "Parameters: %d\n", type->data.function.num_params);
                int i = 0;
                for (mcc_func_param_t *p = type->data.function.params; p; p = p->next, i++) {
                    print_indent(out, indent + 1);
                    fprintf(out, "[%d] ", i);
                    if (p->name) fprintf(out, "'%s' ", p->name);
                    dump_type_str(p->type, out);
                    fprintf(out, "\n");
                }
            }
            if (type->data.function.is_variadic) {
                print_indent(out, indent);
                fprintf(out, "Variadic: yes\n");
            }
            break;
            
        default:
            break;
    }
}

/*
 * Dump a variable declaration
 */
static void dump_var_decl(mcc_ast_node_t *node, FILE *out, int indent)
{
    print_indent(out, indent);
    fprintf(out, "Variable '%s'", node->data.var_decl.name);
    
    if (node->data.var_decl.var_type) {
        fprintf(out, " ");
        dump_type_str(node->data.var_decl.var_type, out);
        mcc_type_t *vt = node->data.var_decl.var_type;
        /* Show array details */
        if (vt->kind == TYPE_ARRAY) {
            dump_array_info(vt, out);
        }
        /* Show size for struct/union */
        else if (vt->size > 0 && (vt->kind == TYPE_STRUCT || vt->kind == TYPE_UNION)) {
            fprintf(out, " [%zu bytes]", vt->size);
        }
    }
    
    /* Storage/qualifiers */
    if (node->data.var_decl.is_static) fprintf(out, " static");
    if (node->data.var_decl.is_extern) fprintf(out, " extern");
    if (node->data.var_decl.is_const) fprintf(out, " const");
    if (node->data.var_decl.is_volatile) fprintf(out, " volatile");
    
    /* Location */
    if (node->location.filename) {
        fprintf(out, " <%s:%d:%d>", node->location.filename, 
                node->location.line, node->location.column);
    }
    
    /* Has initializer? */
    if (node->data.var_decl.init) {
        fprintf(out, " initialized");
    }
    
    fprintf(out, "\n");
    
    /* Attributes */
    if (node->data.var_decl.attrs) {
        dump_attributes(node->data.var_decl.attrs, out, indent + 1);
    }
}

/*
 * Dump a parameter declaration
 */
static void dump_param_decl(mcc_ast_node_t *node, FILE *out, int indent, int param_index)
{
    print_indent(out, indent);
    fprintf(out, "Parameter [%d]", param_index);
    
    if (node->data.param_decl.name) {
        fprintf(out, " '%s'", node->data.param_decl.name);
    } else {
        fprintf(out, " (unnamed)");
    }
    
    if (node->data.param_decl.param_type) {
        fprintf(out, " ");
        dump_type_str(node->data.param_decl.param_type, out);
    }
    
    if (node->location.filename) {
        fprintf(out, " <%s:%d:%d>", node->location.filename,
                node->location.line, node->location.column);
    }
    
    fprintf(out, "\n");
}

/*
 * Dump a function declaration with all details
 */
static void dump_func_decl(mcc_ast_node_t *node, mcc_sema_t *sema, FILE *out, int indent)
{
    print_indent(out, indent);
    fprintf(out, "Function '%s'", node->data.func_decl.name);
    
    if (node->data.func_decl.func_type) {
        fprintf(out, " ");
        dump_type_str(node->data.func_decl.func_type, out);
    }
    
    /* Flags */
    if (node->data.func_decl.is_definition) fprintf(out, " definition");
    else fprintf(out, " declaration");
    if (node->data.func_decl.is_static) fprintf(out, " static");
    if (node->data.func_decl.is_inline) fprintf(out, " inline");
    if (node->data.func_decl.is_noreturn) fprintf(out, " _Noreturn");
    if (node->data.func_decl.is_variadic) fprintf(out, " variadic");
    
    if (node->location.filename) {
        fprintf(out, " <%s:%d:%d>", node->location.filename,
                node->location.line, node->location.column);
    }
    
    fprintf(out, "\n");
    
    /* Attributes */
    if (node->data.func_decl.attrs) {
        dump_attributes(node->data.func_decl.attrs, out, indent + 1);
    }
    
    /* Parameters */
    if (node->data.func_decl.num_params > 0) {
        print_indent(out, indent + 1);
        fprintf(out, "Parameters: (%zu)\n", node->data.func_decl.num_params);
        for (size_t i = 0; i < node->data.func_decl.num_params; i++) {
            dump_param_decl(node->data.func_decl.params[i], out, indent + 2, (int)i);
        }
    }
    
    /* Local variables (scan the body) */
    if (node->data.func_decl.body && node->data.func_decl.is_definition) {
        print_indent(out, indent + 1);
        fprintf(out, "Body:\n");
        dump_ast_node_with_sema(node->data.func_decl.body, sema, out, indent + 2);
    }
}

/*
 * Dump a struct/union declaration
 */
static void dump_struct_decl(mcc_ast_node_t *node, FILE *out, int indent, bool is_union)
{
    print_indent(out, indent);
    fprintf(out, "%s", is_union ? "Union" : "Struct");
    
    if (node->data.struct_decl.tag) {
        fprintf(out, " '%s'", node->data.struct_decl.tag);
    } else {
        fprintf(out, " (anonymous)");
    }
    
    if (node->data.struct_decl.is_definition) fprintf(out, " definition");
    else fprintf(out, " forward");
    
    if (node->location.filename) {
        fprintf(out, " <%s:%d:%d>", node->location.filename,
                node->location.line, node->location.column);
    }
    
    fprintf(out, "\n");
    
    /* Fields from struct_type in AST node */
    if (node->data.struct_decl.is_definition && node->data.struct_decl.struct_type) {
        mcc_type_t *stype = node->data.struct_decl.struct_type;
        if ((stype->kind == TYPE_STRUCT || stype->kind == TYPE_UNION) && 
            stype->data.record.fields && stype->data.record.num_fields > 0) {
            print_indent(out, indent + 1);
            fprintf(out, "Fields: (%d)\n", stype->data.record.num_fields);
            for (mcc_struct_field_t *f = stype->data.record.fields; f; f = f->next) {
                print_indent(out, indent + 2);
                if (f->name) {
                    fprintf(out, "'%s'", f->name);
                } else {
                    fprintf(out, "(anonymous)");
                }
                fprintf(out, " ");
                dump_type_str(f->type, out);
                fprintf(out, " [offset: %d", f->offset);
                if (f->bitfield_width > 0) {
                    fprintf(out, ", bits: %d", f->bitfield_width);
                }
                fprintf(out, "]\n");
            }
        }
    }
}

/*
 * Dump an enum declaration
 */
static void dump_enum_decl(mcc_ast_node_t *node, FILE *out, int indent)
{
    print_indent(out, indent);
    fprintf(out, "Enum");
    
    if (node->data.enum_decl.tag) {
        fprintf(out, " '%s'", node->data.enum_decl.tag);
    } else {
        fprintf(out, " (anonymous)");
    }
    
    if (node->data.enum_decl.is_definition) fprintf(out, " definition");
    else fprintf(out, " forward");
    
    if (node->location.filename) {
        fprintf(out, " <%s:%d:%d>", node->location.filename,
                node->location.line, node->location.column);
    }
    
    fprintf(out, "\n");
    
    /* Enumerators from AST */
    if (node->data.enum_decl.enumerators && node->data.enum_decl.num_enumerators > 0) {
        print_indent(out, indent + 1);
        fprintf(out, "Constants: (%zu)\n", node->data.enum_decl.num_enumerators);
        for (size_t i = 0; i < node->data.enum_decl.num_enumerators; i++) {
            mcc_ast_node_t *e = node->data.enum_decl.enumerators[i];
            print_indent(out, indent + 2);
            fprintf(out, "'%s' = %d\n", e->data.enumerator.name, 
                    e->data.enumerator.resolved_value);
        }
    } else if (node->data.enum_decl.enum_type) {
        /* Get from type */
        dump_type_detailed(node->data.enum_decl.enum_type, out, indent + 1);
    }
}

/*
 * Dump a typedef declaration
 */
static void dump_typedef_decl(mcc_ast_node_t *node, FILE *out, int indent)
{
    print_indent(out, indent);
    fprintf(out, "Typedef '%s'", node->data.typedef_decl.name);
    
    if (node->data.typedef_decl.type) {
        fprintf(out, " -> ");
        dump_type_str(node->data.typedef_decl.type, out);
    }
    
    if (node->location.filename) {
        fprintf(out, " <%s:%d:%d>", node->location.filename,
                node->location.line, node->location.column);
    }
    
    fprintf(out, "\n");
}

/*
 * Recursively dump AST nodes looking for declarations (with sema context)
 */
static void dump_ast_node_with_sema(mcc_ast_node_t *node, mcc_sema_t *sema, FILE *out, int indent)
{
    if (!node) return;
    
    switch (node->kind) {
        case AST_TRANSLATION_UNIT:
            for (size_t i = 0; i < node->data.translation_unit.num_decls; i++) {
                dump_ast_node_with_sema(node->data.translation_unit.decls[i], sema, out, indent);
            }
            break;
            
        case AST_FUNC_DECL:
            dump_func_decl(node, sema, out, indent);
            fprintf(out, "\n");
            break;
            
        case AST_VAR_DECL:
            dump_var_decl(node, out, indent);
            break;
            
        case AST_TYPEDEF_DECL:
            dump_typedef_decl(node, out, indent);
            break;
            
        case AST_STRUCT_DECL:
            dump_struct_decl(node, out, indent, false);
            fprintf(out, "\n");
            break;
            
        case AST_UNION_DECL:
            dump_struct_decl(node, out, indent, true);
            fprintf(out, "\n");
            break;
            
        case AST_ENUM_DECL:
            dump_enum_decl(node, out, indent);
            fprintf(out, "\n");
            break;
            
        case AST_DECL_LIST:
            for (size_t i = 0; i < node->data.decl_list.num_decls; i++) {
                dump_ast_node_with_sema(node->data.decl_list.decls[i], sema, out, indent);
            }
            break;
            
        case AST_COMPOUND_STMT:
            print_indent(out, indent);
            fprintf(out, "Block:\n");
            for (size_t i = 0; i < node->data.compound_stmt.num_stmts; i++) {
                dump_ast_node_with_sema(node->data.compound_stmt.stmts[i], sema, out, indent + 1);
            }
            break;
            
        case AST_FOR_STMT:
            /* Check for init declaration */
            if (node->data.for_stmt.init_decl) {
                print_indent(out, indent);
                fprintf(out, "For-init:\n");
                dump_ast_node_with_sema(node->data.for_stmt.init_decl, sema, out, indent + 1);
            }
            if (node->data.for_stmt.body) {
                dump_ast_node_with_sema(node->data.for_stmt.body, sema, out, indent);
            }
            break;
            
        case AST_IF_STMT:
            if (node->data.if_stmt.then_stmt) {
                dump_ast_node_with_sema(node->data.if_stmt.then_stmt, sema, out, indent);
            }
            if (node->data.if_stmt.else_stmt) {
                dump_ast_node_with_sema(node->data.if_stmt.else_stmt, sema, out, indent);
            }
            break;
            
        case AST_WHILE_STMT:
            if (node->data.while_stmt.body) {
                dump_ast_node_with_sema(node->data.while_stmt.body, sema, out, indent);
            }
            break;
            
        case AST_DO_STMT:
            if (node->data.do_stmt.body) {
                dump_ast_node_with_sema(node->data.do_stmt.body, sema, out, indent);
            }
            break;
            
        case AST_SWITCH_STMT:
            if (node->data.switch_stmt.body) {
                dump_ast_node_with_sema(node->data.switch_stmt.body, sema, out, indent);
            }
            break;
            
        case AST_CASE_STMT:
            if (node->data.case_stmt.stmt) {
                dump_ast_node_with_sema(node->data.case_stmt.stmt, sema, out, indent);
            }
            break;
            
        case AST_DEFAULT_STMT:
            if (node->data.default_stmt.stmt) {
                dump_ast_node_with_sema(node->data.default_stmt.stmt, sema, out, indent);
            }
            break;
            
        case AST_LABEL_STMT:
            print_indent(out, indent);
            fprintf(out, "Label '%s'\n", node->data.label_stmt.label);
            if (node->data.label_stmt.stmt) {
                dump_ast_node_with_sema(node->data.label_stmt.stmt, sema, out, indent);
            }
            break;
            
        default:
            /* Skip other node types */
            break;
    }
}

/*
 * Dump complete type information
 */
static void dump_type_full(mcc_type_t *type, FILE *out, int indent)
{
    if (!type) {
        print_indent(out, indent);
        fprintf(out, "<null type>\n");
        return;
    }
    
    print_indent(out, indent);
    fprintf(out, "Type: %s", mcc_type_to_string(type));
    
    /* Qualifiers */
    if (type->qualifiers) {
        fprintf(out, " [");
        if (type->qualifiers & QUAL_CONST) fprintf(out, "const ");
        if (type->qualifiers & QUAL_VOLATILE) fprintf(out, "volatile ");
        if (type->qualifiers & QUAL_RESTRICT) fprintf(out, "restrict ");
        if (type->qualifiers & QUAL_ATOMIC) fprintf(out, "_Atomic ");
        fprintf(out, "]");
    }
    
    /* Unsigned flag */
    if (type->is_unsigned) fprintf(out, " unsigned");
    
    /* Function specifiers */
    if (type->is_inline) fprintf(out, " inline");
    if (type->is_noreturn) fprintf(out, " _Noreturn");
    
    /* Size/align */
    if (type->size > 0 || type->align > 0) {
        fprintf(out, " (size=%zu, align=%zu)", type->size, type->align);
    }
    
    fprintf(out, "\n");
    
    /* Type-specific details */
    switch (type->kind) {
        case TYPE_POINTER:
            print_indent(out, indent + 1);
            fprintf(out, "Pointee: %s\n", mcc_type_to_string(type->data.pointer.pointee));
            break;
            
        case TYPE_ARRAY:
            print_indent(out, indent + 1);
            fprintf(out, "Element: %s\n", mcc_type_to_string(type->data.array.element));
            print_indent(out, indent + 1);
            fprintf(out, "Length: %zu", type->data.array.length);
            if (type->data.array.is_vla) fprintf(out, " (VLA)");
            if (type->data.array.is_flexible) fprintf(out, " (flexible)");
            fprintf(out, "\n");
            break;
            
        case TYPE_FUNCTION:
            print_indent(out, indent + 1);
            fprintf(out, "Return: %s\n", mcc_type_to_string(type->data.function.return_type));
            print_indent(out, indent + 1);
            fprintf(out, "Params: %d", type->data.function.num_params);
            if (type->data.function.is_variadic) fprintf(out, " (variadic)");
            if (type->data.function.is_oldstyle) fprintf(out, " (K&R)");
            fprintf(out, "\n");
            if (type->data.function.params) {
                int i = 0;
                for (mcc_func_param_t *p = type->data.function.params; p; p = p->next, i++) {
                    print_indent(out, indent + 2);
                    fprintf(out, "[%d] %s: %s\n", i, 
                            p->name ? p->name : "(unnamed)",
                            mcc_type_to_string(p->type));
                }
            }
            break;
            
        case TYPE_STRUCT:
        case TYPE_UNION:
            print_indent(out, indent + 1);
            fprintf(out, "Tag: %s\n", type->data.record.tag ? type->data.record.tag : "(anonymous)");
            print_indent(out, indent + 1);
            fprintf(out, "Complete: %s\n", type->data.record.is_complete ? "yes" : "no");
            print_indent(out, indent + 1);
            fprintf(out, "Fields: %d\n", type->data.record.num_fields);
            if (type->data.record.fields) {
                for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
                    print_indent(out, indent + 2);
                    fprintf(out, "%s: %s [offset=%d",
                            f->name ? f->name : "(anon)",
                            mcc_type_to_string(f->type),
                            f->offset);
                    if (f->bitfield_width > 0) {
                        fprintf(out, ", bits=%d", f->bitfield_width);
                    }
                    fprintf(out, "]\n");
                }
            }
            break;
            
        case TYPE_ENUM:
            print_indent(out, indent + 1);
            fprintf(out, "Tag: %s\n", type->data.enumeration.tag ? type->data.enumeration.tag : "(anonymous)");
            print_indent(out, indent + 1);
            fprintf(out, "Complete: %s\n", type->data.enumeration.is_complete ? "yes" : "no");
            print_indent(out, indent + 1);
            fprintf(out, "Constants: %d\n", type->data.enumeration.num_constants);
            if (type->data.enumeration.constants) {
                for (mcc_enum_const_t *c = type->data.enumeration.constants; c; c = c->next) {
                    print_indent(out, indent + 2);
                    fprintf(out, "%s = %lld\n", c->name, (long long)c->value);
                }
            }
            break;
            
        default:
            break;
    }
}

/*
 * Dump complete symbol information
 */
static void dump_symbol_full(mcc_symbol_t *sym, FILE *out, int indent)
{
    print_indent(out, indent);
    
    const char *kind_name = (sym->kind < SYM_COUNT) ? sym_kind_names[sym->kind] : "Unknown";
    fprintf(out, "%s '%s'\n", kind_name, sym->name);
    
    /* Location */
    if (sym->location.filename) {
        print_indent(out, indent + 1);
        fprintf(out, "Location: %s:%d:%d\n", sym->location.filename,
                sym->location.line, sym->location.column);
    }
    
    /* Storage class */
    print_indent(out, indent + 1);
    fprintf(out, "Storage: %s\n", 
            (sym->storage < 7) ? storage_names[sym->storage] : "unknown");
    
    /* Flags */
    print_indent(out, indent + 1);
    fprintf(out, "Flags:");
    if (sym->is_defined) fprintf(out, " defined");
    if (sym->is_used) fprintf(out, " used");
    if (sym->is_parameter) fprintf(out, " parameter");
    if (!sym->is_defined && !sym->is_used && !sym->is_parameter) fprintf(out, " (none)");
    fprintf(out, "\n");
    
    /* Symbol-specific data */
    switch (sym->kind) {
        case SYM_VAR:
        case SYM_PARAM:
            print_indent(out, indent + 1);
            if (sym->storage == STORAGE_EXTERN || sym->storage == STORAGE_STATIC ||
                (sym->kind == SYM_VAR && sym->data.global_name)) {
                fprintf(out, "Global name: %s\n", sym->data.global_name ? sym->data.global_name : "(null)");
            } else {
                fprintf(out, "Stack offset: %d\n", sym->data.stack_offset);
            }
            break;
            
        case SYM_FUNC:
            print_indent(out, indent + 1);
            fprintf(out, "Global name: %s\n", sym->data.global_name ? sym->data.global_name : "(null)");
            if (sym->ast_node) {
                print_indent(out, indent + 1);
                fprintf(out, "Has AST node: yes\n");
            }
            break;
            
        case SYM_ENUM_CONST:
            print_indent(out, indent + 1);
            fprintf(out, "Value: %d\n", sym->data.enum_value);
            break;
            
        default:
            break;
    }
    
    /* Type */
    if (sym->type) {
        dump_type_full(sym->type, out, indent + 1);
    }
}

/*
 * Dump symbol from symbol table (brief version)
 */
static void dump_symbol(mcc_symbol_t *sym, FILE *out, int indent)
{
    print_indent(out, indent);
    
    const char *kind_name = (sym->kind < SYM_COUNT) ? sym_kind_names[sym->kind] : "Unknown";
    fprintf(out, "%s '%s'", kind_name, sym->name);
    
    if (sym->type) {
        fprintf(out, " ");
        dump_type_str(sym->type, out);
    }
    
    if (sym->storage != STORAGE_NONE && sym->storage < 7) {
        fprintf(out, " %s", storage_names[sym->storage]);
    }
    
    if (sym->is_defined) fprintf(out, " defined");
    if (sym->is_used) fprintf(out, " used");
    if (sym->is_parameter) fprintf(out, " param");
    
    if (sym->location.filename) {
        fprintf(out, " <%s:%d:%d>", sym->location.filename,
                sym->location.line, sym->location.column);
    }
    
    if (sym->kind == SYM_ENUM_CONST) {
        fprintf(out, " = %d", sym->data.enum_value);
    }
    
    
    fprintf(out, "\n");
}

/*
 * Dump global scope symbols
 */
static void dump_global_scope(mcc_sema_t *sema, FILE *out)
{
    mcc_scope_t *global = sema->symtab->global;
    if (!global) return;
    
    fprintf(out, "=== Global Scope ===\n\n");
    
    /* Functions */
    fprintf(out, "Functions:\n");
    for (size_t i = 0; i < global->table_size; i++) {
        for (mcc_symbol_t *sym = global->symbols[i]; sym; sym = sym->next) {
            if (sym->kind == SYM_FUNC) {
                dump_symbol(sym, out, 1);
            }
        }
    }
    fprintf(out, "\n");
    
    /* Global variables */
    fprintf(out, "Global Variables:\n");
    for (size_t i = 0; i < global->table_size; i++) {
        for (mcc_symbol_t *sym = global->symbols[i]; sym; sym = sym->next) {
            if (sym->kind == SYM_VAR) {
                dump_symbol(sym, out, 1);
            }
        }
    }
    fprintf(out, "\n");
    
    /* Typedefs */
    fprintf(out, "Typedefs:\n");
    for (size_t i = 0; i < global->table_size; i++) {
        for (mcc_symbol_t *sym = global->symbols[i]; sym; sym = sym->next) {
            if (sym->kind == SYM_TYPEDEF) {
                dump_symbol(sym, out, 1);
            }
        }
    }
    fprintf(out, "\n");
    
    /* Enum constants */
    fprintf(out, "Enum Constants:\n");
    for (size_t i = 0; i < global->table_size; i++) {
        for (mcc_symbol_t *sym = global->symbols[i]; sym; sym = sym->next) {
            if (sym->kind == SYM_ENUM_CONST) {
                dump_symbol(sym, out, 1);
            }
        }
    }
    fprintf(out, "\n");
    
    /* Tags */
    if (global->num_tags > 0) {
        fprintf(out, "Tags (struct/union/enum):\n");
        for (size_t i = 0; i < global->tag_table_size; i++) {
            for (mcc_symbol_t *sym = global->tags[i]; sym; sym = sym->next) {
                dump_symbol(sym, out, 1);
                /* Show fields/constants for complete types */
                if (sym->type) {
                    dump_type_detailed(sym->type, out, 2);
                }
            }
        }
        fprintf(out, "\n");
    }
}

/*
 * Main dump function - dumps everything
 */
void mcc_sema_dump(mcc_sema_t *sema, FILE *out)
{
    if (!sema) {
        fprintf(out, "(null sema)\n");
        return;
    }
    
    fprintf(out, "=== Semantic Analysis Dump ===\n\n");
    
    /* C Standard info */
    mcc_c_std_t std = mcc_ctx_get_std(sema->ctx);
    const mcc_c_std_info_t *std_info = mcc_c_std_get_info(std);
    fprintf(out, "C Standard: %s", std_info ? std_info->name : "unknown");
    if (std_info && std_info->iso_name) {
        fprintf(out, " (%s)", std_info->iso_name);
    }
    fprintf(out, "\n\n");
    
    /* Dump global scope from symbol table */
    dump_global_scope(sema, out);
}

/*
 * Dump statistics and warnings
 */
static void dump_statistics(mcc_sema_t *sema, FILE *out)
{
    mcc_scope_t *global = sema->symtab->global;
    if (!global) return;
    
    /* Count symbols by kind */
    int num_funcs = 0, num_vars = 0, num_typedefs = 0, num_enums = 0;
    int num_funcs_defined = 0, num_funcs_used = 0;
    int num_vars_used = 0;
    
    for (size_t i = 0; i < global->table_size; i++) {
        for (mcc_symbol_t *sym = global->symbols[i]; sym; sym = sym->next) {
            switch (sym->kind) {
                case SYM_FUNC:
                    num_funcs++;
                    if (sym->is_defined) num_funcs_defined++;
                    if (sym->is_used) num_funcs_used++;
                    break;
                case SYM_VAR:
                    num_vars++;
                    if (sym->is_used) num_vars_used++;
                    break;
                case SYM_TYPEDEF:
                    num_typedefs++;
                    break;
                case SYM_ENUM_CONST:
                    num_enums++;
                    break;
                default:
                    break;
            }
        }
    }
    
    fprintf(out, "=== Statistics ===\n\n");
    fprintf(out, "Functions: %d total, %d defined, %d used\n", num_funcs, num_funcs_defined, num_funcs_used);
    fprintf(out, "Global Variables: %d total, %d used\n", num_vars, num_vars_used);
    fprintf(out, "Typedefs: %d\n", num_typedefs);
    fprintf(out, "Enum Constants: %d\n", num_enums);
    fprintf(out, "Tags: %zu\n", global->num_tags);
    fprintf(out, "\n");
    
    /* Warnings: unused symbols */
    bool has_unused = false;
    for (size_t i = 0; i < global->table_size; i++) {
        for (mcc_symbol_t *sym = global->symbols[i]; sym; sym = sym->next) {
            if (!sym->is_used && sym->is_defined) {
                if (sym->kind == SYM_FUNC && strcmp(sym->name, "main") != 0) {
                    if (!has_unused) {
                        fprintf(out, "=== Warnings: Unused Symbols ===\n\n");
                        has_unused = true;
                    }
                    fprintf(out, "  Function '%s' defined but not used <%s:%d>\n",
                            sym->name, sym->location.filename, sym->location.line);
                } else if (sym->kind == SYM_VAR) {
                    if (!has_unused) {
                        fprintf(out, "=== Warnings: Unused Symbols ===\n\n");
                        has_unused = true;
                    }
                    fprintf(out, "  Variable '%s' defined but not used <%s:%d>\n",
                            sym->name, sym->location.filename, sym->location.line);
                }
            }
        }
    }
    if (has_unused) fprintf(out, "\n");
}

/*
 * Dump type context information
 */
static void dump_type_context(mcc_sema_t *sema, FILE *out)
{
    mcc_type_context_t *types = sema->types;
    if (!types) return;
    
    fprintf(out, "=== Type Context ===\n\n");
    
    fprintf(out, "Pointer size: %d bytes\n", types->ptr_size);
    fprintf(out, "\n");
    
    /* Primitive types info */
    fprintf(out, "Primitive Types:\n");
    if (types->type_char) fprintf(out, "  char:        size=%zu, align=%zu\n", types->type_char->size, types->type_char->align);
    if (types->type_short) fprintf(out, "  short:       size=%zu, align=%zu\n", types->type_short->size, types->type_short->align);
    if (types->type_int) fprintf(out, "  int:         size=%zu, align=%zu\n", types->type_int->size, types->type_int->align);
    if (types->type_long) fprintf(out, "  long:        size=%zu, align=%zu\n", types->type_long->size, types->type_long->align);
    if (types->type_llong) fprintf(out, "  long long:   size=%zu, align=%zu\n", types->type_llong->size, types->type_llong->align);
    if (types->type_float) fprintf(out, "  float:       size=%zu, align=%zu\n", types->type_float->size, types->type_float->align);
    if (types->type_double) fprintf(out, "  double:      size=%zu, align=%zu\n", types->type_double->size, types->type_double->align);
    fprintf(out, "\n");
}

/*
 * Dump all symbols in a scope (detailed)
 */
static void dump_scope_full(mcc_scope_t *scope, FILE *out, int indent, const char *name)
{
    print_indent(out, indent);
    fprintf(out, "Scope: %s (depth=%d", name, scope->depth);
    if (scope->is_file_scope) fprintf(out, ", file");
    if (scope->is_function_scope) fprintf(out, ", function");
    if (scope->is_block_scope) fprintf(out, ", block");
    fprintf(out, ", stack_offset=%d)\n", scope->stack_offset);
    
    /* Symbols */
    print_indent(out, indent + 1);
    fprintf(out, "Symbols: %zu\n", scope->num_symbols);
    for (size_t i = 0; i < scope->table_size; i++) {
        for (mcc_symbol_t *sym = scope->symbols[i]; sym; sym = sym->next) {
            dump_symbol_full(sym, out, indent + 2);
        }
    }
    
    /* Tags */
    if (scope->num_tags > 0) {
        print_indent(out, indent + 1);
        fprintf(out, "Tags: %zu\n", scope->num_tags);
        for (size_t i = 0; i < scope->tag_table_size; i++) {
            for (mcc_symbol_t *sym = scope->tags[i]; sym; sym = sym->next) {
                dump_symbol_full(sym, out, indent + 2);
            }
        }
    }
    
    /* Labels */
    if (scope->num_labels > 0) {
        print_indent(out, indent + 1);
        fprintf(out, "Labels: %zu\n", scope->num_labels);
        for (size_t i = 0; i < scope->label_table_size; i++) {
            for (mcc_symbol_t *sym = scope->labels[i]; sym; sym = sym->next) {
                dump_symbol_full(sym, out, indent + 2);
            }
        }
    }
}

/*
 * Dump with full AST traversal for local variables
 */
void mcc_sema_dump_full(mcc_sema_t *sema, mcc_ast_node_t *ast, FILE *out)
{
    if (!sema) {
        fprintf(out, "(null sema)\n");
        return;
    }
    
    fprintf(out, "=== Full Semantic Analysis Dump ===\n\n");
    
    /* C Standard info */
    mcc_c_std_t std = mcc_ctx_get_std(sema->ctx);
    const mcc_c_std_info_t *std_info = mcc_c_std_get_info(std);
    fprintf(out, "C Standard: %s", std_info ? std_info->name : "unknown");
    if (std_info && std_info->iso_name) {
        fprintf(out, " (%s)", std_info->iso_name);
    }
    fprintf(out, "\n\n");
    
    /* Dump type context */
    dump_type_context(sema, out);
    
    /* Dump global scope */
    dump_global_scope(sema, out);
    
    /* Dump AST declarations with full details */
    fprintf(out, "=== Declarations (from AST) ===\n\n");
    dump_ast_node_with_sema(ast, sema, out, 0);
}

/*
 * Dump symbol table only
 */
void mcc_sema_dump_symtab(mcc_sema_t *sema, FILE *out)
{
    if (!sema || !sema->symtab) {
        fprintf(out, "(no symbol table)\n");
        return;
    }
    
    dump_global_scope(sema, out);
}

/*
 * Dump only global symbols
 */
void mcc_sema_dump_globals(mcc_sema_t *sema, FILE *out)
{
    mcc_sema_dump_symtab(sema, out);
}

/*
 * Dump EVERYTHING in verbose/detailed format
 */
void mcc_sema_dump_verbose(mcc_sema_t *sema, mcc_ast_node_t *ast, FILE *out)
{
    if (!sema) {
        fprintf(out, "(null sema)\n");
        return;
    }
    
    fprintf(out, "================================================================================\n");
    fprintf(out, "                    VERBOSE SEMANTIC ANALYSIS DUMP\n");
    fprintf(out, "================================================================================\n\n");
    
    /* C Standard info */
    mcc_c_std_t std = mcc_ctx_get_std(sema->ctx);
    const mcc_c_std_info_t *std_info = mcc_c_std_get_info(std);
    fprintf(out, "C Standard: %s", std_info ? std_info->name : "unknown");
    if (std_info && std_info->iso_name) {
        fprintf(out, " (%s)", std_info->iso_name);
    }
    fprintf(out, "\n\n");
    
    /* Type context */
    dump_type_context(sema, out);
    
    /* Global scope - detailed */
    fprintf(out, "================================================================================\n");
    fprintf(out, "                           SYMBOL TABLE (DETAILED)\n");
    fprintf(out, "================================================================================\n\n");
    
    dump_scope_full(sema->symtab->global, out, 0, "Global");
    fprintf(out, "\n");
    
    /* AST declarations */
    fprintf(out, "================================================================================\n");
    fprintf(out, "                           AST DECLARATIONS\n");
    fprintf(out, "================================================================================\n\n");
    
    dump_ast_node_with_sema(ast, sema, out, 0);
    
    fprintf(out, "\n================================================================================\n");
    fprintf(out, "                              END OF DUMP\n");
    fprintf(out, "================================================================================\n");
}
