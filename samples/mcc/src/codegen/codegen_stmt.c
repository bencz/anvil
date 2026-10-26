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
            
        case AST_BREAK_STMT: {
            anvil_block_t *target = cg->break_target
                ? cg->break_target : codegen_switch_end_block(cg);
            if (!target) {
                mcc_error_at(cg->mcc_ctx, stmt->location,
                             "break statement not within a loop or switch");
                break;
            }
            anvil_build_br(cg->anvil_ctx, target);
            break;
        }
            
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
        case AST_DEFAULT_STMT: {
            /* A switch label is an ordinary label of the innermost switch:
             * it may sit at any depth of the body (Duff's device). */
            anvil_block_t *target = codegen_switch_label_block(cg, stmt);
            if (!target) {
                mcc_error_at(cg->mcc_ctx, stmt->location,
                             "%s label not within a switch statement",
                             stmt->kind == AST_CASE_STMT ? "case" : "default");
                break;
            }
            if (!codegen_block_has_terminator(cg))
                anvil_build_br(cg->anvil_ctx, target);   /* fallthrough */
            codegen_set_current_block(cg, target);
            codegen_stmt(cg, stmt->kind == AST_CASE_STMT
                                 ? stmt->data.case_stmt.stmt
                                 : stmt->data.default_stmt.stmt);
            break;
        }
            
        case AST_NULL_STMT:
            break;
            
        case AST_VAR_DECL: {
            /* Local variable declaration */
            mcc_type_t *var_type = stmt->data.var_decl.var_type;
            anvil_type_t *type = codegen_type(cg, var_type);
            anvil_value_t *alloca_val;

            /* A block-scope extern names the file-scope object; references
             * resolve through its symbol, so no storage is created here. */
            if (stmt->data.var_decl.is_extern)
                break;

            /* A block-scope static has static storage duration: one internal
             * object initialized at load time, not a stack slot.  The '.'
             * keeps the symbol disjoint from every C identifier. */
            if (stmt->data.var_decl.is_static) {
                char symbol[256];
                snprintf(symbol, sizeof(symbol), "mcc.static.%d.%s",
                         cg->label_counter++, stmt->data.var_decl.name);
                anvil_value_t *storage = anvil_module_add_global(
                    cg->anvil_mod, symbol, type, ANVIL_LINK_INTERNAL);
                anvil_value_t *constant = codegen_const_initializer(
                    cg, var_type, stmt->data.var_decl.init);
                if (!storage || !constant ||
                    !anvil_global_set_initializer(storage, constant)) {
                    mcc_error_at(cg->mcc_ctx, stmt->location,
                                 "cannot emit static local '%s'",
                                 stmt->data.var_decl.name);
                    break;
                }
                codegen_add_local(cg, stmt->data.var_decl.name, storage);
                break;
            }

            /* VLA: dimensions use runtime expressions. Emit
             * anvil_build_alloca_dyn with the count so the backend bumps
             * sp at runtime. The alloca is typed with the element type
             * — indexing still works because arrays decay to pointers
             * and codegen_lvalue walks the stack slot directly. */
            if (codegen_type_has_vla(var_type)) {
                /* One contiguous run of innermost elements; every runtime
                 * dimension is evaluated once, here. */
                anvil_type_t *innermost = codegen_type(cg, codegen_vla_innermost(var_type));
                anvil_value_t *count = codegen_vla_bind(cg, var_type)
                    ? codegen_vla_element_count(cg, var_type) : NULL;
                if (!innermost || !count)
                    break;
                alloca_val = anvil_build_alloca_dyn(cg->anvil_ctx, innermost,
                                                    count, stmt->data.var_decl.name);
            } else {
                /* A variably modified type such as int (*)[n] evaluates its
                 * dimensions here, once (C11 6.7.6.2p5). */
                if (codegen_type_is_variably_modified(var_type) &&
                    !codegen_vla_bind(cg, var_type))
                    break;
                alloca_val = codegen_entry_alloca(cg, type, stmt->data.var_decl.name);
            }

            /* Add to locals by name */
            codegen_add_local(cg, stmt->data.var_decl.name, alloca_val);

            codegen_init_object(cg, var_type, alloca_val,
                                stmt->data.var_decl.init);
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

/* True when `node` holds a label that a jump can target: a goto label
 * anywhere, or a case/default label of the switch being generated. */
static bool stmt_contains_label(mcc_ast_node_t *node, bool in_nested_switch)
{
    if (!node) return false;
    switch (node->kind) {
        case AST_LABEL_STMT:
            return true;
        case AST_CASE_STMT:
            return !in_nested_switch ||
                   stmt_contains_label(node->data.case_stmt.stmt, true);
        case AST_DEFAULT_STMT:
            return !in_nested_switch ||
                   stmt_contains_label(node->data.default_stmt.stmt, true);
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < node->data.compound_stmt.num_stmts; i++)
                if (stmt_contains_label(node->data.compound_stmt.stmts[i],
                                        in_nested_switch))
                    return true;
            return false;
        case AST_IF_STMT:
            return stmt_contains_label(node->data.if_stmt.then_stmt,
                                       in_nested_switch) ||
                   stmt_contains_label(node->data.if_stmt.else_stmt,
                                       in_nested_switch);
        case AST_WHILE_STMT:
            return stmt_contains_label(node->data.while_stmt.body,
                                       in_nested_switch);
        case AST_DO_STMT:
            return stmt_contains_label(node->data.do_stmt.body,
                                       in_nested_switch);
        case AST_FOR_STMT:
            return stmt_contains_label(node->data.for_stmt.body,
                                       in_nested_switch);
        case AST_SWITCH_STMT:
            return stmt_contains_label(node->data.switch_stmt.body, true);
        default:
            return false;
    }
}

