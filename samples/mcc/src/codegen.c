/*
 * MCC - Micro C Compiler
 * Code generator using ANVIL
 */

#include "mcc.h"

/* Map MCC architecture to ANVIL architecture */
static anvil_arch_t mcc_to_anvil_arch(mcc_arch_t arch)
{
    switch (arch) {
        case MCC_ARCH_X86:         return ANVIL_ARCH_X86;
        case MCC_ARCH_X86_64:      return ANVIL_ARCH_X86_64;
        case MCC_ARCH_S370:        return ANVIL_ARCH_S370;
        case MCC_ARCH_S370_XA:     return ANVIL_ARCH_S370_XA;
        case MCC_ARCH_S390:        return ANVIL_ARCH_S390;
        case MCC_ARCH_ZARCH:       return ANVIL_ARCH_ZARCH;
        case MCC_ARCH_PPC32:       return ANVIL_ARCH_PPC32;
        case MCC_ARCH_PPC64:       return ANVIL_ARCH_PPC64;
        case MCC_ARCH_PPC64LE:     return ANVIL_ARCH_PPC64LE;
        case MCC_ARCH_ARM64:       return ANVIL_ARCH_ARM64;
        case MCC_ARCH_ARM64_MACOS: return ANVIL_ARCH_ARM64;  /* Same arch, different ABI */
        default:                   return ANVIL_ARCH_X86_64;
    }
}

/* Check if architecture uses Darwin ABI */
static bool mcc_arch_is_darwin(mcc_arch_t arch)
{
    return arch == MCC_ARCH_ARM64_MACOS;
}

mcc_codegen_t *mcc_codegen_create(mcc_context_t *ctx, mcc_symtab_t *symtab,
                                   mcc_type_context_t *types)
{
    mcc_codegen_t *cg = mcc_alloc(ctx, sizeof(mcc_codegen_t));
    cg->mcc_ctx = ctx;
    cg->symtab = symtab;
    cg->types = types;
    
    /* Create ANVIL context */
    cg->anvil_ctx = anvil_ctx_create();
    if (!cg->anvil_ctx) {
        mcc_fatal(ctx, "Failed to create ANVIL context");
        return NULL;
    }
    
    return cg;
}

void mcc_codegen_destroy(mcc_codegen_t *cg)
{
    if (cg && cg->anvil_ctx) {
        anvil_ctx_destroy(cg->anvil_ctx);
    }
}

void mcc_codegen_set_target(mcc_codegen_t *cg, mcc_arch_t arch)
{
    anvil_ctx_set_target(cg->anvil_ctx, mcc_to_anvil_arch(arch));
    
    /* Set Darwin ABI for macOS ARM64 */
    if (mcc_arch_is_darwin(arch)) {
        anvil_ctx_set_abi(cg->anvil_ctx, ANVIL_ABI_DARWIN);
    }
}

void mcc_codegen_set_opt_level(mcc_codegen_t *cg, mcc_opt_level_t level)
{
    /* Optimization level - stored for later use */
    (void)cg;
    (void)level;
}

/* Convert MCC type to ANVIL type */
anvil_type_t *mcc_codegen_type(mcc_codegen_t *cg, mcc_type_t *type)
{
    if (!type) return anvil_type_i32(cg->anvil_ctx);
    
    switch (type->kind) {
        case TYPE_VOID:
            return anvil_type_void(cg->anvil_ctx);
        case TYPE_CHAR:
            return anvil_type_i8(cg->anvil_ctx);
        case TYPE_SHORT:
            return anvil_type_i16(cg->anvil_ctx);
        case TYPE_INT:
        case TYPE_ENUM:
            return anvil_type_i32(cg->anvil_ctx);
        case TYPE_LONG:
            return anvil_type_i32(cg->anvil_ctx); /* 32-bit long for C89 */
        case TYPE_FLOAT:
            return anvil_type_f32(cg->anvil_ctx);
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            return anvil_type_f64(cg->anvil_ctx);
        case TYPE_POINTER:
            return anvil_type_ptr(cg->anvil_ctx,
                mcc_codegen_type(cg, type->data.pointer.pointee));
        case TYPE_ARRAY:
            return anvil_type_array(cg->anvil_ctx,
                mcc_codegen_type(cg, type->data.array.element),
                type->data.array.length);
        case TYPE_STRUCT:
        case TYPE_UNION: {
            /* Create struct type */
            int num_fields = type->data.record.num_fields;
            anvil_type_t **field_types = mcc_alloc(cg->mcc_ctx,
                num_fields * sizeof(anvil_type_t*));
            
            int i = 0;
            for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next, i++) {
                field_types[i] = mcc_codegen_type(cg, f->type);
            }
            
            return anvil_type_struct(cg->anvil_ctx, NULL, field_types, num_fields);
        }
        case TYPE_FUNCTION: {
            anvil_type_t *ret_type = mcc_codegen_type(cg, type->data.function.return_type);
            int num_params = type->data.function.num_params;
            anvil_type_t **param_types = mcc_alloc(cg->mcc_ctx,
                (num_params > 0 ? num_params : 1) * sizeof(anvil_type_t*));
            
            int i = 0;
            for (mcc_func_param_t *p = type->data.function.params; p; p = p->next, i++) {
                param_types[i] = mcc_codegen_type(cg, p->type);
            }
            
            return anvil_type_func(cg->anvil_ctx, ret_type, param_types, num_params,
                                   type->data.function.is_variadic);
        }
        default:
            return anvil_type_i32(cg->anvil_ctx);
    }
}

/* Find local variable value by name */
static anvil_value_t *find_local(mcc_codegen_t *cg, const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < cg->num_locals; i++) {
        if (cg->locals[i].name && strcmp(cg->locals[i].name, name) == 0) {
            return cg->locals[i].value;
        }
    }
    return NULL;
}

