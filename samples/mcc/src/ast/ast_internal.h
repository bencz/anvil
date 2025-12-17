/*
 * MCC - Micro C Compiler
 * AST internal header
 */

#ifndef MCC_AST_INTERNAL_H
#define MCC_AST_INTERNAL_H

#include "mcc.h"
#include "types.h"

/* Shared name tables - defined in ast.c, used by ast_dump.c */
extern const char *mcc_ast_kind_names[];
extern const char *mcc_binop_names[];
extern const char *mcc_unop_names[];
extern const char *mcc_int_suffix_names[];
extern const char *mcc_float_suffix_names[];
extern const char *mcc_attr_kind_names[];

#endif /* MCC_AST_INTERNAL_H */
