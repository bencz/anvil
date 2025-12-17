/*
 * MCC - Micro C Compiler
 * Code Generator - Statement Generation
 * 
 * This file handles code generation for all statement types.
 */

#include "codegen_internal.h"

/* Generate code for any statement */
void codegen_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    if (!stmt) return;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            codegen_compound_stmt(cg, stmt);
            break;
            
        case AST_EXPR_STMT:
            if (stmt->data.expr_stmt.expr) {
                codegen_expr(cg, stmt->data.expr_stmt.expr);
            }
            break;
            
        case AST_IF_STMT:
            codegen_if_stmt(cg, stmt);
            break;
            
        case AST_WHILE_STMT:
            codegen_while_stmt(cg, stmt);
            break;
            
        case AST_DO_STMT:
            codegen_do_stmt(cg, stmt);
            break;
            
        case AST_FOR_STMT:
            codegen_for_stmt(cg, stmt);
            break;
            
        case AST_SWITCH_STMT:
            codegen_switch_stmt(cg, stmt);
            break;
            
        case AST_RETURN_STMT:
            codegen_return_stmt(cg, stmt);
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
            anvil_block_t *target = codegen_get_label_block(cg, stmt->data.goto_stmt.label);
            anvil_build_br(cg->anvil_ctx, target);
            break;
        }
        
        case AST_LABEL_STMT: {
            anvil_block_t *label_block = codegen_get_label_block(cg, stmt->data.label_stmt.label);
            anvil_build_br(cg->anvil_ctx, label_block);
            codegen_set_current_block(cg, label_block);
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
            anvil_type_t *type = codegen_type(cg, stmt->data.var_decl.var_type);
            anvil_value_t *alloca_val = anvil_build_alloca(cg->anvil_ctx, type, stmt->data.var_decl.name);
            
            /* Add to locals by name */
            codegen_add_local(cg, stmt->data.var_decl.name, alloca_val);
            
            /* Initialize if needed */
            if (stmt->data.var_decl.init) {
                mcc_ast_node_t *init_node = stmt->data.var_decl.init;
                
                /* Handle array initializer lists specially */
                if (init_node->kind == AST_INIT_LIST && 
                    stmt->data.var_decl.var_type->kind == TYPE_ARRAY) {
                    /* Initialize each element of the array */
                    mcc_type_t *elem_type = stmt->data.var_decl.var_type->data.array.element;
                    anvil_type_t *anvil_elem_type = codegen_type(cg, elem_type);
                    
                    for (size_t i = 0; i < init_node->data.init_list.num_exprs; i++) {
                        /* Get pointer to element */
                        anvil_value_t *indices[1];
                        indices[0] = anvil_const_i64(cg->anvil_ctx, (int64_t)i);
                        anvil_value_t *elem_ptr = anvil_build_gep(cg->anvil_ctx, 
                            anvil_elem_type, alloca_val, indices, 1, "elem");
                        
                        /* Generate element value */
                        anvil_value_t *elem_val = codegen_expr(cg, init_node->data.init_list.exprs[i]);
                        if (elem_val) {
                            anvil_build_store(cg->anvil_ctx, elem_val, elem_ptr);
                        }
                    }
                } else {
                    anvil_value_t *init = codegen_expr(cg, init_node);
                    if (init) {
                        anvil_build_store(cg->anvil_ctx, init, alloca_val);
                    }
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

void codegen_compound_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
        codegen_stmt(cg, stmt->data.compound_stmt.stmts[i]);
    }
}

void codegen_if_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    anvil_value_t *cond = codegen_expr(cg, stmt->data.if_stmt.cond);
    
    int id = cg->label_counter++;
    char then_name[32], else_name[32], end_name[32];
    snprintf(then_name, sizeof(then_name), "if%d.then", id);
    snprintf(else_name, sizeof(else_name), "if%d.else", id);
    snprintf(end_name, sizeof(end_name), "if%d.end", id);
    
    anvil_block_t *then_block = anvil_block_create(cg->current_func, then_name);
    anvil_block_t *else_block = stmt->data.if_stmt.else_stmt ?
        anvil_block_create(cg->current_func, else_name) : NULL;
    anvil_block_t *end_block = anvil_block_create(cg->current_func, end_name);
    
    /* Convert to boolean if not already */
    anvil_value_t *cond_bool = codegen_to_bool(cg, cond);
    
    anvil_build_br_cond(cg->anvil_ctx, cond_bool, then_block,
                        else_block ? else_block : end_block);
    
    /* Then block */
    codegen_set_current_block(cg, then_block);
    codegen_stmt(cg, stmt->data.if_stmt.then_stmt);
    if (!codegen_block_has_terminator(cg)) {
        anvil_build_br(cg->anvil_ctx, end_block);
    }
    
    /* Else block */
    if (else_block) {
        codegen_set_current_block(cg, else_block);
        codegen_stmt(cg, stmt->data.if_stmt.else_stmt);
        if (!codegen_block_has_terminator(cg)) {
            anvil_build_br(cg->anvil_ctx, end_block);
        }
    }
    
    codegen_set_current_block(cg, end_block);
}