/* Add local variable */
static void add_local(mcc_codegen_t *cg, const char *name, anvil_value_t *value)
{
    if (cg->num_locals >= cg->cap_locals) {
        size_t new_cap = cg->cap_locals ? cg->cap_locals * 2 : 16;
        cg->locals = mcc_realloc(cg->mcc_ctx, cg->locals,
                                  cg->cap_locals * sizeof(cg->locals[0]),
                                  new_cap * sizeof(cg->locals[0]));
        cg->cap_locals = new_cap;
    }
    cg->locals[cg->num_locals].name = name;
    cg->locals[cg->num_locals].value = value;
    cg->num_locals++;
}

/* Find or create string literal */
static anvil_value_t *get_string_literal(mcc_codegen_t *cg, const char *str)
{
    /* Check if already exists */
    for (size_t i = 0; i < cg->num_strings; i++) {
        if (strcmp(cg->strings[i].str, str) == 0) {
            return cg->strings[i].value;
        }
    }
    
    /* Create string constant */
    anvil_value_t *strval = anvil_const_string(cg->anvil_ctx, str);
    
    /* Add to pool */
    if (cg->num_strings >= cg->cap_strings) {
        size_t new_cap = cg->cap_strings ? cg->cap_strings * 2 : 8;
        cg->strings = mcc_realloc(cg->mcc_ctx, cg->strings,
                                   cg->cap_strings * sizeof(cg->strings[0]),
                                   new_cap * sizeof(cg->strings[0]));
        cg->cap_strings = new_cap;
    }
    cg->strings[cg->num_strings].str = str;
    cg->strings[cg->num_strings].value = strval;
    cg->num_strings++;
    
    return strval;
}

/* Find or create label block */
static anvil_block_t *get_label_block(mcc_codegen_t *cg, const char *name)
{
    for (size_t i = 0; i < cg->num_labels; i++) {
        if (strcmp(cg->labels[i].name, name) == 0) {
            return cg->labels[i].block;
        }
    }
    
    /* Create new block */
    anvil_block_t *block = anvil_block_create(cg->current_func, name);
    
    if (cg->num_labels >= cg->cap_labels) {
        size_t new_cap = cg->cap_labels ? cg->cap_labels * 2 : 8;
        cg->labels = mcc_realloc(cg->mcc_ctx, cg->labels,
                                  cg->cap_labels * sizeof(cg->labels[0]),
                                  new_cap * sizeof(cg->labels[0]));
        cg->cap_labels = new_cap;
    }
    cg->labels[cg->num_labels].name = name;
    cg->labels[cg->num_labels].block = block;
    cg->num_labels++;
    
    return block;
}

/* Set current block for code generation */
static void set_current_block(mcc_codegen_t *cg, anvil_block_t *block)
{
    cg->current_block = block;
    anvil_set_insert_point(cg->anvil_ctx, block);
}

/* Find function by symbol */
static anvil_func_t *find_func(mcc_codegen_t *cg, mcc_symbol_t *sym)
{
    for (size_t i = 0; i < cg->num_funcs; i++) {
        if (cg->funcs[i].sym == sym) {
            return cg->funcs[i].func;
        }
    }
    return NULL;
}

/* Add function mapping */
static void add_func(mcc_codegen_t *cg, mcc_symbol_t *sym, anvil_func_t *func)
{
    if (cg->num_funcs >= cg->cap_funcs) {
        size_t new_cap = cg->cap_funcs ? cg->cap_funcs * 2 : 16;
        cg->funcs = mcc_realloc(cg->mcc_ctx, cg->funcs,
                                 cg->cap_funcs * sizeof(cg->funcs[0]),
                                 new_cap * sizeof(cg->funcs[0]));
        cg->cap_funcs = new_cap;
    }
    cg->funcs[cg->num_funcs].sym = sym;
    cg->funcs[cg->num_funcs].func = func;
    cg->num_funcs++;
}

/* Get or declare a function */
static anvil_func_t *get_or_declare_func(mcc_codegen_t *cg, mcc_symbol_t *sym)
{
    /* Check if already declared */
    anvil_func_t *func = find_func(cg, sym);
    if (func) return func;
    
    /* Create function type */
    anvil_type_t *func_type = mcc_codegen_type(cg, sym->type);
    
    /* Declare the function */
    func = anvil_func_declare(cg->anvil_mod, sym->name, func_type);
    add_func(cg, sym, func);
    
    return func;
}

