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
                anvil_value_t *init = codegen_expr(cg, stmt->data.var_decl.init);
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
    
    /* Compare to zero */
    anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
    anvil_value_t *cond_bool = anvil_build_cmp_ne(cg->anvil_ctx, cond, zero, "cond");
    
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
    anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
    anvil_value_t *cond_bool = anvil_build_cmp_ne(cg->anvil_ctx, cond, zero, "cond");
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
    anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
    anvil_value_t *cond_bool = anvil_build_cmp_ne(cg->anvil_ctx, cond, zero, "cond");
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
        anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
        anvil_value_t *cond_bool = anvil_build_cmp_ne(cg->anvil_ctx, cond, zero, "cond");
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

void codegen_switch_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    anvil_value_t *val = codegen_expr(cg, stmt->data.switch_stmt.expr);
    anvil_block_t *end_block = anvil_block_create(cg->current_func, "switch.end");
    
    anvil_block_t *old_break = cg->break_target;
    cg->break_target = end_block;
    
    /* For now, generate as if-else chain */
    /* A proper implementation would use a switch instruction or jump table */
    
    codegen_stmt(cg, stmt->data.switch_stmt.body);
    
    if (!codegen_block_has_terminator(cg)) {
        anvil_build_br(cg->anvil_ctx, end_block);
    }
    
    codegen_set_current_block(cg, end_block);
    cg->break_target = old_break;
    (void)val;
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
