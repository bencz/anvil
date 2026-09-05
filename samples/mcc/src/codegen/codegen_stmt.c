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
            if (!codegen_block_has_terminator(cg)) {
                anvil_build_br(cg->anvil_ctx, label_block);
            }
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
            mcc_type_t *var_type = stmt->data.var_decl.var_type;
            anvil_type_t *type = codegen_type(cg, var_type);
            anvil_value_t *alloca_val;

            /* VLA: dimensions use runtime expressions. Emit
             * anvil_build_alloca_dyn with the count so the backend bumps
             * sp at runtime. The alloca is typed with the element type
             * — indexing still works because arrays decay to pointers
             * and codegen_lvalue walks the stack slot directly. */
            if (var_type->kind == TYPE_ARRAY && var_type->data.array.is_vla &&
                var_type->data.array.length_expr) {
                mcc_type_t *elem_type = var_type->data.array.element;
                anvil_type_t *anvil_elem = codegen_type(cg, elem_type);
                anvil_value_t *count = codegen_expr(cg, var_type->data.array.length_expr);
                alloca_val = anvil_build_alloca_dyn(cg->anvil_ctx, anvil_elem,
                                                    count, stmt->data.var_decl.name);
            } else {
                alloca_val = anvil_build_alloca(cg->anvil_ctx, type, stmt->data.var_decl.name);
            }

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
                    for (size_t i = 0; i < init_node->data.init_list.num_exprs; i++) {
                        /* Get pointer to element */
                        anvil_value_t *indices[2];
                        indices[0] = anvil_const_i64(cg->anvil_ctx, 0);
                        indices[1] = anvil_const_i64(cg->anvil_ctx, (int64_t)i);
                        anvil_value_t *elem_ptr = anvil_build_gep(cg->anvil_ctx, 
                            type, alloca_val, indices, 2, "elem");
                        
                        /* Generate element value */
                        mcc_ast_node_t *elem_node = init_node->data.init_list.exprs[i];
                        anvil_value_t *elem_val = codegen_expr(cg, elem_node);
                        elem_val = codegen_convert_value(cg, elem_val,
                                                         elem_node->type,
                                                         elem_type,
                                                         "init.cast");
                        if (elem_val) {
                            codegen_store_object(cg, elem_type, elem_val, elem_ptr);
                        }
                    }
                } else {
                    anvil_value_t *init = codegen_expr(cg, init_node);
                    init = codegen_convert_value(cg, init,
                                                 init_node->type,
                                                 stmt->data.var_decl.var_type,
                                                 "init.cast");
                    if (init) {
                        codegen_store_object(cg, stmt->data.var_decl.var_type, init, alloca_val);
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
    size_t saved_num_locals = cg->num_locals;
    for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
        mcc_ast_node_t *child = stmt->data.compound_stmt.stmts[i];
        /* A terminator ends ordinary sequential code. Labels remain valid
         * re-entry points and switch insertion to their dedicated blocks. */
        if (codegen_block_has_terminator(cg) && child &&
            child->kind != AST_LABEL_STMT &&
            child->kind != AST_CASE_STMT &&
            child->kind != AST_DEFAULT_STMT) {
            continue;
        }
        codegen_stmt(cg, child);
    }
    cg->num_locals = saved_num_locals;
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
    /* Without an else the false edge needs a merge immediately.  With an
     * else, defer creating it until we know that at least one arm falls
     * through; otherwise `if (...) return; else return;` would leave an
     * unreachable empty block, which is invalid ANVIL IR. */
    anvil_block_t *end_block = else_block ? NULL
        : anvil_block_create(cg->current_func, end_name);
    
    /* Convert to boolean if not already */
    anvil_value_t *cond_bool = codegen_to_bool(cg, cond);
    
    anvil_build_br_cond(cg->anvil_ctx, cond_bool, then_block,
                        else_block ? else_block : end_block);
    
    /* Then block */
    codegen_set_current_block(cg, then_block);
    codegen_stmt(cg, stmt->data.if_stmt.then_stmt);
    anvil_block_t *then_exit = cg->current_block;
    bool then_falls_through = !codegen_block_has_terminator(cg);
    
    /* Else block */
    if (else_block) {
        codegen_set_current_block(cg, else_block);
        codegen_stmt(cg, stmt->data.if_stmt.else_stmt);
        anvil_block_t *else_exit = cg->current_block;
        bool else_falls_through = !codegen_block_has_terminator(cg);

        if (then_falls_through || else_falls_through) {
            end_block = anvil_block_create(cg->current_func, end_name);
            if (then_falls_through) {
                codegen_set_current_block(cg, then_exit);
                anvil_build_br(cg->anvil_ctx, end_block);
            }
            if (else_falls_through) {
                codegen_set_current_block(cg, else_exit);
                anvil_build_br(cg->anvil_ctx, end_block);
            }
        }
    } else if (then_falls_through) {
        codegen_set_current_block(cg, then_exit);
        anvil_build_br(cg->anvil_ctx, end_block);
    }

    if (end_block) codegen_set_current_block(cg, end_block);
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
    /* A declaration in the initializer belongs to the for statement's
     * enclosing scope, including its condition, increment, and body, but not
     * statements that follow the loop. */
    size_t saved_num_locals = cg->num_locals;
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
    cg->num_locals = saved_num_locals;
    cg->break_target = old_break;
    cg->continue_target = old_continue;
}