void codegen_while_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    int id = cg->label_counter++;
    char cond_name[32], body_name[32], end_name[32];
    snprintf(cond_name, sizeof(cond_name), "while%d.cond", id);
    snprintf(body_name, sizeof(body_name), "while%d.body", id);
    snprintf(end_name, sizeof(end_name), "while%d.end", id);
    
    anvil_block_t *cond_block = anvil_block_create(cg->current_func, cond_name);
    anvil_block_t *body_block = anvil_block_create(cg->current_func, body_name);
    anvil_block_t *end_block = anvil_block_create(cg->current_func, end_name);
    
    anvil_block_t *old_break = cg->break_target;
    anvil_block_t *old_continue = cg->continue_target;
    cg->break_target = end_block;
    cg->continue_target = cond_block;
    
    anvil_build_br(cg->anvil_ctx, cond_block);
    
    /* Condition */
    codegen_set_current_block(cg, cond_block);
    anvil_value_t *cond = codegen_expr(cg, stmt->data.while_stmt.cond);
    /* Convert to boolean if not already (comparison results are already boolean) */
    anvil_value_t *cond_bool = codegen_to_bool(cg, cond);
    anvil_build_br_cond(cg->anvil_ctx, cond_bool, body_block, end_block);
    
    /* Body */
    codegen_set_current_block(cg, body_block);
    codegen_stmt(cg, stmt->data.while_stmt.body);
    if (!codegen_block_has_terminator(cg)) {
        anvil_build_br(cg->anvil_ctx, cond_block);
    }
    
    codegen_set_current_block(cg, end_block);
    cg->break_target = old_break;
    cg->continue_target = old_continue;
}

void codegen_do_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    int id = cg->label_counter++;
    char body_name[32], cond_name[32], end_name[32];
    snprintf(body_name, sizeof(body_name), "do%d.body", id);
    snprintf(cond_name, sizeof(cond_name), "do%d.cond", id);
    snprintf(end_name, sizeof(end_name), "do%d.end", id);
    
    anvil_block_t *body_block = anvil_block_create(cg->current_func, body_name);
    anvil_block_t *cond_block = anvil_block_create(cg->current_func, cond_name);
    anvil_block_t *end_block = anvil_block_create(cg->current_func, end_name);
    
    anvil_block_t *old_break = cg->break_target;
    anvil_block_t *old_continue = cg->continue_target;
    cg->break_target = end_block;
    cg->continue_target = cond_block;
    
    anvil_build_br(cg->anvil_ctx, body_block);
    
    /* Body */
    codegen_set_current_block(cg, body_block);
    codegen_stmt(cg, stmt->data.do_stmt.body);
    if (!codegen_block_has_terminator(cg)) {
        anvil_build_br(cg->anvil_ctx, cond_block);
    }
    
    /* Condition */
    codegen_set_current_block(cg, cond_block);
    anvil_value_t *cond = codegen_expr(cg, stmt->data.do_stmt.cond);
    anvil_value_t *cond_bool = codegen_to_bool(cg, cond);
    anvil_build_br_cond(cg->anvil_ctx, cond_bool, body_block, end_block);
    
    codegen_set_current_block(cg, end_block);
    cg->break_target = old_break;
    cg->continue_target = old_continue;
}