/* Generate code for expression (returns value) */
anvil_value_t *mcc_codegen_expr(mcc_codegen_t *cg, mcc_ast_node_t *expr)
{
    if (!expr) return NULL;
    
    switch (expr->kind) {
        case AST_INT_LIT:
            return anvil_const_i32(cg->anvil_ctx, (int32_t)expr->data.int_lit.value);
            
        case AST_FLOAT_LIT:
            if (expr->data.float_lit.suffix == FLOAT_SUFFIX_F) {
                return anvil_const_f32(cg->anvil_ctx, (float)expr->data.float_lit.value);
            }
            return anvil_const_f64(cg->anvil_ctx, expr->data.float_lit.value);
            
        case AST_CHAR_LIT:
            return anvil_const_i8(cg->anvil_ctx, (int8_t)expr->data.char_lit.value);
            
        case AST_STRING_LIT:
            return get_string_literal(cg, expr->data.string_lit.value);
            
        case AST_IDENT_EXPR: {
            mcc_symbol_t *sym = expr->data.ident_expr.symbol;
            const char *name = expr->data.ident_expr.name;
            
            /* Try to find local by name */
            anvil_value_t *ptr = find_local(cg, name);
            
            if (ptr) {
                /* Load from local variable */
                anvil_type_t *type = sym ? mcc_codegen_type(cg, sym->type) 
                                         : anvil_type_i32(cg->anvil_ctx);
                return anvil_build_load(cg->anvil_ctx, type, ptr, "load");
            }
            
            /* For functions, get or declare the function and return its value */
            if (sym && sym->kind == SYM_FUNC) {
                anvil_func_t *func = get_or_declare_func(cg, sym);
                return anvil_func_get_value(func);
            }
            
            return NULL;
        }
        
        case AST_BINARY_EXPR: {
            mcc_binop_t op = expr->data.binary_expr.op;
            
            /* Handle assignment */
            if (op >= BINOP_ASSIGN && op <= BINOP_RSHIFT_ASSIGN) {
                anvil_value_t *lhs_ptr = mcc_codegen_lvalue(cg, expr->data.binary_expr.lhs);
                anvil_value_t *rhs = mcc_codegen_expr(cg, expr->data.binary_expr.rhs);
                
                if (!lhs_ptr || !rhs) return NULL;
                
                anvil_value_t *result = rhs;
                
                /* Compound assignment */
                if (op != BINOP_ASSIGN) {
                    anvil_type_t *type = mcc_codegen_type(cg, expr->data.binary_expr.lhs->type);
                    anvil_value_t *lhs = anvil_build_load(cg->anvil_ctx, type, lhs_ptr, "lhs");
                    
                    switch (op) {
                        case BINOP_ADD_ASSIGN:
                            result = anvil_build_add(cg->anvil_ctx, lhs, rhs, "add");
                            break;
                        case BINOP_SUB_ASSIGN:
                            result = anvil_build_sub(cg->anvil_ctx, lhs, rhs, "sub");
                            break;
                        case BINOP_MUL_ASSIGN:
                            result = anvil_build_mul(cg->anvil_ctx, lhs, rhs, "mul");
                            break;
                        case BINOP_DIV_ASSIGN:
                            result = anvil_build_sdiv(cg->anvil_ctx, lhs, rhs, "div");
                            break;
                        case BINOP_MOD_ASSIGN:
                            result = anvil_build_smod(cg->anvil_ctx, lhs, rhs, "mod");
                            break;
                        case BINOP_AND_ASSIGN:
                            result = anvil_build_and(cg->anvil_ctx, lhs, rhs, "and");
                            break;
                        case BINOP_OR_ASSIGN:
                            result = anvil_build_or(cg->anvil_ctx, lhs, rhs, "or");
                            break;
                        case BINOP_XOR_ASSIGN:
                            result = anvil_build_xor(cg->anvil_ctx, lhs, rhs, "xor");
                            break;
                        case BINOP_LSHIFT_ASSIGN:
                            result = anvil_build_shl(cg->anvil_ctx, lhs, rhs, "shl");
                            break;
                        case BINOP_RSHIFT_ASSIGN:
                            result = anvil_build_shr(cg->anvil_ctx, lhs, rhs, "shr");
                            break;
                        default:
                            break;
                    }
                }
                
                anvil_build_store(cg->anvil_ctx, result, lhs_ptr);
                return result;
            }
            
            /* Handle short-circuit logical operators */
            if (op == BINOP_AND || op == BINOP_OR) {
                anvil_value_t *lhs = mcc_codegen_expr(cg, expr->data.binary_expr.lhs);
                
                anvil_block_t *rhs_block = anvil_block_create(cg->current_func, "land.rhs");
                anvil_block_t *end_block = anvil_block_create(cg->current_func, "land.end");
                
                /* Compare LHS to zero */
                anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
                anvil_value_t *lhs_bool = anvil_build_cmp_ne(cg->anvil_ctx, lhs, zero, "cmp");
                
                if (op == BINOP_AND) {
                    anvil_build_br_cond(cg->anvil_ctx, lhs_bool, rhs_block, end_block);
                } else {
                    anvil_build_br_cond(cg->anvil_ctx, lhs_bool, end_block, rhs_block);
                }
                
                /* RHS block */
                set_current_block(cg, rhs_block);
                anvil_value_t *rhs = mcc_codegen_expr(cg, expr->data.binary_expr.rhs);
                anvil_value_t *rhs_bool = anvil_build_cmp_ne(cg->anvil_ctx, rhs, zero, "cmp");
                anvil_build_br(cg->anvil_ctx, end_block);
                anvil_block_t *rhs_end = cg->current_block;
                
                /* End block with PHI */
                set_current_block(cg, end_block);
                anvil_value_t *phi = anvil_build_phi(cg->anvil_ctx, anvil_type_i32(cg->anvil_ctx), "phi");
                
                if (op == BINOP_AND) {
                    anvil_phi_add_incoming(phi, zero, rhs_block);
                } else {
                    anvil_value_t *one = anvil_const_i32(cg->anvil_ctx, 1);
                    anvil_phi_add_incoming(phi, one, rhs_block);
                }
                anvil_phi_add_incoming(phi, rhs_bool, rhs_end);
                
                return phi;
            }
            
            /* Regular binary operators */
            anvil_value_t *lhs = mcc_codegen_expr(cg, expr->data.binary_expr.lhs);
            anvil_value_t *rhs = mcc_codegen_expr(cg, expr->data.binary_expr.rhs);
            
            if (!lhs || !rhs) return NULL;
            
            switch (op) {
                case BINOP_ADD:
                    if (expr->type && mcc_type_is_floating(expr->type)) {
                        return anvil_build_fadd(cg->anvil_ctx, lhs, rhs, "fadd");
                    }
                    return anvil_build_add(cg->anvil_ctx, lhs, rhs, "add");
                case BINOP_SUB:
                    if (expr->type && mcc_type_is_floating(expr->type)) {
                        return anvil_build_fsub(cg->anvil_ctx, lhs, rhs, "fsub");
                    }
                    return anvil_build_sub(cg->anvil_ctx, lhs, rhs, "sub");
                case BINOP_MUL:
                    if (expr->type && mcc_type_is_floating(expr->type)) {
                        return anvil_build_fmul(cg->anvil_ctx, lhs, rhs, "fmul");
                    }
                    return anvil_build_mul(cg->anvil_ctx, lhs, rhs, "mul");
                case BINOP_DIV:
                    if (expr->type && mcc_type_is_floating(expr->type)) {
                        return anvil_build_fdiv(cg->anvil_ctx, lhs, rhs, "fdiv");
                    }
                    if (expr->type && expr->type->is_unsigned) {
                        return anvil_build_udiv(cg->anvil_ctx, lhs, rhs, "udiv");
                    }
                    return anvil_build_sdiv(cg->anvil_ctx, lhs, rhs, "sdiv");
                case BINOP_MOD:
                    if (expr->type && expr->type->is_unsigned) {
                        return anvil_build_umod(cg->anvil_ctx, lhs, rhs, "umod");
                    }
                    return anvil_build_smod(cg->anvil_ctx, lhs, rhs, "smod");
                case BINOP_BIT_AND:
                    return anvil_build_and(cg->anvil_ctx, lhs, rhs, "and");
                case BINOP_BIT_OR:
                    return anvil_build_or(cg->anvil_ctx, lhs, rhs, "or");
                case BINOP_BIT_XOR:
                    return anvil_build_xor(cg->anvil_ctx, lhs, rhs, "xor");
                case BINOP_LSHIFT:
                    return anvil_build_shl(cg->anvil_ctx, lhs, rhs, "shl");
                case BINOP_RSHIFT:
                    if (expr->data.binary_expr.lhs->type &&
                        expr->data.binary_expr.lhs->type->is_unsigned) {
                        return anvil_build_shr(cg->anvil_ctx, lhs, rhs, "shr");
                    }
                    return anvil_build_sar(cg->anvil_ctx, lhs, rhs, "sar");
                case BINOP_EQ:
                    return anvil_build_cmp_eq(cg->anvil_ctx, lhs, rhs, "eq");
                case BINOP_NE:
                    return anvil_build_cmp_ne(cg->anvil_ctx, lhs, rhs, "ne");
                case BINOP_LT:
                    if (expr->data.binary_expr.lhs->type &&
                        expr->data.binary_expr.lhs->type->is_unsigned) {
                        return anvil_build_cmp_ult(cg->anvil_ctx, lhs, rhs, "ult");
                    }
                    return anvil_build_cmp_lt(cg->anvil_ctx, lhs, rhs, "lt");
                case BINOP_GT:
                    if (expr->data.binary_expr.lhs->type &&
                        expr->data.binary_expr.lhs->type->is_unsigned) {
                        return anvil_build_cmp_ugt(cg->anvil_ctx, lhs, rhs, "ugt");
                    }
                    return anvil_build_cmp_gt(cg->anvil_ctx, lhs, rhs, "gt");
                case BINOP_LE:
                    if (expr->data.binary_expr.lhs->type &&
                        expr->data.binary_expr.lhs->type->is_unsigned) {
                        return anvil_build_cmp_ule(cg->anvil_ctx, lhs, rhs, "ule");
                    }
                    return anvil_build_cmp_le(cg->anvil_ctx, lhs, rhs, "le");
                case BINOP_GE:
                    if (expr->data.binary_expr.lhs->type &&
                        expr->data.binary_expr.lhs->type->is_unsigned) {
                        return anvil_build_cmp_uge(cg->anvil_ctx, lhs, rhs, "uge");
                    }
                    return anvil_build_cmp_ge(cg->anvil_ctx, lhs, rhs, "ge");
                default:
                    return NULL;
            }
        }
        
        case AST_UNARY_EXPR: {
            mcc_unop_t op = expr->data.unary_expr.op;
            
            switch (op) {
                case UNOP_NEG: {
                    anvil_value_t *val = mcc_codegen_expr(cg, expr->data.unary_expr.operand);
                    if (expr->type && mcc_type_is_floating(expr->type)) {
                        return anvil_build_fneg(cg->anvil_ctx, val, "fneg");
                    }
                    return anvil_build_neg(cg->anvil_ctx, val, "neg");
                }
                case UNOP_POS:
                    return mcc_codegen_expr(cg, expr->data.unary_expr.operand);
                case UNOP_NOT: {
                    anvil_value_t *val = mcc_codegen_expr(cg, expr->data.unary_expr.operand);
                    anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
                    return anvil_build_cmp_eq(cg->anvil_ctx, val, zero, "not");
                }
                case UNOP_BIT_NOT: {
                    anvil_value_t *val = mcc_codegen_expr(cg, expr->data.unary_expr.operand);
                    return anvil_build_not(cg->anvil_ctx, val, "bitnot");
                }
                case UNOP_DEREF: {
                    anvil_value_t *ptr = mcc_codegen_expr(cg, expr->data.unary_expr.operand);
                    anvil_type_t *type = mcc_codegen_type(cg, expr->type);
                    return anvil_build_load(cg->anvil_ctx, type, ptr, "deref");
                }
                case UNOP_ADDR:
                    return mcc_codegen_lvalue(cg, expr->data.unary_expr.operand);
                case UNOP_PRE_INC:
                case UNOP_PRE_DEC: {
                    anvil_value_t *ptr = mcc_codegen_lvalue(cg, expr->data.unary_expr.operand);
                    anvil_type_t *type = mcc_codegen_type(cg, expr->data.unary_expr.operand->type);
                    anvil_value_t *val = anvil_build_load(cg->anvil_ctx, type, ptr, "val");
                    anvil_value_t *one = anvil_const_i32(cg->anvil_ctx, 1);
                    anvil_value_t *result = (op == UNOP_PRE_INC) ?
                        anvil_build_add(cg->anvil_ctx, val, one, "inc") :
                        anvil_build_sub(cg->anvil_ctx, val, one, "dec");
                    anvil_build_store(cg->anvil_ctx, result, ptr);
                    return result;
                }
                case UNOP_POST_INC:
                case UNOP_POST_DEC: {
                    anvil_value_t *ptr = mcc_codegen_lvalue(cg, expr->data.unary_expr.operand);
                    anvil_type_t *type = mcc_codegen_type(cg, expr->data.unary_expr.operand->type);
                    anvil_value_t *val = anvil_build_load(cg->anvil_ctx, type, ptr, "val");
                    anvil_value_t *one = anvil_const_i32(cg->anvil_ctx, 1);
                    anvil_value_t *result = (op == UNOP_POST_INC) ?
                        anvil_build_add(cg->anvil_ctx, val, one, "inc") :
                        anvil_build_sub(cg->anvil_ctx, val, one, "dec");
                    anvil_build_store(cg->anvil_ctx, result, ptr);
                    return val; /* Return original value */
                }
                default:
                    return NULL;
            }
        }
        
        case AST_TERNARY_EXPR: {
            anvil_value_t *cond = mcc_codegen_expr(cg, expr->data.ternary_expr.cond);
            
            anvil_block_t *then_block = anvil_block_create(cg->current_func, "ternary.then");
            anvil_block_t *else_block = anvil_block_create(cg->current_func, "ternary.else");
            anvil_block_t *end_block = anvil_block_create(cg->current_func, "ternary.end");
            
            /* Compare to zero */
            anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
            anvil_value_t *cond_bool = anvil_build_cmp_ne(cg->anvil_ctx, cond, zero, "cond");
            anvil_build_br_cond(cg->anvil_ctx, cond_bool, then_block, else_block);
            
            /* Then block */
            set_current_block(cg, then_block);
            anvil_value_t *then_val = mcc_codegen_expr(cg, expr->data.ternary_expr.then_expr);
            anvil_build_br(cg->anvil_ctx, end_block);
            anvil_block_t *then_end = cg->current_block;
            
            /* Else block */
            set_current_block(cg, else_block);
            anvil_value_t *else_val = mcc_codegen_expr(cg, expr->data.ternary_expr.else_expr);
            anvil_build_br(cg->anvil_ctx, end_block);
            anvil_block_t *else_end = cg->current_block;
            
            /* End block with PHI */
            set_current_block(cg, end_block);
            anvil_type_t *type = mcc_codegen_type(cg, expr->type);
            anvil_value_t *phi = anvil_build_phi(cg->anvil_ctx, type, "ternary");
            anvil_phi_add_incoming(phi, then_val, then_end);
            anvil_phi_add_incoming(phi, else_val, else_end);
            
            return phi;
        }
        
        case AST_CALL_EXPR: {
            anvil_value_t *func = mcc_codegen_expr(cg, expr->data.call_expr.func);
            
            size_t num_args = expr->data.call_expr.num_args;
            anvil_value_t **args = NULL;
            if (num_args > 0) {
                args = mcc_alloc(cg->mcc_ctx, num_args * sizeof(anvil_value_t*));
                for (size_t i = 0; i < num_args; i++) {
                    args[i] = mcc_codegen_expr(cg, expr->data.call_expr.args[i]);
                }
            }
            
            anvil_type_t *func_type = mcc_codegen_type(cg, expr->data.call_expr.func->type);
            return anvil_build_call(cg->anvil_ctx, func_type, func, args, num_args, "call");
        }
        
        case AST_SUBSCRIPT_EXPR: {
            anvil_value_t *ptr = mcc_codegen_lvalue(cg, expr);
            anvil_type_t *type = mcc_codegen_type(cg, expr->type);
            return anvil_build_load(cg->anvil_ctx, type, ptr, "subscript");
        }
        
        case AST_MEMBER_EXPR: {
            anvil_value_t *ptr = mcc_codegen_lvalue(cg, expr);
            anvil_type_t *type = mcc_codegen_type(cg, expr->type);
            return anvil_build_load(cg->anvil_ctx, type, ptr, "member");
        }
        
        case AST_CAST_EXPR: {
            anvil_value_t *val = mcc_codegen_expr(cg, expr->data.cast_expr.expr);
            mcc_type_t *from = expr->data.cast_expr.expr->type;
            mcc_type_t *to = expr->data.cast_expr.target_type;
            
            if (!from || !to) return val;
            
            /* Integer to integer */
            if (mcc_type_is_integer(from) && mcc_type_is_integer(to)) {
                if (from->size < to->size) {
                    if (from->is_unsigned) {
                        return anvil_build_zext(cg->anvil_ctx, val,
                            mcc_codegen_type(cg, to), "zext");
                    }
                    return anvil_build_sext(cg->anvil_ctx, val,
                        mcc_codegen_type(cg, to), "sext");
                } else if (from->size > to->size) {
                    return anvil_build_trunc(cg->anvil_ctx, val,
                        mcc_codegen_type(cg, to), "trunc");
                }
            }
            
            /* Integer to float */
            if (mcc_type_is_integer(from) && mcc_type_is_floating(to)) {
                if (from->is_unsigned) {
                    return anvil_build_uitofp(cg->anvil_ctx, val,
                        mcc_codegen_type(cg, to), "uitofp");
                }
                return anvil_build_sitofp(cg->anvil_ctx, val,
                    mcc_codegen_type(cg, to), "sitofp");
            }
            
            /* Float to integer */
            if (mcc_type_is_floating(from) && mcc_type_is_integer(to)) {
                if (to->is_unsigned) {
                    return anvil_build_fptoui(cg->anvil_ctx, val,
                        mcc_codegen_type(cg, to), "fptoui");
                }
                return anvil_build_fptosi(cg->anvil_ctx, val,
                    mcc_codegen_type(cg, to), "fptosi");
            }
            
            /* Pointer casts */
            if (mcc_type_is_pointer(from) || mcc_type_is_pointer(to)) {
                return anvil_build_bitcast(cg->anvil_ctx, val,
                    mcc_codegen_type(cg, to), "bitcast");
            }
            
            return val;
        }
        
        case AST_SIZEOF_EXPR: {
            size_t size;
            if (expr->data.sizeof_expr.type_arg) {
                size = mcc_type_sizeof(expr->data.sizeof_expr.type_arg);
            } else if (expr->data.sizeof_expr.expr_arg) {
                size = mcc_type_sizeof(expr->data.sizeof_expr.expr_arg->type);
            } else {
                size = 0;
            }
            return anvil_const_i32(cg->anvil_ctx, (int32_t)size);
        }
        
        case AST_COMMA_EXPR:
            mcc_codegen_expr(cg, expr->data.comma_expr.left);
            return mcc_codegen_expr(cg, expr->data.comma_expr.right);
            
        default:
            return NULL;
    }
}

