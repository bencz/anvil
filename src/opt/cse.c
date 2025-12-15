/*
 * ANVIL - Common Subexpression Elimination (CSE) Pass
 * 
 * Identifies and eliminates redundant computations by reusing
 * previously computed values.
 * 
 * Example:
 *   a = x + y
 *   b = x + y  // Same computation
 * Becomes:
 *   a = x + y
 *   b = a      // Reuse previous result
 * 
 * This pass works within basic blocks (local CSE).
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include <stdlib.h>
#include <string.h>

/* Maximum number of expressions to track */
#define MAX_EXPRESSIONS 256

/* Expression entry for tracking computed values */
typedef struct {
    anvil_op_t op;
    anvil_value_t *op1;
    anvil_value_t *op2;
    anvil_value_t *result;
    bool valid;
} expr_entry_t;

/* Expression table */
typedef struct {
    expr_entry_t entries[MAX_EXPRESSIONS];
    size_t count;
} expr_table_t;

/* Check if an operation is suitable for CSE */
static bool is_cse_candidate(anvil_op_t op)
{
    switch (op) {
        /* Arithmetic operations */
        case ANVIL_OP_ADD:
        case ANVIL_OP_SUB:
        case ANVIL_OP_MUL:
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
        case ANVIL_OP_SMOD:
        case ANVIL_OP_UMOD:
        /* Bitwise operations */
        case ANVIL_OP_AND:
        case ANVIL_OP_OR:
        case ANVIL_OP_XOR:
        case ANVIL_OP_SHL:
        case ANVIL_OP_SHR:
        case ANVIL_OP_SAR:
        /* Comparison operations */
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
        case ANVIL_OP_CMP_ULT:
        case ANVIL_OP_CMP_ULE:
        case ANVIL_OP_CMP_UGT:
        case ANVIL_OP_CMP_UGE:
            return true;
        default:
            return false;
    }
}

/* Check if operation is commutative */
static bool is_commutative(anvil_op_t op)
{
    switch (op) {
        case ANVIL_OP_ADD:
        case ANVIL_OP_MUL:
        case ANVIL_OP_AND:
        case ANVIL_OP_OR:
        case ANVIL_OP_XOR:
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
            return true;
        default:
            return false;
    }
}

/* Initialize expression table */
static void expr_table_init(expr_table_t *table)
{
    memset(table, 0, sizeof(*table));
}

/* Look up an expression in the table */
static anvil_value_t *expr_table_lookup(expr_table_t *table, anvil_op_t op,
                                         anvil_value_t *op1, anvil_value_t *op2)
{
    for (size_t i = 0; i < table->count; i++) {
        expr_entry_t *e = &table->entries[i];
        if (!e->valid) continue;
        if (e->op != op) continue;
        
        /* Check operands match */
        if (e->op1 == op1 && e->op2 == op2) {
            return e->result;
        }
        
        /* For commutative ops, also check swapped operands */
        if (is_commutative(op) && e->op1 == op2 && e->op2 == op1) {
            return e->result;
        }
    }
    
    return NULL;
}

/* Add an expression to the table */
static void expr_table_add(expr_table_t *table, anvil_op_t op,
                           anvil_value_t *op1, anvil_value_t *op2,
                           anvil_value_t *result)
{
    if (table->count >= MAX_EXPRESSIONS) return;
    
    expr_entry_t *e = &table->entries[table->count++];
    e->op = op;
    e->op1 = op1;
    e->op2 = op2;
    e->result = result;
    e->valid = true;
}

/* Invalidate expressions that use a given value as operand (for future use) */
static void expr_table_invalidate(expr_table_t *table, anvil_value_t *val)
    __attribute__((unused));

static void expr_table_invalidate(expr_table_t *table, anvil_value_t *val)
{
    for (size_t i = 0; i < table->count; i++) {
        expr_entry_t *e = &table->entries[i];
        if (!e->valid) continue;
        if (e->op1 == val || e->op2 == val || e->result == val) {
            e->valid = false;
        }
    }
}

/* Replace all uses of old_val with new_val after start instruction */
static int replace_uses_after(anvil_instr_t *start, anvil_value_t *old_val, 
                               anvil_value_t *new_val)
{
    int count = 0;
    
    for (anvil_instr_t *instr = start->next; instr; instr = instr->next) {
        for (size_t i = 0; i < instr->num_operands; i++) {
            if (instr->operands[i] == old_val) {
                instr->operands[i] = new_val;
                count++;
            }
        }
    }
    
    return count;
}

/* Process a single basic block for CSE */
static bool cse_block(anvil_block_t *block)
{
    bool changed = false;
    expr_table_t table;
    expr_table_init(&table);
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        /* Skip non-CSE candidates */
        if (!is_cse_candidate(instr->op)) {
            /* Stores and calls invalidate memory-dependent expressions */
            if (instr->op == ANVIL_OP_STORE || instr->op == ANVIL_OP_CALL) {
                /* For now, be conservative and clear all */
                expr_table_init(&table);
            }
            continue;
        }
        
        /* Need at least 2 operands for binary ops */
        if (instr->num_operands < 2) continue;
        
        anvil_value_t *op1 = instr->operands[0];
        anvil_value_t *op2 = instr->operands[1];
        
        /* Look for existing computation */
        anvil_value_t *existing = expr_table_lookup(&table, instr->op, op1, op2);
        
        if (existing && instr->result) {
            /* Found a common subexpression - replace uses */
            int replaced = replace_uses_after(instr, instr->result, existing);
            if (replaced > 0) {
                /* Mark instruction as dead (NOP) */
                instr->op = ANVIL_OP_NOP;
                changed = true;
            }
        } else if (instr->result) {
            /* Add this computation to the table */
            expr_table_add(&table, instr->op, op1, op2, instr->result);
        }
    }
    
    return changed;
}

/* Main CSE pass */
bool anvil_pass_cse(anvil_func_t *func)
{
    if (!func || !func->blocks) return false;
    
    bool changed = false;
    
    /* Process each basic block independently (local CSE) */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        if (cse_block(block)) {
            changed = true;
        }
    }
    
    return changed;
}