/* Count first so switch lowering can allocate once and cannot leave a raw
 * realloc result unchecked. */
static bool count_cases(mcc_codegen_t *cg, mcc_ast_node_t *node,
                        size_t *num_cases, mcc_ast_node_t **default_case)
{
    if (!node) return true;
    
    if (node->kind == AST_CASE_STMT) {
        if (*num_cases == SIZE_MAX) {
            mcc_error(cg->mcc_ctx, "too many switch cases");
            return false;
        }
        (*num_cases)++;
        return count_cases(cg, node->data.case_stmt.stmt, num_cases, default_case);
    } else if (node->kind == AST_DEFAULT_STMT) {
        *default_case = node;
        return count_cases(cg, node->data.default_stmt.stmt, num_cases, default_case);
    } else if (node->kind == AST_COMPOUND_STMT) {
        for (size_t i = 0; i < node->data.compound_stmt.num_stmts; i++) {
            if (!count_cases(cg, node->data.compound_stmt.stmts[i],
                             num_cases, default_case)) return false;
        }
    }
    return true;
}

static void fill_cases(mcc_ast_node_t *node, mcc_ast_node_t **cases,
                       size_t *case_index)
{
    if (!node) return;
    if (node->kind == AST_CASE_STMT) {
        cases[(*case_index)++] = node;
        fill_cases(node->data.case_stmt.stmt, cases, case_index);
    } else if (node->kind == AST_DEFAULT_STMT) {
        fill_cases(node->data.default_stmt.stmt, cases, case_index);
    } else if (node->kind == AST_COMPOUND_STMT) {
        for (size_t i = 0; i < node->data.compound_stmt.num_stmts; i++) {
            fill_cases(node->data.compound_stmt.stmts[i], cases, case_index);
        }
    }
}

static bool switch_body_has_break(mcc_ast_node_t *node)
{
    if (!node) return false;
    if (node->kind == AST_BREAK_STMT) return true;
    /* A break nested in another loop/switch does not target this switch. */
    if (node->kind == AST_SWITCH_STMT || node->kind == AST_WHILE_STMT ||
        node->kind == AST_DO_STMT || node->kind == AST_FOR_STMT) return false;
    if (node->kind == AST_COMPOUND_STMT) {
        for (size_t i = 0; i < node->data.compound_stmt.num_stmts; i++) {
            if (switch_body_has_break(node->data.compound_stmt.stmts[i])) return true;
        }
    } else if (node->kind == AST_IF_STMT) {
        return switch_body_has_break(node->data.if_stmt.then_stmt) ||
               switch_body_has_break(node->data.if_stmt.else_stmt);
    } else if (node->kind == AST_CASE_STMT) {
        return switch_body_has_break(node->data.case_stmt.stmt);
    } else if (node->kind == AST_DEFAULT_STMT) {
        return switch_body_has_break(node->data.default_stmt.stmt);
    }
    return false;
}