/* Generate code for lvalue (returns pointer) */
anvil_value_t *mcc_codegen_lvalue(mcc_codegen_t *cg, mcc_ast_node_t *expr)
{
    if (!expr) return NULL;
    
    switch (expr->kind) {
        case AST_IDENT_EXPR: {
            const char *name = expr->data.ident_expr.name;
            
            anvil_value_t *ptr = find_local(cg, name);
            if (ptr) return ptr;
            
            return NULL;
        }
        
        case AST_UNARY_EXPR:
            if (expr->data.unary_expr.op == UNOP_DEREF) {
                return mcc_codegen_expr(cg, expr->data.unary_expr.operand);
            }
            return NULL;
            
        case AST_SUBSCRIPT_EXPR: {
            anvil_value_t *array = mcc_codegen_expr(cg, expr->data.subscript_expr.array);
            anvil_value_t *index = mcc_codegen_expr(cg, expr->data.subscript_expr.index);
            anvil_type_t *elem_type = mcc_codegen_type(cg, expr->type);
            anvil_value_t *indices[1] = { index };
            return anvil_build_gep(cg->anvil_ctx, elem_type, array, indices, 1, "gep");
        }
        
        case AST_MEMBER_EXPR: {
            mcc_ast_node_t *obj = expr->data.member_expr.object;
            anvil_value_t *ptr;
            
            if (expr->data.member_expr.is_arrow) {
                ptr = mcc_codegen_expr(cg, obj);
            } else {
                ptr = mcc_codegen_lvalue(cg, obj);
            }
            
            /* Find field index */
            mcc_type_t *obj_type = obj->type;
            if (expr->data.member_expr.is_arrow && mcc_type_is_pointer(obj_type)) {
                obj_type = obj_type->data.pointer.pointee;
            }
            
            int field_idx = 0;
            for (mcc_struct_field_t *f = obj_type->data.record.fields; f; f = f->next, field_idx++) {
                if (strcmp(f->name, expr->data.member_expr.member) == 0) {
                    break;
                }
            }
            
            anvil_type_t *struct_type = mcc_codegen_type(cg, obj_type);
            return anvil_build_struct_gep(cg->anvil_ctx, struct_type, ptr, field_idx, "field");
        }
        
        default:
            return NULL;
    }
}

