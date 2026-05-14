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
#include "opt_utils.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* Expression entry for tracking computed values. Hash table cells with
 * result==NULL are empty slots. */
typedef struct {
    anvil_op_t op;
    anvil_value_t *op1;
    anvil_value_t *op2;
    anvil_value_t *result;
} expr_entry_t;

/* Open-addressing hash table keyed by (op, op1, op2). Previously a fixed
 * 256-entry linear-scan array that silently dropped expressions beyond 256
 * and turned every lookup into O(n). */
typedef struct {
    expr_entry_t *entries;
    size_t size;      /* number of non-empty slots */
    size_t cap;       /* capacity (always a power of two) */
} expr_table_t;

#define EXPR_INITIAL_CAP 32

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

/* Mix function — good enough for pointer+small-integer inputs. */
static inline uint64_t expr_hash(anvil_op_t op, anvil_value_t *op1, anvil_value_t *op2)
{
    uint64_t h = (uint64_t)op * 0x9E3779B185EBCA87ULL;
    h ^= (uintptr_t)op1 + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= (uintptr_t)op2 + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

static void expr_table_init(expr_table_t *table)
{
    table->entries = calloc(EXPR_INITIAL_CAP, sizeof(expr_entry_t));
    table->cap = table->entries ? EXPR_INITIAL_CAP : 0;
    table->size = 0;
}

static void expr_table_free(expr_table_t *table)
{
    free(table->entries);
    table->entries = NULL;
    table->size = 0;
    table->cap = 0;
}

static void expr_table_clear(expr_table_t *table)
{
    if (!table->entries) return;
    memset(table->entries, 0, table->cap * sizeof(expr_entry_t));
    table->size = 0;
}

/* Normalise operand order for commutative ops so both orderings hash/match
 * the same slot. */
static inline void normalise_operands(anvil_op_t op, anvil_value_t **op1, anvil_value_t **op2)
{
    if (is_commutative(op) && (uintptr_t)*op1 > (uintptr_t)*op2) {
        anvil_value_t *tmp = *op1; *op1 = *op2; *op2 = tmp;
    }
}

static anvil_value_t *expr_table_lookup(expr_table_t *table, anvil_op_t op,
                                         anvil_value_t *op1, anvil_value_t *op2)
{
    if (!table->entries || table->cap == 0) return NULL;

    normalise_operands(op, &op1, &op2);
    size_t mask = table->cap - 1;
    size_t i = (size_t)expr_hash(op, op1, op2) & mask;

    for (size_t probe = 0; probe < table->cap; probe++) {
        expr_entry_t *e = &table->entries[i];
        if (!e->result) return NULL;
        if (e->op == op && e->op1 == op1 && e->op2 == op2) {
            return e->result;
        }
        i = (i + 1) & mask;
    }
    return NULL;
}

static void expr_table_resize(expr_table_t *table);

static void expr_table_add(expr_table_t *table, anvil_op_t op,
                           anvil_value_t *op1, anvil_value_t *op2,
                           anvil_value_t *result)
{
    if (!result) return;
    if (!table->entries) return;

    /* Grow at 70% load factor. */
    if ((table->size + 1) * 10 > table->cap * 7) {
        expr_table_resize(table);
        if (!table->entries) return;
    }

    normalise_operands(op, &op1, &op2);
    size_t mask = table->cap - 1;
    size_t i = (size_t)expr_hash(op, op1, op2) & mask;

    while (table->entries[i].result) {
        /* On duplicate, leave existing entry (prefer the earliest). */
        if (table->entries[i].op == op &&
            table->entries[i].op1 == op1 &&
            table->entries[i].op2 == op2) {
            return;
        }
        i = (i + 1) & mask;
    }
    table->entries[i].op = op;
    table->entries[i].op1 = op1;
    table->entries[i].op2 = op2;
    table->entries[i].result = result;
    table->size++;
}

static void expr_table_resize(expr_table_t *table)
{
    size_t old_cap = table->cap;
    expr_entry_t *old = table->entries;
    size_t new_cap = old_cap ? old_cap * 2 : EXPR_INITIAL_CAP;

    table->entries = calloc(new_cap, sizeof(expr_entry_t));
    if (!table->entries) {
        table->entries = old;   /* best-effort: keep old on alloc failure */
        return;
    }
    table->cap = new_cap;
    table->size = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].result) {
            expr_table_add(table, old[i].op, old[i].op1, old[i].op2, old[i].result);
        }
    }
    free(old);
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
            /* Stores and calls may invalidate memory-dependent cached
             * expressions. Only pure arithmetic/compares are cached today,
             * so nothing in the table can actually alias memory — but we
             * clear conservatively to stay correct if the candidate set
             * grows. expr_table_clear preserves the allocation (old code
             * called expr_table_init here, which leaked the existing
             * heap-allocated buffer). */
            if (instr->op == ANVIL_OP_STORE || instr->op == ANVIL_OP_CALL) {
                expr_table_clear(&table);
            }
            continue;
        }

        /* Need at least 2 operands for binary ops */
        if (instr->num_operands < 2) continue;

        anvil_value_t *op1 = instr->operands[0];
        anvil_value_t *op2 = instr->operands[1];

        anvil_value_t *existing = expr_table_lookup(&table, instr->op, op1, op2);

        if (existing && instr->result) {
            int replaced = anvil_opt_replace_uses_in_block_after(instr, instr->result, existing);
            if (replaced > 0) {
                instr->op = ANVIL_OP_NOP;
                changed = true;
            }
        } else if (instr->result) {
            expr_table_add(&table, instr->op, op1, op2, instr->result);
        }
    }

    expr_table_free(&table);
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
