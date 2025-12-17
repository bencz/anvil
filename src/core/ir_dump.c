/*
 * ANVIL - IR Dump/Debug Module
 * 
 * Provides functions to print IR for debugging purposes.
 * Outputs human-readable representation of modules, functions, blocks, and instructions.
 */

#include "anvil/anvil_internal.h"
#include <stdio.h>
#include <string.h>

/* Get operation name as string */
static const char *op_name(anvil_op_t op)
{
    static const char *names[] = {
        [ANVIL_OP_ADD] = "add",
        [ANVIL_OP_SUB] = "sub",
        [ANVIL_OP_MUL] = "mul",
        [ANVIL_OP_DIV] = "div",
        [ANVIL_OP_SDIV] = "sdiv",
        [ANVIL_OP_UDIV] = "udiv",
        [ANVIL_OP_MOD] = "mod",
        [ANVIL_OP_SMOD] = "smod",
        [ANVIL_OP_UMOD] = "umod",
        [ANVIL_OP_NEG] = "neg",
        [ANVIL_OP_AND] = "and",
        [ANVIL_OP_OR] = "or",
        [ANVIL_OP_XOR] = "xor",
        [ANVIL_OP_NOT] = "not",
        [ANVIL_OP_SHL] = "shl",
        [ANVIL_OP_SHR] = "shr",
        [ANVIL_OP_SAR] = "sar",
        [ANVIL_OP_CMP_EQ] = "cmp_eq",
        [ANVIL_OP_CMP_NE] = "cmp_ne",
        [ANVIL_OP_CMP_LT] = "cmp_lt",
        [ANVIL_OP_CMP_LE] = "cmp_le",
        [ANVIL_OP_CMP_GT] = "cmp_gt",
        [ANVIL_OP_CMP_GE] = "cmp_ge",
        [ANVIL_OP_CMP_ULT] = "cmp_ult",
        [ANVIL_OP_CMP_ULE] = "cmp_ule",
        [ANVIL_OP_CMP_UGT] = "cmp_ugt",
        [ANVIL_OP_CMP_UGE] = "cmp_uge",
        [ANVIL_OP_LOAD] = "load",
        [ANVIL_OP_STORE] = "store",
        [ANVIL_OP_ALLOCA] = "alloca",
        [ANVIL_OP_GEP] = "gep",
        [ANVIL_OP_STRUCT_GEP] = "struct_gep",
        [ANVIL_OP_BR] = "br",
        [ANVIL_OP_BR_COND] = "br_cond",
        [ANVIL_OP_CALL] = "call",
        [ANVIL_OP_RET] = "ret",
        [ANVIL_OP_SWITCH] = "switch",
        [ANVIL_OP_TRUNC] = "trunc",
        [ANVIL_OP_ZEXT] = "zext",
        [ANVIL_OP_SEXT] = "sext",
        [ANVIL_OP_FPTRUNC] = "fptrunc",
        [ANVIL_OP_FPEXT] = "fpext",
        [ANVIL_OP_FPTOSI] = "fptosi",
        [ANVIL_OP_FPTOUI] = "fptoui",
        [ANVIL_OP_SITOFP] = "sitofp",
        [ANVIL_OP_UITOFP] = "uitofp",
        [ANVIL_OP_PTRTOINT] = "ptrtoint",
        [ANVIL_OP_INTTOPTR] = "inttoptr",
        [ANVIL_OP_BITCAST] = "bitcast",
        [ANVIL_OP_FADD] = "fadd",
        [ANVIL_OP_FSUB] = "fsub",
        [ANVIL_OP_FMUL] = "fmul",
        [ANVIL_OP_FDIV] = "fdiv",
        [ANVIL_OP_FNEG] = "fneg",
        [ANVIL_OP_FABS] = "fabs",
        [ANVIL_OP_FCMP] = "fcmp",
        [ANVIL_OP_PHI] = "phi",
        [ANVIL_OP_SELECT] = "select",
        [ANVIL_OP_NOP] = "nop",
    };
    
    if (op >= 0 && op < ANVIL_OP_COUNT && names[op]) {
        return names[op];
    }
    return "unknown";
}