static bool stmt_always_terminates(mcc_ast_node_t *node)
{
    if (!node) return false;
    if (node->kind == AST_RETURN_STMT || node->kind == AST_GOTO_STMT ||
        node->kind == AST_BREAK_STMT || node->kind == AST_CONTINUE_STMT) {
        return true;
    }
    if (node->kind == AST_COMPOUND_STMT) {
        size_t n = node->data.compound_stmt.num_stmts;
        return n > 0 && stmt_always_terminates(
            node->data.compound_stmt.stmts[n - 1]);
    }
    if (node->kind == AST_IF_STMT) {
        return node->data.if_stmt.else_stmt &&
               stmt_always_terminates(node->data.if_stmt.then_stmt) &&
               stmt_always_terminates(node->data.if_stmt.else_stmt);
    }
    return false;
}

static void emit_switch_body(mcc_codegen_t *cg, mcc_ast_node_t *node, mcc_ast_node_t **cases,
                             anvil_block_t **blocks, size_t count, anvil_block_t *default_block, bool *started)
{
    if (!node)
        return;

    if (node->kind == AST_COMPOUND_STMT)
    {
        for (size_t index = 0; index < node->data.compound_stmt.num_stmts; index++)
            emit_switch_body(cg, node->data.compound_stmt.stmts[index], cases, blocks, count, default_block, started);

        return;
    }

    anvil_block_t *destination = NULL;
    mcc_ast_node_t *statement = NULL;
    if (node->kind == AST_CASE_STMT)
    {
        for (size_t index = 0; index < count; index++)
        {
            if (cases[index] == node)
            {
                destination = blocks[index];
                break;
            }
        }

        statement = node->data.case_stmt.stmt;
    }
    else if (node->kind == AST_DEFAULT_STMT)
    {
        destination = default_block;
        statement = node->data.default_stmt.stmt;
    }
    else
    {
        if (*started)
            codegen_stmt(cg, node);

        return;
    }

    if (!destination)
    {
        mcc_error(cg->mcc_ctx, "switch label has no destination");
        return;
    }

    if (*started && !codegen_block_has_terminator(cg))
        anvil_build_br(cg->anvil_ctx, destination);

    codegen_set_current_block(cg, destination);
    *started = true;
    emit_switch_body(cg, statement, cases, blocks, count, default_block, started);
}