/* Forward declarations for statement codegen */
static void codegen_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);

/* Check if block has terminator */
static bool block_has_terminator(mcc_codegen_t *cg)
{
    /* Simple heuristic - in a real implementation we'd track this */
    (void)cg;
    return false;
}

/* Generate code for statement */
void mcc_codegen_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    codegen_stmt(cg, stmt);
}

static void codegen_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    if (!stmt) return;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            mcc_codegen_compound_stmt(cg, stmt);
            break;
            
        case AST_EXPR_STMT:
            if (stmt->data.expr_stmt.expr) {
                mcc_codegen_expr(cg, stmt->data.expr_stmt.expr);
            }
            break;
            
        case AST_IF_STMT:
            mcc_codegen_if_stmt(cg, stmt);
            break;
            
        case AST_WHILE_STMT:
            mcc_codegen_while_stmt(cg, stmt);
            break;
            
        case AST_DO_STMT:
            mcc_codegen_do_stmt(cg, stmt);
            break;
            
        case AST_FOR_STMT:
            mcc_codegen_for_stmt(cg, stmt);
            break;
            
        case AST_SWITCH_STMT:
            mcc_codegen_switch_stmt(cg, stmt);
            break;
            
        case AST_RETURN_STMT:
            mcc_codegen_return_stmt(cg, stmt);
            break;
            
        case AST_BREAK_STMT:
            if (cg->break_target) {
                anvil_build_br(cg->anvil_ctx, cg->break_target);
            }
            break;
            
        case AST_CONTINUE_STMT:
            if (cg->continue_target) {
                anvil_build_br(cg->anvil_ctx, cg->continue_target);
            }
            break;
            
        case AST_GOTO_STMT: {
            anvil_block_t *target = get_label_block(cg, stmt->data.goto_stmt.label);
            anvil_build_br(cg->anvil_ctx, target);
            break;
        }
        
        case AST_LABEL_STMT: {
            anvil_block_t *label_block = get_label_block(cg, stmt->data.label_stmt.label);
            anvil_build_br(cg->anvil_ctx, label_block);
            set_current_block(cg, label_block);
            codegen_stmt(cg, stmt->data.label_stmt.stmt);
            break;
        }
        
        case AST_CASE_STMT:
        case AST_DEFAULT_STMT:
            /* Handled by switch statement */
            break;
            
        case AST_NULL_STMT:
            break;
            
        case AST_VAR_DECL: {
            /* Local variable declaration */
            anvil_type_t *type = mcc_codegen_type(cg, stmt->data.var_decl.var_type);
            anvil_value_t *alloca_val = anvil_build_alloca(cg->anvil_ctx, type, stmt->data.var_decl.name);
            
            /* Add to locals by name */
            add_local(cg, stmt->data.var_decl.name, alloca_val);
            
            /* Initialize if needed */
            if (stmt->data.var_decl.init) {
                anvil_value_t *init = mcc_codegen_expr(cg, stmt->data.var_decl.init);
                if (init) {
                    anvil_build_store(cg->anvil_ctx, init, alloca_val);
                }
            }
            break;
        }
        
        case AST_DECL_LIST:
            /* Multiple declarations: int a, b, c; */
            for (size_t i = 0; i < stmt->data.decl_list.num_decls; i++) {
                codegen_stmt(cg, stmt->data.decl_list.decls[i]);
            }
            break;
        
        default:
            break;
    }
}