/* Get type name as string */
static const char *type_kind_name(anvil_type_kind_t kind)
{
    switch (kind) {
        case ANVIL_TYPE_VOID: return "void";
        case ANVIL_TYPE_I8: return "i8";
        case ANVIL_TYPE_I16: return "i16";
        case ANVIL_TYPE_I32: return "i32";
        case ANVIL_TYPE_I64: return "i64";
        case ANVIL_TYPE_U8: return "u8";
        case ANVIL_TYPE_U16: return "u16";
        case ANVIL_TYPE_U32: return "u32";
        case ANVIL_TYPE_U64: return "u64";
        case ANVIL_TYPE_F32: return "f32";
        case ANVIL_TYPE_F64: return "f64";
        case ANVIL_TYPE_PTR: return "ptr";
        case ANVIL_TYPE_STRUCT: return "struct";
        case ANVIL_TYPE_ARRAY: return "array";
        case ANVIL_TYPE_FUNC: return "func";
        default: return "?";
    }
}

/* Print escaped string to file */
static void print_escaped_string(FILE *out, const char *str)
{
    if (!str) {
        return;
    }
    
    while (*str) {
        unsigned char c = (unsigned char)*str;
        switch (c) {
            case '\0': fprintf(out, "\\0"); break;
            case '\a': fprintf(out, "\\a"); break;
            case '\b': fprintf(out, "\\b"); break;
            case '\t': fprintf(out, "\\t"); break;
            case '\n': fprintf(out, "\\n"); break;
            case '\v': fprintf(out, "\\v"); break;
            case '\f': fprintf(out, "\\f"); break;
            case '\r': fprintf(out, "\\r"); break;
            case '\\': fprintf(out, "\\\\"); break;
            case '"':  fprintf(out, "\\\""); break;
            default:
                if (c >= 32 && c < 127) {
                    fputc(c, out);
                } else {
                    fprintf(out, "\\x%02x", c);
                }
                break;
        }
        str++;
    }
}

/* Get value kind name as string */
static const char *value_kind_name(anvil_val_kind_t kind)
{
    switch (kind) {
        case ANVIL_VAL_CONST_INT: return "const_int";
        case ANVIL_VAL_CONST_FLOAT: return "const_float";
        case ANVIL_VAL_CONST_NULL: return "const_null";
        case ANVIL_VAL_CONST_STRING: return "const_string";
        case ANVIL_VAL_CONST_ARRAY: return "const_array";
        case ANVIL_VAL_GLOBAL: return "global";
        case ANVIL_VAL_FUNC: return "func";
        case ANVIL_VAL_PARAM: return "param";
        case ANVIL_VAL_INSTR: return "instr";
        case ANVIL_VAL_BLOCK: return "block";
        default: return "?";
    }
}

/* Print type to file */
void anvil_dump_type(FILE *out, anvil_type_t *type)
{
    if (!type) {
        fprintf(out, "null");
        return;
    }
    
    switch (type->kind) {
        case ANVIL_TYPE_VOID:
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U8:
        case ANVIL_TYPE_U16:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F32:
        case ANVIL_TYPE_F64:
            fprintf(out, "%s", type_kind_name(type->kind));
            break;
            
        case ANVIL_TYPE_PTR:
            fprintf(out, "ptr<");
            anvil_dump_type(out, type->data.pointee);
            fprintf(out, ">");
            break;
            
        case ANVIL_TYPE_ARRAY:
            fprintf(out, "[%zu x ", type->data.array.count);
            anvil_dump_type(out, type->data.array.elem);
            fprintf(out, "]");
            break;
            
        case ANVIL_TYPE_STRUCT:
            if (type->data.struc.name) {
                fprintf(out, "%%%s", type->data.struc.name);
            } else {
                fprintf(out, "{");
                for (size_t i = 0; i < type->data.struc.num_fields; i++) {
                    if (i > 0) fprintf(out, ", ");
                    anvil_dump_type(out, type->data.struc.fields[i]);
                }
                fprintf(out, "}");
            }
            break;
            
        case ANVIL_TYPE_FUNC:
            anvil_dump_type(out, type->data.func.ret);
            fprintf(out, "(");
            for (size_t i = 0; i < type->data.func.num_params; i++) {
                if (i > 0) fprintf(out, ", ");
                anvil_dump_type(out, type->data.func.params[i]);
            }
            if (type->data.func.variadic) {
                fprintf(out, ", ...");
            }
            fprintf(out, ")");
            break;
            
        default:
            fprintf(out, "?type");
            break;
    }
}