void codegen_switch_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    size_t num_cases = 0;
    mcc_ast_node_t *default_case = NULL;
    if (!count_cases(cg, stmt->data.switch_stmt.body, &num_cases,
                     &default_case)) return;
    if (num_cases > SIZE_MAX / sizeof(mcc_ast_node_t *) ||
        num_cases > SIZE_MAX / sizeof(anvil_block_t *)) {
        mcc_error(cg->mcc_ctx, "switch case table size overflow");
        return;
    }

    mcc_ast_node_t **cases = NULL;
    anvil_block_t **case_blocks = NULL;
    anvil_block_t **cmp_blocks = NULL;
    if (num_cases > 0) {
        cases = mcc_alloc(cg->mcc_ctx,
                          num_cases * sizeof(mcc_ast_node_t *));
        case_blocks = mcc_alloc(cg->mcc_ctx,
                                num_cases * sizeof(anvil_block_t *));
        cmp_blocks = mcc_alloc(cg->mcc_ctx,
                               num_cases * sizeof(anvil_block_t *));
        if (!cases || !case_blocks || !cmp_blocks) return;
        size_t case_index = 0;
        fill_cases(stmt->data.switch_stmt.body, cases, &case_index);
    }

    int id = cg->label_counter++;
    char end_name[32];
    snprintf(end_name, sizeof(end_name), "switch%d.end", id);
    
    /* Generate switch expression and store in a local variable so it persists across blocks */
    mcc_type_t *switch_c_type = stmt->data.switch_stmt.expr->type;
    if (switch_c_type && mcc_type_is_integer(switch_c_type)) {
        switch_c_type = mcc_type_promote(cg->types, switch_c_type);
    }
    anvil_value_t *switch_expr = codegen_expr(cg, stmt->data.switch_stmt.expr);
    switch_expr = codegen_convert_value(cg, switch_expr,
                                        stmt->data.switch_stmt.expr->type,
                                        switch_c_type,
                                        "switch.cast");
    anvil_type_t *switch_type = codegen_type(cg, switch_c_type);
    anvil_value_t *switch_ptr = anvil_build_alloca(cg->anvil_ctx, switch_type, "switch.val");
    anvil_build_store(cg->anvil_ctx, switch_expr, switch_ptr);
    
    /* ANVIL rejects unreachable blocks.  A fully returning switch with a
     * default has no path to a merge block, so do not manufacture one. */
    bool need_end = !default_case ||
                    switch_body_has_break(stmt->data.switch_stmt.body) ||
                    !stmt_always_terminates(default_case->data.default_stmt.stmt);
    anvil_block_t *end_block = need_end
        ? anvil_block_create(cg->current_func, end_name) : NULL;

    anvil_block_t *old_break = cg->break_target;
    cg->break_target = end_block;
    
    /* Create blocks for each case */
    if (num_cases > 0) {
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
    if (num_cases > 0) {
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
        case_val = codegen_convert_value(cg, case_val,
                                         case_node->data.case_stmt.expr->type,
                                         switch_c_type,
                                         "case.cast");

        anvil_block_t *next_block;
        if (i + 1 < num_cases) {
            next_block = cmp_blocks[i + 1];
        } else if (default_block) {
            next_block = default_block;
        } else {
            next_block = end_block;
        }

        if (case_node->data.case_stmt.end_expr) {
            /* GNU case range: `case LO ... HI:` matches when
             * switch_val >= LO && switch_val <= HI. */
            anvil_value_t *hi_val = codegen_expr(cg, case_node->data.case_stmt.end_expr);
            hi_val = codegen_convert_value(cg, hi_val,
                                           case_node->data.case_stmt.end_expr->type,
                                           switch_c_type,
                                           "case.cast");
            anvil_value_t *ge = anvil_build_cmp_ge(cg->anvil_ctx, switch_val, case_val, "ge");
            anvil_value_t *le = anvil_build_cmp_le(cg->anvil_ctx, switch_val, hi_val, "le");
            anvil_value_t *in_range = anvil_build_and(cg->anvil_ctx, ge, le, "range");
            anvil_build_br_cond(cg->anvil_ctx, in_range, case_blocks[i], next_block);
        } else {
            anvil_value_t *cmp = anvil_build_cmp_eq(cg->anvil_ctx, switch_val, case_val, "cmp");
            anvil_build_br_cond(cg->anvil_ctx, cmp, case_blocks[i], next_block);
        }
    }
    
    /* Emit labels in source order, including stacked case/default labels.
     * Fallthrough belongs to the live tail of the previous statement. */
    bool started = false;
    emit_switch_body(cg, stmt->data.switch_stmt.body, cases, case_blocks, num_cases, default_block, &started);
    if (started && !codegen_block_has_terminator(cg) && end_block)
        anvil_build_br(cg->anvil_ctx, end_block);
    
    if (end_block) codegen_set_current_block(cg, end_block);
    cg->break_target = old_break;
    
}

void codegen_return_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    if (stmt->data.return_stmt.expr) {
        anvil_value_t *val = codegen_expr(cg, stmt->data.return_stmt.expr);
        if (codegen_type_is_record(cg->current_return_type))
        {
            anvil_abi_value_plan_t plan;
            if (!codegen_abi_plan(cg, cg->current_return_type, true, &plan))
                return;

            if (cg->current_result_pointer)
            {
                if (!codegen_copy_object(cg, cg->current_return_type, val, cg->current_result_pointer, NULL))
                    return;

                anvil_build_ret(cg->anvil_ctx, cg->current_result_pointer);
            }
            else
            {
                val = codegen_abi_pack(cg, cg->current_return_type, val, &plan);
                anvil_build_ret(cg->anvil_ctx, val);
            }
            return;
        }

        val = codegen_convert_value(cg, val,
                                    stmt->data.return_stmt.expr->type,
                                    cg->current_return_type,
                                    "return.cast");
        anvil_build_ret(cg->anvil_ctx, val);
    } else {
        anvil_build_ret_void(cg->anvil_ctx);
    }
}