void mcc_codegen_compound_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
        codegen_stmt(cg, stmt->data.compound_stmt.stmts[i]);
    }
}

void mcc_codegen_if_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    anvil_value_t *cond = mcc_codegen_expr(cg, stmt->data.if_stmt.cond);
    
    anvil_block_t *then_block = anvil_block_create(cg->current_func, "if.then");
    anvil_block_t *else_block = stmt->data.if_stmt.else_stmt ?
        anvil_block_create(cg->current_func, "if.else") : NULL;
    anvil_block_t *end_block = anvil_block_create(cg->current_func, "if.end");
    
    /* Compare to zero */
    anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
    anvil_value_t *cond_bool = anvil_build_cmp_ne(cg->anvil_ctx, cond, zero, "cond");
    
    anvil_build_br_cond(cg->anvil_ctx, cond_bool, then_block,
                        else_block ? else_block : end_block);
    
    /* Then block */
    set_current_block(cg, then_block);
    codegen_stmt(cg, stmt->data.if_stmt.then_stmt);
    if (!block_has_terminator(cg)) {
        anvil_build_br(cg->anvil_ctx, end_block);
    }
    
    /* Else block */
    if (else_block) {
        set_current_block(cg, else_block);
        codegen_stmt(cg, stmt->data.if_stmt.else_stmt);
        if (!block_has_terminator(cg)) {
            anvil_build_br(cg->anvil_ctx, end_block);
        }
    }
    
    set_current_block(cg, end_block);
}