/* Print value reference to file */
void anvil_dump_value(FILE *out, anvil_value_t *val)
{
    if (!val) {
        fprintf(out, "null");
        return;
    }
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            fprintf(out, "%lld", (long long)val->data.i);
            break;
            
        case ANVIL_VAL_CONST_FLOAT:
            fprintf(out, "%g", val->data.f);
            break;
            
        case ANVIL_VAL_CONST_NULL:
            fprintf(out, "null");
            break;
            
        case ANVIL_VAL_CONST_STRING:
            fprintf(out, "\"");
            print_escaped_string(out, val->data.str);
            fprintf(out, "\"");
            break;
            
        case ANVIL_VAL_CONST_ARRAY:
            fprintf(out, "[array %zu elems]", val->data.array.num_elements);
            break;
            
        case ANVIL_VAL_GLOBAL:
            fprintf(out, "@%s", val->name ? val->name : "?");
            break;
            
        case ANVIL_VAL_FUNC:
            fprintf(out, "@%s", val->name ? val->name : "?");
            break;
            
        case ANVIL_VAL_PARAM:
            if (val->name) {
                fprintf(out, "%%%s", val->name);
            } else {
                fprintf(out, "%%arg%zu", val->data.param.index);
            }
            break;
            
        case ANVIL_VAL_INSTR:
            if (val->name) {
                fprintf(out, "%%%s", val->name);
            } else {
                fprintf(out, "%%v%u", val->id);
            }
            break;
            
        case ANVIL_VAL_BLOCK:
            fprintf(out, "label %%%s", val->name ? val->name : "?");
            break;
            
        default:
            fprintf(out, "?val");
            break;
    }
}

/* Print instruction to file */
void anvil_dump_instr(FILE *out, anvil_instr_t *instr)
{
    if (!instr) return;
    
    fprintf(out, "    ");
    
    /* Print result if any */
    if (instr->result) {
        anvil_dump_value(out, instr->result);
        fprintf(out, " = ");
    }
    
    /* Print operation */
    fprintf(out, "%s", op_name(instr->op));
    
    /* Print result type for certain ops */
    if (instr->result && instr->result->type) {
        fprintf(out, " ");
        anvil_dump_type(out, instr->result->type);
    }
    
    /* Print operands */
    for (size_t i = 0; i < instr->num_operands; i++) {
        fprintf(out, "%s", i == 0 ? " " : ", ");
        anvil_dump_value(out, instr->operands[i]);
    }
    
    /* Print branch targets */
    if (instr->op == ANVIL_OP_BR && instr->true_block) {
        fprintf(out, " label %%%s", instr->true_block->name ? instr->true_block->name : "?");
    } else if (instr->op == ANVIL_OP_BR_COND) {
        if (instr->true_block) {
            fprintf(out, ", label %%%s", instr->true_block->name ? instr->true_block->name : "?");
        }
        if (instr->false_block) {
            fprintf(out, ", label %%%s", instr->false_block->name ? instr->false_block->name : "?");
        }
    }
    
    /* Print PHI incoming values */
    if (instr->op == ANVIL_OP_PHI && instr->num_phi_incoming > 0) {
        for (size_t i = 0; i < instr->num_phi_incoming; i++) {
            fprintf(out, " [");
            if (i < instr->num_operands) {
                anvil_dump_value(out, instr->operands[i]);
            }
            fprintf(out, ", %%%s]", 
                instr->phi_blocks && instr->phi_blocks[i] ? 
                    (instr->phi_blocks[i]->name ? instr->phi_blocks[i]->name : "?") : "?");
        }
    }
    
    /* Print aux type for struct_gep */
    if (instr->op == ANVIL_OP_STRUCT_GEP && instr->aux_type) {
        fprintf(out, " ; struct ");
        anvil_dump_type(out, instr->aux_type);
    }
    
    fprintf(out, "\n");
}

/* Print basic block to file */
void anvil_dump_block(FILE *out, anvil_block_t *block)
{
    if (!block) return;
    
    fprintf(out, "%s:", block->name ? block->name : "?");
    
    /* Print predecessors */
    if (block->num_preds > 0) {
        fprintf(out, "  ; preds: ");
        for (size_t i = 0; i < block->num_preds; i++) {
            if (i > 0) fprintf(out, ", ");
            fprintf(out, "%%%s", 
                block->preds[i] && block->preds[i]->name ? block->preds[i]->name : "?");
        }
    }
    fprintf(out, "\n");
    
    /* Print instructions */
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        anvil_dump_instr(out, instr);
    }
}

