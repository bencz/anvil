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
 * anvil_pass_cse works within basic blocks; anvil_pass_gvn across blocks,
 * replacing an expression by an equal one in a dominating block.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include "anvil/anvil_analysis.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

enum { EXPR_OPERANDS = 3 };

/* What makes two pure instructions compute the same value: opcode, FCMP
 * predicate, GEP source type, operands and result type (casts of one value
 * differ by it). */
typedef struct {
    anvil_op_t op;
    anvil_fcmp_pred_t fcmp_pred;
    const anvil_type_t *aux_type;
    anvil_value_t *operands[EXPR_OPERANDS];
    size_t count;
    anvil_type_t *type;
} expr_key_t;

/* Hash table cells with result==NULL are empty slots. */
typedef struct {
    expr_key_t key;
    anvil_value_t *result;
} expr_entry_t;

/* Open-addressing hash table keyed by expr_key_t. */
typedef struct {
    anvil_ctx_t *ctx;
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
        case ANVIL_OP_UMULH:
        case ANVIL_OP_SMULH:
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
        case ANVIL_OP_ROTL:
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
        case ANVIL_OP_FCMP:
        /* Integer and pointer conversions (FP conversions depend on the
         * floating-point environment) */
        case ANVIL_OP_SEXT:
        case ANVIL_OP_ZEXT:
        case ANVIL_OP_TRUNC:
        case ANVIL_OP_BITCAST:
        case ANVIL_OP_PTRTOINT:
        case ANVIL_OP_INTTOPTR:
        case ANVIL_OP_NEG:
        case ANVIL_OP_NOT:
        /* Address arithmetic and selection */
        case ANVIL_OP_GEP:
        case ANVIL_OP_STRUCT_GEP:
        case ANVIL_OP_SELECT:
            return true;
        default:
            return false;
    }
}

static bool is_unary(anvil_op_t op)
{
    switch (op) {
        case ANVIL_OP_SEXT:
        case ANVIL_OP_ZEXT:
        case ANVIL_OP_TRUNC:
        case ANVIL_OP_BITCAST:
        case ANVIL_OP_PTRTOINT:
        case ANVIL_OP_INTTOPTR:
        case ANVIL_OP_NEG:
        case ANVIL_OP_NOT:
            return true;
        default:
            return false;
    }
}

/* The key of a candidate; false when it has no result or another shape. */
static bool expr_key(const anvil_instr_t *instr, expr_key_t *key)
{
    size_t arity = is_unary(instr->op) ? 1 : instr->op == ANVIL_OP_SELECT ? 3 : 2;
    bool walk = instr->op == ANVIL_OP_GEP || instr->op == ANVIL_OP_STRUCT_GEP;
    if (!instr->result || (walk ? instr->num_operands < 2 || instr->num_operands > EXPR_OPERANDS : instr->num_operands != arity))
        return false;
    *key = (expr_key_t){
        .op = instr->op,
        .fcmp_pred = instr->fcmp_pred,
        .aux_type = walk ? instr->aux_type : NULL,
        .count = instr->num_operands,
        .type = instr->result->type,
    };
    for (size_t operand = 0; operand < instr->num_operands; operand++)
        key->operands[operand] = instr->operands[operand];
    return true;
}