void mcc_codegen_while_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    anvil_block_t *cond_block = anvil_block_create(cg->current_func, "while.cond");
    anvil_block_t *body_block = anvil_block_create(cg->current_func, "while.body");
    anvil_block_t *end_block = anvil_block_create(cg->current_func, "while.end");
    
    anvil_block_t *old_break = cg->break_target;
    anvil_block_t *old_continue = cg->continue_target;
    cg->break_target = end_block;
    cg->continue_target = cond_block;
    
    anvil_build_br(cg->anvil_ctx, cond_block);
    
    /* Condition */
    set_current_block(cg, cond_block);
    anvil_value_t *cond = mcc_codegen_expr(cg, stmt->data.while_stmt.cond);
    anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
    anvil_value_t *cond_bool = anvil_build_cmp_ne(cg->anvil_ctx, cond, zero, "cond");
    anvil_build_br_cond(cg->anvil_ctx, cond_bool, body_block, end_block);
    
    /* Body */
    set_current_block(cg, body_block);
    codegen_stmt(cg, stmt->data.while_stmt.body);
    if (!block_has_terminator(cg)) {
        anvil_build_br(cg->anvil_ctx, cond_block);
    }
    
    set_current_block(cg, end_block);
    cg->break_target = old_break;
    cg->continue_target = old_continue;
}

void mcc_codegen_do_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    anvil_block_t *body_block = anvil_block_create(cg->current_func, "do.body");
    anvil_block_t *cond_block = anvil_block_create(cg->current_func, "do.cond");
    anvil_block_t *end_block = anvil_block_create(cg->current_func, "do.end");
    
    anvil_block_t *old_break = cg->break_target;
    anvil_block_t *old_continue = cg->continue_target;
    cg->break_target = end_block;
    cg->continue_target = cond_block;
    
    anvil_build_br(cg->anvil_ctx, body_block);
    
    /* Body */
    set_current_block(cg, body_block);
    codegen_stmt(cg, stmt->data.do_stmt.body);
    if (!block_has_terminator(cg)) {
        anvil_build_br(cg->anvil_ctx, cond_block);
    }
    
    /* Condition */
    set_current_block(cg, cond_block);
    anvil_value_t *cond = mcc_codegen_expr(cg, stmt->data.do_stmt.cond);
    anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
    anvil_value_t *cond_bool = anvil_build_cmp_ne(cg->anvil_ctx, cond, zero, "cond");
    anvil_build_br_cond(cg->anvil_ctx, cond_bool, body_block, end_block);
    
    set_current_block(cg, end_block);
    cg->break_target = old_break;
    cg->continue_target = old_continue;
}

void mcc_codegen_for_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    anvil_block_t *cond_block = anvil_block_create(cg->current_func, "for.cond");
    anvil_block_t *body_block = anvil_block_create(cg->current_func, "for.body");
    anvil_block_t *incr_block = anvil_block_create(cg->current_func, "for.incr");
    anvil_block_t *end_block = anvil_block_create(cg->current_func, "for.end");
    
    anvil_block_t *old_break = cg->break_target;
    anvil_block_t *old_continue = cg->continue_target;
    cg->break_target = end_block;
    cg->continue_target = incr_block;
    
    /* Init */
    if (stmt->data.for_stmt.init) {
        mcc_codegen_expr(cg, stmt->data.for_stmt.init);
    }
    anvil_build_br(cg->anvil_ctx, cond_block);
    
    /* Condition */
    set_current_block(cg, cond_block);
    if (stmt->data.for_stmt.cond) {
        anvil_value_t *cond = mcc_codegen_expr(cg, stmt->data.for_stmt.cond);
        anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
        anvil_value_t *cond_bool = anvil_build_cmp_ne(cg->anvil_ctx, cond, zero, "cond");
        anvil_build_br_cond(cg->anvil_ctx, cond_bool, body_block, end_block);
    } else {
        anvil_build_br(cg->anvil_ctx, body_block);
    }
    
    /* Body */
    set_current_block(cg, body_block);
    codegen_stmt(cg, stmt->data.for_stmt.body);
    if (!block_has_terminator(cg)) {
        anvil_build_br(cg->anvil_ctx, incr_block);
    }
    
    /* Increment */
    set_current_block(cg, incr_block);
    if (stmt->data.for_stmt.incr) {
        mcc_codegen_expr(cg, stmt->data.for_stmt.incr);
    }
    anvil_build_br(cg->anvil_ctx, cond_block);
    
    set_current_block(cg, end_block);
    cg->break_target = old_break;
    cg->continue_target = old_continue;
}

void mcc_codegen_switch_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    anvil_value_t *val = mcc_codegen_expr(cg, stmt->data.switch_stmt.expr);
    anvil_block_t *end_block = anvil_block_create(cg->current_func, "switch.end");
    
    anvil_block_t *old_break = cg->break_target;
    cg->break_target = end_block;
    
    /* For now, generate as if-else chain */
    /* A proper implementation would use a switch instruction or jump table */
    
    codegen_stmt(cg, stmt->data.switch_stmt.body);
    
    if (!block_has_terminator(cg)) {
        anvil_build_br(cg->anvil_ctx, end_block);
    }
    
    set_current_block(cg, end_block);
    cg->break_target = old_break;
    (void)val;
}

void mcc_codegen_return_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    if (stmt->data.return_stmt.expr) {
        anvil_value_t *val = mcc_codegen_expr(cg, stmt->data.return_stmt.expr);
        anvil_build_ret(cg->anvil_ctx, val);
    } else {
        anvil_build_ret_void(cg->anvil_ctx);
    }
}