/* Print function to file */
void anvil_dump_func(FILE *out, anvil_func_t *func)
{
    if (!func) return;
    
    /* Print function header */
    if (func->is_declaration) {
        fprintf(out, "declare ");
    } else {
        fprintf(out, "define ");
    }
    
    /* Linkage */
    switch (func->linkage) {
        case ANVIL_LINK_INTERNAL: fprintf(out, "internal "); break;
        case ANVIL_LINK_EXTERNAL: fprintf(out, "external "); break;
        case ANVIL_LINK_WEAK: fprintf(out, "weak "); break;
        case ANVIL_LINK_COMMON: fprintf(out, "common "); break;
    }
    
    /* Return type */
    if (func->type && func->type->kind == ANVIL_TYPE_FUNC) {
        anvil_dump_type(out, func->type->data.func.ret);
    } else {
        fprintf(out, "?");
    }
    
    /* Function name and parameters */
    fprintf(out, " @%s(", func->name ? func->name : "?");
    
    if (func->params) {
        for (size_t i = 0; i < func->num_params; i++) {
            if (i > 0) fprintf(out, ", ");
            anvil_value_t *param = func->params[i];
            if (param && param->type) {
                anvil_dump_type(out, param->type);
                fprintf(out, " ");
            }
            if (param) {
                anvil_dump_value(out, param);
            }
        }
    }
    fprintf(out, ")");
    
    if (func->is_declaration) {
        fprintf(out, "\n\n");
        return;
    }
    
    /* Function body */
    fprintf(out, " {\n");
    
    /* Print stack frame info */
    fprintf(out, "; Stack size: %zu bytes, max call args: %zu\n", 
        func->stack_size, func->max_call_args);
    
    /* Print blocks */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_dump_block(out, block);
        fprintf(out, "\n");
    }
    
    fprintf(out, "}\n\n");
}

/* Print global variable to file */
void anvil_dump_global(FILE *out, anvil_global_t *global)
{
    if (!global || !global->value) return;
    
    anvil_value_t *val = global->value;
    
    fprintf(out, "@%s = ", val->name ? val->name : "?");
    
    /* Linkage */
    switch (val->data.global.linkage) {
        case ANVIL_LINK_INTERNAL: fprintf(out, "internal "); break;
        case ANVIL_LINK_EXTERNAL: fprintf(out, "external "); break;
        case ANVIL_LINK_WEAK: fprintf(out, "weak "); break;
        case ANVIL_LINK_COMMON: fprintf(out, "common "); break;
    }
    
    fprintf(out, "global ");
    anvil_dump_type(out, val->type);
    
    /* Initializer */
    if (val->data.global.init) {
        fprintf(out, " ");
        anvil_dump_value(out, val->data.global.init);
    }
    
    fprintf(out, "\n");
}

/* Print module to file */
void anvil_dump_module(FILE *out, anvil_module_t *mod)
{
    if (!mod) {
        fprintf(out, "; (null module)\n");
        return;
    }
    
    fprintf(out, "; ModuleID = '%s'\n", mod->name ? mod->name : "?");
    fprintf(out, "; Functions: %zu, Globals: %zu\n\n", mod->num_funcs, mod->num_globals);
    
    /* Print globals */
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        anvil_dump_global(out, g);
    }
    if (mod->num_globals > 0) {
        fprintf(out, "\n");
    }
    
    /* Print functions */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        anvil_dump_func(out, func);
    }
}

/* Print module to stdout */
void anvil_print_module(anvil_module_t *mod)
{
    anvil_dump_module(stdout, mod);
}

/* Print function to stdout */
void anvil_print_func(anvil_func_t *func)
{
    anvil_dump_func(stdout, func);
}

/* Print instruction to stdout */
void anvil_print_instr(anvil_instr_t *instr)
{
    anvil_dump_instr(stdout, instr);
}

/* Dump module to string */
char *anvil_module_to_string(anvil_module_t *mod)
{
    if (!mod) return NULL;
    
    /* Use a temporary file to capture output */
    char *result = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&result, &size);
    if (!stream) return NULL;
    
    anvil_dump_module(stream, mod);
    fclose(stream);
    
    return result;
}

/* Dump function to string */
char *anvil_func_to_string(anvil_func_t *func)
{
    if (!func) return NULL;
    
    char *result = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&result, &size);
    if (!stream) return NULL;
    
    anvil_dump_func(stream, func);
    fclose(stream);
    
    return result;
}