/* A declaration that sequential control flow never reaches can still be
 * entered by a jump to a later label in its scope (C11 6.8.6.1), so its
 * object must exist.  Its initializer is not evaluated. */
static void codegen_unreached_decl(mcc_codegen_t *cg, mcc_ast_node_t *decl)
{
    if (decl->kind == AST_DECL_LIST) {
        for (size_t i = 0; i < decl->data.decl_list.num_decls; i++)
            codegen_unreached_decl(cg, decl->data.decl_list.decls[i]);
        return;
    }
    if (decl->kind != AST_VAR_DECL || decl->data.var_decl.is_extern)
        return;
    if (decl->data.var_decl.is_static) {
        /* Static storage emits no instructions. */
        codegen_stmt(cg, decl);
        return;
    }
    mcc_type_t *type = decl->data.var_decl.var_type;
    if (codegen_type_has_vla(type)) {
        mcc_error_at(cg->mcc_ctx, decl->location,
                     "jump into the scope of variable-length array '%s'",
                     decl->data.var_decl.name);
        return;
    }
    anvil_value_t *storage = codegen_entry_alloca(
        cg, codegen_type(cg, type), decl->data.var_decl.name);
    if (storage)
        codegen_add_local(cg, decl->data.var_decl.name, storage);
}

void codegen_compound_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    size_t saved_num_locals = cg->num_locals;
    for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
        mcc_ast_node_t *child = stmt->data.compound_stmt.stmts[i];
        /* A terminator ends ordinary sequential code. Labels remain valid
         * re-entry points; declarations keep their storage. */
        if (codegen_block_has_terminator(cg) && child &&
            child->kind != AST_LABEL_STMT &&
            child->kind != AST_CASE_STMT &&
            child->kind != AST_DEFAULT_STMT) {
            if (child->kind == AST_VAR_DECL || child->kind == AST_DECL_LIST) {
                codegen_unreached_decl(cg, child);
            } else if (stmt_contains_label(child, false)) {
                /* Emit the statement in a block of its own so the labels
                 * inside stay reachable; the unreachable prefix is removed
                 * after the function is complete. */
                char name[32];
                snprintf(name, sizeof(name), "dead%d", cg->label_counter++);
                codegen_set_current_block(
                    cg, anvil_block_create(cg->current_func, name));
                codegen_stmt(cg, child);
            }
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

/* Collects the case/default labels that belong to a switch body, at any
 * depth, excluding the labels of nested switch statements.  With NULL
 * `cases` it only counts. */
static bool collect_switch_labels(mcc_codegen_t *cg, mcc_ast_node_t *node,
                                  mcc_ast_node_t **cases, size_t *num_cases,
                                  mcc_ast_node_t **default_case)
{
    if (!node) return true;
    switch (node->kind) {
        case AST_CASE_STMT:
            if (cases) {
                cases[*num_cases] = node;
            } else if (*num_cases == SIZE_MAX) {
                mcc_error(cg->mcc_ctx, "too many switch cases");
                return false;
            }
            (*num_cases)++;
            return collect_switch_labels(cg, node->data.case_stmt.stmt, cases,
                                         num_cases, default_case);
        case AST_DEFAULT_STMT:
            if (*default_case && *default_case != node) {
                mcc_error_at(cg->mcc_ctx, node->location,
                             "multiple default labels in one switch");
                return false;
            }
            *default_case = node;
            return collect_switch_labels(cg, node->data.default_stmt.stmt,
                                         cases, num_cases, default_case);
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < node->data.compound_stmt.num_stmts; i++)
                if (!collect_switch_labels(cg, node->data.compound_stmt.stmts[i],
                                           cases, num_cases, default_case))
                    return false;
            return true;
        case AST_IF_STMT:
            return collect_switch_labels(cg, node->data.if_stmt.then_stmt, cases,
                                         num_cases, default_case) &&
                   collect_switch_labels(cg, node->data.if_stmt.else_stmt, cases,
                                         num_cases, default_case);
        case AST_WHILE_STMT:
            return collect_switch_labels(cg, node->data.while_stmt.body, cases,
                                         num_cases, default_case);
        case AST_DO_STMT:
            return collect_switch_labels(cg, node->data.do_stmt.body, cases,
                                         num_cases, default_case);
        case AST_FOR_STMT:
            return collect_switch_labels(cg, node->data.for_stmt.body, cases,
                                         num_cases, default_case);
        case AST_LABEL_STMT:
            return collect_switch_labels(cg, node->data.label_stmt.stmt, cases,
                                         num_cases, default_case);
        default:
            /* Nested switches own their labels. */
            return true;
    }
}