/* Generate code for function */
void mcc_codegen_func(mcc_codegen_t *cg, mcc_ast_node_t *func)
{
    if (!func->data.func_decl.is_definition) {
        /* Just a declaration, no code to generate */
        return;
    }
    
    /* Create function type */
    anvil_type_t *ret_type = mcc_codegen_type(cg, func->data.func_decl.func_type);
    int num_params = (int)func->data.func_decl.num_params;
    anvil_type_t **param_types = NULL;
    
    if (num_params > 0) {
        param_types = mcc_alloc(cg->mcc_ctx, num_params * sizeof(anvil_type_t*));
        for (int i = 0; i < num_params; i++) {
            mcc_ast_node_t *p = func->data.func_decl.params[i];
            param_types[i] = mcc_codegen_type(cg, p->data.param_decl.param_type);
        }
    }
    
    anvil_type_t *func_type = anvil_type_func(cg->anvil_ctx, ret_type, param_types,
                                               num_params, false);
    
    /* Create function */
    anvil_linkage_t linkage = func->data.func_decl.is_static ? 
        ANVIL_LINK_INTERNAL : ANVIL_LINK_EXTERNAL;
    cg->current_func = anvil_func_create(cg->anvil_mod, func->data.func_decl.name, 
                                          func_type, linkage);
    
    /* Register function in mapping */
    mcc_symbol_t *func_sym = mcc_symtab_lookup(cg->symtab, func->data.func_decl.name);
    if (func_sym) {
        add_func(cg, func_sym, cg->current_func);
    }
    
    /* Create entry block */
    anvil_block_t *entry = anvil_func_get_entry(cg->current_func);
    set_current_block(cg, entry);
    
    /* Reset locals */
    cg->num_locals = 0;
    cg->num_labels = 0;
    
    /* Allocate space for parameters and add to locals */
    for (int i = 0; i < num_params; i++) {
        mcc_ast_node_t *p = func->data.func_decl.params[i];
        if (p->data.param_decl.name) {
            anvil_value_t *param = anvil_func_get_param(cg->current_func, i);
            anvil_value_t *alloca_val = anvil_build_alloca(cg->anvil_ctx, param_types[i], 
                                                           p->data.param_decl.name);
            anvil_build_store(cg->anvil_ctx, param, alloca_val);
            
            /* Add parameter to locals by name */
            add_local(cg, p->data.param_decl.name, alloca_val);
        }
    }
    
    /* Generate body */
    codegen_stmt(cg, func->data.func_decl.body);
    
    /* Add implicit return if needed */
    if (!block_has_terminator(cg)) {
        if (ret_type == anvil_type_void(cg->anvil_ctx)) {
            anvil_build_ret_void(cg->anvil_ctx);
        } else {
            anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
            anvil_build_ret(cg->anvil_ctx, zero);
        }
    }
    
    cg->current_func = NULL;
    cg->current_block = NULL;
}

/* Generate code for global variable */
void mcc_codegen_global_var(mcc_codegen_t *cg, mcc_ast_node_t *var)
{
    anvil_type_t *type = mcc_codegen_type(cg, var->data.var_decl.var_type);
    
    anvil_linkage_t linkage = var->data.var_decl.is_static ?
        ANVIL_LINK_INTERNAL : ANVIL_LINK_EXTERNAL;
    
    anvil_module_add_global(cg->anvil_mod, var->data.var_decl.name, type, linkage);
}

/* Generate code for declaration */
void mcc_codegen_decl(mcc_codegen_t *cg, mcc_ast_node_t *decl)
{
    if (!decl) return;
    
    switch (decl->kind) {
        case AST_FUNC_DECL:
            mcc_codegen_func(cg, decl);
            break;
        case AST_VAR_DECL:
            mcc_codegen_global_var(cg, decl);
            break;
        case AST_DECL_LIST:
            /* Multiple declarations: int a, b, c; */
            for (size_t i = 0; i < decl->data.decl_list.num_decls; i++) {
                mcc_codegen_decl(cg, decl->data.decl_list.decls[i]);
            }
            break;
        default:
            break;
    }
}

/* Main code generation entry point */
bool mcc_codegen_generate(mcc_codegen_t *cg, mcc_ast_node_t *ast)
{
    if (!ast || ast->kind != AST_TRANSLATION_UNIT) {
        return false;
    }
    
    /* Create module */
    cg->anvil_mod = anvil_module_create(cg->anvil_ctx, "mcc_output");
    
    /* Generate code for all declarations */
    for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
        mcc_codegen_decl(cg, ast->data.translation_unit.decls[i]);
    }
    
    return !mcc_has_errors(cg->mcc_ctx);
}

/* Add AST from another file to the same module (multi-file support) */
bool mcc_codegen_add_ast(mcc_codegen_t *cg, mcc_ast_node_t *ast)
{
    if (!ast || ast->kind != AST_TRANSLATION_UNIT) {
        return false;
    }
    
    /* Create module if not already created */
    if (!cg->anvil_mod) {
        cg->anvil_mod = anvil_module_create(cg->anvil_ctx, "mcc_output");
    }
    
    /* Generate code for all declarations in this AST */
    for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
        mcc_codegen_decl(cg, ast->data.translation_unit.decls[i]);
    }
    
    return !mcc_has_errors(cg->mcc_ctx);
}

/* Finalize code generation after all files have been added */
bool mcc_codegen_finalize(mcc_codegen_t *cg)
{
    /* Currently nothing special to do - module is ready for codegen */
    /* Future: could add link-time optimizations, symbol resolution checks, etc. */
    return !mcc_has_errors(cg->mcc_ctx);
}

/* Get generated assembly output */
char *mcc_codegen_get_output(mcc_codegen_t *cg, size_t *len)
{
    if (!cg->anvil_mod) {
        *len = 0;
        return NULL;
    }
    
    /* Generate code */
    char *output = NULL;
    anvil_error_t err = anvil_module_codegen(cg->anvil_mod, &output, len);
    if (err != ANVIL_OK) {
        *len = 0;
        return NULL;
    }
    
    return output;
}