void codegen_for_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    int id = cg->label_counter++;
    char cond_name[32], body_name[32], incr_name[32], end_name[32];
    snprintf(cond_name, sizeof(cond_name), "for%d.cond", id);
    snprintf(body_name, sizeof(body_name), "for%d.body", id);
    snprintf(incr_name, sizeof(incr_name), "for%d.incr", id);
    snprintf(end_name, sizeof(end_name), "for%d.end", id);
    
    anvil_block_t *cond_block = anvil_block_create(cg->current_func, cond_name);
    anvil_block_t *body_block = anvil_block_create(cg->current_func, body_name);
    anvil_block_t *incr_block = anvil_block_create(cg->current_func, incr_name);
    anvil_block_t *end_block = anvil_block_create(cg->current_func, end_name);
    
    anvil_block_t *old_break = cg->break_target;
    anvil_block_t *old_continue = cg->continue_target;
    cg->break_target = end_block;
    cg->continue_target = incr_block;
    
    /* Init */
    if (stmt->data.for_stmt.init_decl) {
        codegen_stmt(cg, stmt->data.for_stmt.init_decl);
    } else if (stmt->data.for_stmt.init) {
        codegen_expr(cg, stmt->data.for_stmt.init);
    }
    anvil_build_br(cg->anvil_ctx, cond_block);
    
    /* Condition */
    codegen_set_current_block(cg, cond_block);
    if (stmt->data.for_stmt.cond) {
        anvil_value_t *cond = codegen_expr(cg, stmt->data.for_stmt.cond);
        anvil_value_t *cond_bool = codegen_to_bool(cg, cond);
        anvil_build_br_cond(cg->anvil_ctx, cond_bool, body_block, end_block);
    } else {
        anvil_build_br(cg->anvil_ctx, body_block);
    }
    
    /* Body */
    codegen_set_current_block(cg, body_block);
    codegen_stmt(cg, stmt->data.for_stmt.body);
    if (!codegen_block_has_terminator(cg)) {
        anvil_build_br(cg->anvil_ctx, incr_block);
    }
    
    /* Increment */
    codegen_set_current_block(cg, incr_block);
    if (stmt->data.for_stmt.incr) {
        codegen_expr(cg, stmt->data.for_stmt.incr);
    }
    anvil_build_br(cg->anvil_ctx, cond_block);
    
    codegen_set_current_block(cg, end_block);
    cg->break_target = old_break;
    cg->continue_target = old_continue;
}

/* Helper to collect case statements from switch body */
static void collect_cases(mcc_ast_node_t *node, mcc_ast_node_t ***cases, size_t *num_cases,
                          mcc_ast_node_t **default_case)
{
    if (!node) return;
    
    if (node->kind == AST_CASE_STMT) {
        size_t n = *num_cases;
        *cases = realloc(*cases, (n + 1) * sizeof(mcc_ast_node_t*));
        (*cases)[n] = node;
        (*num_cases)++;
    } else if (node->kind == AST_DEFAULT_STMT) {
        *default_case = node;
    } else if (node->kind == AST_COMPOUND_STMT) {
        for (size_t i = 0; i < node->data.compound_stmt.num_stmts; i++) {
            collect_cases(node->data.compound_stmt.stmts[i], cases, num_cases, default_case);
        }
    }
}