/* Check if operation is commutative */
static bool is_commutative(anvil_op_t op)
{
    switch (op) {
        case ANVIL_OP_ADD:
        case ANVIL_OP_MUL:
        case ANVIL_OP_UMULH:
        case ANVIL_OP_SMULH:
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

/* Constants are not interned: two integer constants of one type and value
 * are the same operand. */
static bool same_operand(const anvil_value_t *left, const anvil_value_t *right)
{
    return left == right || (anvil_opt_is_const_int(left) && anvil_opt_is_const_int(right) && left->data.i == right->data.i &&
                             anvil_types_equal(left->type, right->type));
}

/* An operand's identity for hashing and ordering, by value for constants. */
static uint64_t operand_identity(const anvil_value_t *value)
{
    if (anvil_opt_is_const_int(value))
        return ((uint64_t)value->data.i * 0xFF51AFD7ED558CCDULL) ^ (value->type ? (uint64_t)value->type->size << 56 : 0) ^ 1;
    return (uintptr_t)value;
}

/* Mix function — good enough for pointer+small-integer inputs. */
static uint64_t expr_hash(const expr_key_t *key)
{
    uint64_t h = (uint64_t)key->op * 0x9E3779B185EBCA87ULL;
    if (key->type)
        h ^= ((uint64_t)key->type->kind << 32 | (uint64_t)key->type->size) * 0xC2B2AE3D27D4EB4FULL;
    h ^= (uint64_t)(unsigned)key->fcmp_pred * 0xD6E8FEB86659FD93ULL;
    h ^= (uintptr_t)key->aux_type + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    for (size_t operand = 0; operand < key->count; operand++)
        h ^= operand_identity(key->operands[operand]) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

static bool expr_equal(const expr_key_t *left, const expr_key_t *right)
{
    if (left->op != right->op || left->fcmp_pred != right->fcmp_pred || left->count != right->count ||
        !anvil_types_equal(left->type, right->type) ||
        (left->aux_type != right->aux_type && !anvil_types_equal(left->aux_type, right->aux_type)))
        return false;
    for (size_t operand = 0; operand < left->count; operand++) {
        if (!same_operand(left->operands[operand], right->operands[operand]))
            return false;
    }
    return true;
}

static void expr_table_init(expr_table_t *table, anvil_ctx_t *ctx)
{
    table->ctx = ctx;
    table->entries = anvil_ctx_calloc(ctx, EXPR_INITIAL_CAP,
                                      sizeof(expr_entry_t));
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
static inline void normalise_operands(expr_key_t *key)
{
    if (key->count == 2 && is_commutative(key->op) && operand_identity(key->operands[0]) > operand_identity(key->operands[1])) {
        anvil_value_t *tmp = key->operands[0];
        key->operands[0] = key->operands[1];
        key->operands[1] = tmp;
    }
}

static anvil_value_t *expr_table_lookup(expr_table_t *table, expr_key_t key)
{
    if (!table->entries || table->cap == 0) return NULL;

    normalise_operands(&key);
    size_t mask = table->cap - 1;
    size_t i = (size_t)expr_hash(&key) & mask;

    for (size_t probe = 0; probe < table->cap; probe++) {
        expr_entry_t *e = &table->entries[i];
        if (!e->result) return NULL;
        if (expr_equal(&e->key, &key))
            return e->result;
        i = (i + 1) & mask;
    }
    return NULL;
}

static void expr_table_resize(expr_table_t *table);

static void expr_table_add(expr_table_t *table, expr_key_t key, anvil_value_t *result)
{
    if (!result) return;
    if (!table->entries) return;

    /* Grow at 70% load factor. */
    if ((table->size + 1) * 10 > table->cap * 7) {
        expr_table_resize(table);
        if (!table->entries) return;
    }

    normalise_operands(&key);
    size_t mask = table->cap - 1;
    size_t i = (size_t)expr_hash(&key) & mask;

    while (table->entries[i].result) {
        /* A global lookup may replace an expression from a sibling block. */
        if (expr_equal(&table->entries[i].key, &key)) {
            table->entries[i].result = result;
            return;
        }
        i = (i + 1) & mask;
    }
    table->entries[i].key = key;
    table->entries[i].result = result;
    table->size++;
}

static void expr_table_resize(expr_table_t *table)
{
    size_t old_cap = table->cap;
    expr_entry_t *old = table->entries;
    size_t new_cap = old_cap ? old_cap * 2 : EXPR_INITIAL_CAP;

    if (old_cap > SIZE_MAX / 2 ||
        new_cap > SIZE_MAX / sizeof(expr_entry_t)) {
        anvil_set_error(table->ctx, ANVIL_ERR_NOMEM,
                        "CSE expression table capacity overflow");
        return;
    }
    table->entries = anvil_ctx_calloc(table->ctx, new_cap,
                                      sizeof(expr_entry_t));
    if (!table->entries) {
        table->entries = old;   /* best-effort: keep old on alloc failure */
        return;
    }
    table->cap = new_cap;
    table->size = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].result)
            expr_table_add(table, old[i].key, old[i].result);
    }
    free(old);
}

/* Process a single basic block for CSE */
static bool cse_block(anvil_block_t *block)
{
    bool changed = false;
    expr_table_t table;
    anvil_ctx_t *ctx = block && block->parent && block->parent->parent
        ? block->parent->parent->ctx : NULL;
    expr_table_init(&table, ctx);
    
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

        expr_key_t key;
        if (!expr_key(instr, &key)) continue;

        anvil_value_t *existing = expr_table_lookup(&table, key);

        if (existing && instr->result) {
            /* The earlier expression dominates this instruction, and a valid
             * SSA result may also be used by successor blocks or PHIs.  Replace
             * every use before deleting the duplicate; a block-local rewrite
             * can otherwise leave cross-block uses referring to a NOP. */
            int replaced = anvil_opt_replace_uses_in_func(block->parent,
                                                          instr->result,
                                                          existing);
            if (replaced > 0) {
                anvil_opt_erase_instr(instr);
                changed = true;
            }
        } else if (instr->result) {
            expr_table_add(&table, key, instr->result);
        }
    }

    expr_table_free(&table);
    return changed;
}

/* Main CSE pass */
anvil_pass_result_t anvil_pass_cse(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx)
        return ANVIL_PASS_RUN_ERROR;
    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (!func->blocks) return ANVIL_PASS_RUN_UNCHANGED;
    
    bool changed = false;
    
    /* Process each basic block independently (local CSE) */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        if (cse_block(block)) {
            changed = true;
        }
    }
    
    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}

/* Dominance-based value numbering for integer expressions. Memory and FP
 * environment effects are deliberately outside this expression domain. */
anvil_pass_result_t anvil_pass_gvn(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;

    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (func->is_declaration || !func->blocks)
        return ANVIL_PASS_RUN_UNCHANGED;

    anvil_opt_cfg_t cfg;
    if (!anvil_opt_cfg_build(func, &cfg))
        return ANVIL_PASS_RUN_ERROR;

    expr_table_t table;
    expr_table_init(&table, ctx);
    bool changed = false;
    for (size_t rank = 0; table.entries && rank < cfg.reachable_count; rank++)
    {
        size_t block_index = cfg.rpo[rank];
        anvil_block_t *block = cfg.blocks[block_index];
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
        {
            expr_key_t key;
            if (!is_cse_candidate(instr->op) || instr->op == ANVIL_OP_FCMP || !expr_key(instr, &key))
                continue;

            anvil_value_t *existing = expr_table_lookup(&table, key);
            size_t defining_block = existing ? anvil_opt_cfg_index(&cfg, existing->data.instr->parent) : SIZE_MAX;
            if (existing && anvil_types_equal(existing->type, instr->result->type) && anvil_opt_cfg_dominates(&cfg, defining_block, block_index))
            {
                anvil_opt_replace_uses_in_func(func, instr->result, existing);
                anvil_opt_erase_instr(instr);
                changed = true;
            }
            else
            {
                expr_table_add(&table, key, instr->result);
                if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
                    break;
            }
        }

        if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
            break;
    }

    expr_table_free(&table);
    anvil_opt_cfg_destroy(&cfg);
    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;

    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
