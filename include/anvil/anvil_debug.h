/*
 * ANVIL - Debug/Dump API
 * 
 * Functions for printing IR in human-readable format for debugging.
 */

#ifndef ANVIL_DEBUG_H
#define ANVIL_DEBUG_H

#include "anvil.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations for internal types */
struct anvil_instr;
typedef struct anvil_instr anvil_instr_t;

struct anvil_global;
typedef struct anvil_global anvil_global_t;

/* ============================================================================
 * Dump to FILE* API
 * ============================================================================ */

/* Dump type to file */
void anvil_dump_type(FILE *out, anvil_type_t *type);

/* Dump value reference to file */
void anvil_dump_value(FILE *out, anvil_value_t *val);

/* Dump instruction to file */
void anvil_dump_instr(FILE *out, anvil_instr_t *instr);

/* Dump basic block to file */
void anvil_dump_block(FILE *out, anvil_block_t *block);

/* Dump function to file */
void anvil_dump_func(FILE *out, anvil_func_t *func);

/* Dump global variable to file */
void anvil_dump_global(FILE *out, anvil_global_t *global);

/* Dump module to file */
void anvil_dump_module(FILE *out, anvil_module_t *mod);

/* ============================================================================
 * Print to stdout API (convenience wrappers)
 * ============================================================================ */

/* Print module to stdout */
void anvil_print_module(anvil_module_t *mod);

/* Print function to stdout */
void anvil_print_func(anvil_func_t *func);

/* Print instruction to stdout */
void anvil_print_instr(anvil_instr_t *instr);

/* ============================================================================
 * Dump to string API
 * ============================================================================ */

/* Dump module to newly allocated string (caller must free) */
char *anvil_module_to_string(anvil_module_t *mod);

/* Dump function to newly allocated string (caller must free) */
char *anvil_func_to_string(anvil_func_t *func);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_DEBUG_H */