void codegen_switch_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    int id = cg->label_counter++;
    char end_name[32];
    snprintf(end_name, sizeof(end_name), "switch%d.end", id);
    
    /* Generate switch expression and store in a local variable so it persists across blocks */
    anvil_value_t *switch_expr = codegen_expr(cg, stmt->data.switch_stmt.expr);
    anvil_type_t *switch_type = codegen_type(cg, stmt->data.switch_stmt.expr->type);
    anvil_value_t *switch_ptr = anvil_build_alloca(cg->anvil_ctx, switch_type, "switch.val");
    anvil_build_store(cg->anvil_ctx, switch_expr, switch_ptr);
    
    anvil_block_t *end_block = anvil_block_create(cg->current_func, end_name);
    
    anvil_block_t *old_break = cg->break_target;
    cg->break_target = end_block;
    
    /* Collect all case and default statements */
    mcc_ast_node_t **cases = NULL;
    size_t num_cases = 0;
    mcc_ast_node_t *default_case = NULL;
    collect_cases(stmt->data.switch_stmt.body, &cases, &num_cases, &default_case);
    
    /* Create blocks for each case */
    anvil_block_t **case_blocks = NULL;
    if (num_cases > 0) {
        case_blocks = malloc(num_cases * sizeof(anvil_block_t*));
        for (size_t i = 0; i < num_cases; i++) {
            char case_name[32];
            snprintf(case_name, sizeof(case_name), "switch%d.case%zu", id, i);
            case_blocks[i] = anvil_block_create(cg->current_func, case_name);
        }
    }
    
    anvil_block_t *default_block = NULL;
    if (default_case) {
        char def_name[32];
        snprintf(def_name, sizeof(def_name), "switch%d.default", id);
        default_block = anvil_block_create(cg->current_func, def_name);
    }
    
    /* Generate comparison chain - first create all comparison blocks */
    anvil_block_t **cmp_blocks = NULL;
    if (num_cases > 0) {
        cmp_blocks = malloc(num_cases * sizeof(anvil_block_t*));
        for (size_t i = 0; i < num_cases; i++) {
            char cmp_name[32];
            snprintf(cmp_name, sizeof(cmp_name), "switch%d.cmp%zu", id, i);
            cmp_blocks[i] = anvil_block_create(cg->current_func, cmp_name);
        }
        /* Branch to first comparison block */
        anvil_build_br(cg->anvil_ctx, cmp_blocks[0]);
    } else if (default_block) {
        anvil_build_br(cg->anvil_ctx, default_block);
    } else {
        anvil_build_br(cg->anvil_ctx, end_block);
    }
    
    /* Generate comparison code in each comparison block */
    for (size_t i = 0; i < num_cases; i++) {
        codegen_set_current_block(cg, cmp_blocks[i]);
        
        /* Load switch value from local variable */
        anvil_value_t *switch_val = anvil_build_load(cg->anvil_ctx, switch_type, switch_ptr, "switch.load");
        
        mcc_ast_node_t *case_node = cases[i];
        anvil_value_t *case_val = codegen_expr(cg, case_node->data.case_stmt.expr);
        anvil_value_t *cmp = anvil_build_cmp_eq(cg->anvil_ctx, switch_val, case_val, "cmp");
        
        anvil_block_t *next_block;
        if (i + 1 < num_cases) {
            next_block = cmp_blocks[i + 1];
        } else if (default_block) {
            next_block = default_block;
        } else {
            next_block = end_block;
        }
        
        anvil_build_br_cond(cg->anvil_ctx, cmp, case_blocks[i], next_block);
    }
    free(cmp_blocks);
    
    /* Generate code for switch body - process statements between cases */
    /* In C, case labels are just labels - statements continue until break/return */
    mcc_ast_node_t *body = stmt->data.switch_stmt.body;
    if (body && body->kind == AST_COMPOUND_STMT) {
        anvil_block_t *current_case_block = NULL;
        size_t case_idx = 0;
        
        for (size_t i = 0; i < body->data.compound_stmt.num_stmts; i++) {
            mcc_ast_node_t *s = body->data.compound_stmt.stmts[i];
            
            if (s->kind == AST_CASE_STMT) {
                /* Switch to this case's block */
                if (case_idx < num_cases) {
                    current_case_block = case_blocks[case_idx++];
                    codegen_set_current_block(cg, current_case_block);
                    /* Generate the case's direct statement if any */
                    if (s->data.case_stmt.stmt) {
                        codegen_stmt(cg, s->data.case_stmt.stmt);
                    }
                }
            } else if (s->kind == AST_DEFAULT_STMT) {
                /* Switch to default block */
                if (default_block) {
                    current_case_block = default_block;
                    codegen_set_current_block(cg, current_case_block);
                    if (s->data.default_stmt.stmt) {
                        codegen_stmt(cg, s->data.default_stmt.stmt);
                    }
                }
            } else if (current_case_block) {
                /* Regular statement - add to current case block */
                codegen_stmt(cg, s);
            }
        }
        
        /* Add fall-through branches for cases without terminators */
        for (size_t i = 0; i < num_cases; i++) {
            codegen_set_current_block(cg, case_blocks[i]);
            if (!codegen_block_has_terminator(cg)) {
                if (i + 1 < num_cases) {
                    anvil_build_br(cg->anvil_ctx, case_blocks[i + 1]);
                } else if (default_block) {
                    anvil_build_br(cg->anvil_ctx, default_block);
                } else {
                    anvil_build_br(cg->anvil_ctx, end_block);
                }
            }
        }
        
        /* Add fall-through for default if needed */
        if (default_block) {
            codegen_set_current_block(cg, default_block);
            if (!codegen_block_has_terminator(cg)) {
                anvil_build_br(cg->anvil_ctx, end_block);
            }
        }
    }
    
    codegen_set_current_block(cg, end_block);
    cg->break_target = old_break;
    
    free(cases);
    free(case_blocks);
}

void codegen_return_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    if (stmt->data.return_stmt.expr) {
        anvil_value_t *val = codegen_expr(cg, stmt->data.return_stmt.expr);
        anvil_build_ret(cg->anvil_ctx, val);
    } else {
        anvil_build_ret_void(cg->anvil_ctx);
    }
}