anvil_block_t *codegen_switch_label_block(mcc_codegen_t *cg,
                                          mcc_ast_node_t *label)
{
    if (!cg->switch_info.active) return NULL;
    if (label->kind == AST_DEFAULT_STMT) return cg->switch_info.default_block;
    for (size_t i = 0; i < cg->switch_info.num_cases; i++)
        if (cg->switch_info.cases[i] == label) return cg->switch_info.blocks[i];
    return NULL;
}

anvil_block_t *codegen_switch_end_block(mcc_codegen_t *cg)
{
    if (!cg->switch_info.active) return NULL;
    if (!cg->switch_info.end_block) {
        char name[32];
        snprintf(name, sizeof(name), "switch%d.end", cg->switch_info.id);
        cg->switch_info.end_block = anvil_block_create(cg->current_func, name);
    }
    return cg->switch_info.end_block;
}

void codegen_switch_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    mcc_ast_node_t *body = stmt->data.switch_stmt.body;
    size_t num_cases = 0;
    mcc_ast_node_t *default_case = NULL;
    if (!collect_switch_labels(cg, body, NULL, &num_cases, &default_case))
        return;
    if (num_cases > SIZE_MAX / sizeof(mcc_ast_node_t *) ||
        num_cases > SIZE_MAX / sizeof(anvil_block_t *)) {
        mcc_error(cg->mcc_ctx, "switch case table size overflow");
        return;
    }

    mcc_ast_node_t **cases = NULL;
    anvil_block_t **case_blocks = NULL;
    if (num_cases > 0) {
        cases = mcc_alloc(cg->mcc_ctx, num_cases * sizeof(*cases));
        case_blocks = mcc_alloc(cg->mcc_ctx, num_cases * sizeof(*case_blocks));
        if (!cases || !case_blocks) return;
        size_t filled = 0;
        collect_switch_labels(cg, body, cases, &filled, &default_case);
    }

    int id = cg->label_counter++;

    /* The controlling expression is evaluated once; the comparison chain
     * reads it in blocks dominated by this one. */
    mcc_type_t *switch_c_type = stmt->data.switch_stmt.expr->type;
    if (switch_c_type && mcc_type_is_integer(switch_c_type))
        switch_c_type = mcc_type_promote(cg->types, switch_c_type);
    anvil_value_t *switch_val = codegen_expr(cg, stmt->data.switch_stmt.expr);
    switch_val = codegen_convert_value(cg, switch_val,
                                       stmt->data.switch_stmt.expr->type,
                                       switch_c_type, "switch.cast");
    if (!switch_val) {
        mcc_error_at(cg->mcc_ctx, stmt->location,
                     "cannot evaluate switch controlling expression");
        return;
    }
    bool is_unsigned = switch_c_type && switch_c_type->is_unsigned;

    struct mcc_switch_info saved = cg->switch_info;
    anvil_block_t *old_break = cg->break_target;
    size_t saved_num_locals = cg->num_locals;
    cg->switch_info.cases = cases;
    cg->switch_info.blocks = case_blocks;
    cg->switch_info.num_cases = num_cases;
    cg->switch_info.default_block = NULL;
    cg->switch_info.end_block = NULL;
    cg->switch_info.id = id;
    cg->switch_info.active = true;
    /* Breaks resolve through codegen_switch_end_block. */
    cg->break_target = NULL;

    for (size_t i = 0; i < num_cases; i++) {
        char name[48];
        snprintf(name, sizeof(name), "switch%d.case%zu", id, i);
        case_blocks[i] = anvil_block_create(cg->current_func, name);
    }
    if (default_case) {
        char name[32];
        snprintf(name, sizeof(name), "switch%d.default", id);
        cg->switch_info.default_block = anvil_block_create(cg->current_func, name);
    }

    /* Comparison chain in source order of the labels. */
    for (size_t i = 0; i < num_cases; i++) {
        mcc_ast_node_t *case_node = cases[i];
        anvil_value_t *low = codegen_expr(cg, case_node->data.case_stmt.expr);
        low = codegen_convert_value(cg, low, case_node->data.case_stmt.expr->type,
                                    switch_c_type, "case.cast");
        anvil_block_t *next;
        if (i + 1 < num_cases) {
            char name[48];
            snprintf(name, sizeof(name), "switch%d.cmp%zu", id, i + 1);
            next = anvil_block_create(cg->current_func, name);
        } else {
            next = cg->switch_info.default_block
                ? cg->switch_info.default_block : codegen_switch_end_block(cg);
        }

        if (case_node->data.case_stmt.end_expr) {
            /* GNU case range LO ... HI, compared with the signedness of the
             * promoted controlling type. */
            anvil_value_t *high = codegen_expr(cg, case_node->data.case_stmt.end_expr);
            high = codegen_convert_value(cg, high,
                                         case_node->data.case_stmt.end_expr->type,
                                         switch_c_type, "case.cast");
            char name[48];
            snprintf(name, sizeof(name), "switch%d.range%zu", id, i);
            anvil_block_t *upper = anvil_block_create(cg->current_func, name);
            anvil_value_t *ge = is_unsigned
                ? anvil_build_cmp_uge(cg->anvil_ctx, switch_val, low, "case.ge")
                : anvil_build_cmp_ge(cg->anvil_ctx, switch_val, low, "case.ge");
            anvil_build_br_cond(cg->anvil_ctx, ge, upper, next);
            codegen_set_current_block(cg, upper);
            anvil_value_t *le = is_unsigned
                ? anvil_build_cmp_ule(cg->anvil_ctx, switch_val, high, "case.le")
                : anvil_build_cmp_le(cg->anvil_ctx, switch_val, high, "case.le");
            anvil_build_br_cond(cg->anvil_ctx, le, case_blocks[i], next);
        } else {
            anvil_value_t *eq = anvil_build_cmp_eq(cg->anvil_ctx, switch_val,
                                                   low, "case.eq");
            anvil_build_br_cond(cg->anvil_ctx, eq, case_blocks[i], next);
        }
        if (i + 1 < num_cases) codegen_set_current_block(cg, next);
    }
    if (num_cases == 0) {
        anvil_build_br(cg->anvil_ctx, cg->switch_info.default_block
            ? cg->switch_info.default_block : codegen_switch_end_block(cg));
    }

    /* The body runs as ordinary code; its labels branch into the blocks
     * above.  Code before the first label is unreachable. */
    codegen_stmt(cg, body);
    if (!codegen_block_has_terminator(cg))
        anvil_build_br(cg->anvil_ctx, codegen_switch_end_block(cg));

    anvil_block_t *end_block = cg->switch_info.end_block;
    cg->switch_info = saved;
    cg->break_target = old_break;
    cg->num_locals = saved_num_locals;
    /* With no edge into the merge block the switch never completes and
     * whatever follows is unreachable: leave the current block terminated. */
    if (end_block) codegen_set_current_block(cg, end_block);
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

                if (codegen_abi_uses_param_attrs(cg))
                    anvil_build_ret_void(cg->anvil_ctx);
                else
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
