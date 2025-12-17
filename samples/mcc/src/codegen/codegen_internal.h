/*
 * MCC - Micro C Compiler
 * Code Generator Internal Header
 * 
 * This file contains internal structures, constants, and function
 * declarations used by the code generator implementation.
 */

#ifndef MCC_CODEGEN_INTERNAL_H
#define MCC_CODEGEN_INTERNAL_H

#include "anvil/anvil.h"
#include "mcc.h"

/* ============================================================
 * Architecture Mapping (codegen.c)
 * ============================================================ */

/* Check if architecture uses Darwin ABI */
bool codegen_arch_is_darwin(mcc_arch_t arch);

/* ============================================================
 * Local Variable Management (codegen.c)
 * ============================================================ */

/* Find local variable value by name */
anvil_value_t *codegen_find_local(mcc_codegen_t *cg, const char *name);

/* Add local variable */
void codegen_add_local(mcc_codegen_t *cg, const char *name, anvil_value_t *value);

/* Get or create global variable reference */
anvil_value_t *codegen_get_or_add_global(mcc_codegen_t *cg, const char *name, anvil_type_t *type);

/* ============================================================
 * String Literal Management (codegen.c)
 * ============================================================ */

/* Find or create string literal */
anvil_value_t *codegen_get_string_literal(mcc_codegen_t *cg, const char *str);

/* ============================================================
 * Label Management (codegen.c)
 * ============================================================ */

/* Find or create label block */
anvil_block_t *codegen_get_label_block(mcc_codegen_t *cg, const char *name);

/* ============================================================
 * Block Management (codegen.c)
 * ============================================================ */

/* Set current block for code generation */
void codegen_set_current_block(mcc_codegen_t *cg, anvil_block_t *block);

/* Check if block has terminator */
bool codegen_block_has_terminator(mcc_codegen_t *cg);

/* ============================================================
 * Function Management (codegen.c)
 * ============================================================ */

/* Find function by symbol */
anvil_func_t *codegen_find_func(mcc_codegen_t *cg, mcc_symbol_t *sym);

/* Add function mapping */
void codegen_add_func(mcc_codegen_t *cg, mcc_symbol_t *sym, anvil_func_t *func);

/* Get or declare a function */
anvil_func_t *codegen_get_or_declare_func(mcc_codegen_t *cg, mcc_symbol_t *sym);

/* ============================================================
 * Type Conversion (codegen_type.c)
 * ============================================================ */

/* Convert MCC type to ANVIL type */
anvil_type_t *codegen_type(mcc_codegen_t *cg, mcc_type_t *type);

/* Get sizeof for a type using ANVIL arch info for pointer size */
size_t codegen_sizeof(mcc_codegen_t *cg, mcc_type_t *type);

/* ============================================================
 * Expression Code Generation (codegen_expr.c)
 * ============================================================ */

/* Generate code for expression (returns value) */
anvil_value_t *codegen_expr(mcc_codegen_t *cg, mcc_ast_node_t *expr);

/* Generate code for lvalue (returns pointer) */
anvil_value_t *codegen_lvalue(mcc_codegen_t *cg, mcc_ast_node_t *expr);

/* Convert value to boolean (avoids redundant CMP_NE if already boolean) */
anvil_value_t *codegen_to_bool(mcc_codegen_t *cg, anvil_value_t *val);

/* ============================================================
 * Statement Code Generation (codegen_stmt.c)
 * ============================================================ */

/* Generate code for any statement */
void codegen_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);

/* Generate code for compound statement */
void codegen_compound_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);

/* Generate code for if statement */
void codegen_if_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);

/* Generate code for while statement */
void codegen_while_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);

/* Generate code for do-while statement */
void codegen_do_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);

/* Generate code for for statement */
void codegen_for_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);

/* Generate code for switch statement */
void codegen_switch_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);

/* Generate code for return statement */
void codegen_return_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt);

/* ============================================================
 * Declaration Code Generation (codegen_decl.c)
 * ============================================================ */

/* Generate code for any declaration */
void codegen_decl(mcc_codegen_t *cg, mcc_ast_node_t *decl);

/* Generate code for function */
void codegen_func(mcc_codegen_t *cg, mcc_ast_node_t *func);

/* Generate code for global variable */
void codegen_global_var(mcc_codegen_t *cg, mcc_ast_node_t *var);

#endif /* MCC_CODEGEN_INTERNAL_H */
