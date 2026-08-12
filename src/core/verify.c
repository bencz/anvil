/*
 * ANVIL - Source-level IR verifier.
 */

#include "anvil/anvil_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool verify_fail(char *error, size_t error_len, const char *fmt, ...)
{
    if (error && error_len > 0) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, error_len, fmt, args);
        va_end(args);
    }
    return false;
}

static const char *func_name(const anvil_func_t *func)
{
    return (func && func->name) ? func->name : "<anon>";
}

static const char *block_name(const anvil_block_t *block)
{
    return (block && block->name) ? block->name : "<anon>";
}

static bool type_is_integer(const anvil_type_t *type)
{
    if (!type) return false;
    switch (type->kind) {
        case ANVIL_TYPE_I1:
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U8:
        case ANVIL_TYPE_U16:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_U64:
            return true;
        default:
            return false;
    }
}

static bool type_is_void(const anvil_type_t *type)
{
    return type && type->kind == ANVIL_TYPE_VOID;
}

static bool type_is_bool_like(const anvil_type_t *type)
{
    return anvil_sem_bool_type(type);
}

static bool type_equal(const anvil_type_t *lhs, const anvil_type_t *rhs)
{
    return anvil_types_equal(lhs, rhs);
}

static bool block_belongs_to_func(const anvil_func_t *func,
                                  const anvil_block_t *block)
{
    if (!func || !block) return false;
    for (const anvil_block_t *cur = func->blocks; cur; cur = cur->next) {
        if (cur == block) return true;
    }
    return false;
}

static bool module_has_global_value(const anvil_module_t *mod,
                                    const anvil_value_t *value)
{
    return mod && value && value->kind == ANVIL_VAL_GLOBAL &&
           value->owner_module == mod && value->name &&
           anvil_module_lookup_symbol(mod, value->name) == value;
}

static bool module_has_func_value(const anvil_module_t *mod,
                                  const anvil_value_t *value)
{
    return mod && value && value->kind == ANVIL_VAL_FUNC &&
           value->owner_module == mod && value->name &&
           anvil_module_lookup_symbol(mod, value->name) == value;
}

static bool func_has_param_value(const anvil_func_t *func,
                                 const anvil_value_t *value)
{
    if (!func || !value || value->kind != ANVIL_VAL_PARAM) return false;
    if (value->owner_module != func->parent) return false;
    if (value->data.param.func != func) return false;
    if (value->data.param.index >= func->num_params) return false;
    return func->params && func->params[value->data.param.index] == value;
}

static bool func_has_instr_result(const anvil_func_t *func,
                                  const anvil_value_t *value)
{
    if (!func || !value || value->kind != ANVIL_VAL_INSTR ||
        value->owner_module != func->parent || !value->data.instr) {
        return false;
    }
    const anvil_instr_t *instr = value->data.instr;
    return instr->result == value &&
           instr->parent &&
           instr->parent->parent == func &&
           block_belongs_to_func(func, instr->parent);
}

static bool verify_value_ref(const anvil_module_t *mod,
                             const anvil_func_t *func,
                             const anvil_value_t *value,
                             char *error,
                             size_t error_len)
{
    if (!value) {
        return verify_fail(error, error_len,
                           "function %s references a null value",
                           func_name(func));
    }
    if (!value->type) {
        return verify_fail(error, error_len,
                           "function %s references a value without type",
                           func_name(func));
    }
    if (!mod || value->owner_ctx != mod->ctx ||
        value->type->owner_ctx != mod->ctx) {
        return verify_fail(error, error_len,
                           "function %s references a value from another context",
                           func_name(func));
    }

    switch (value->kind) {
        case ANVIL_VAL_CONST_INT:
        case ANVIL_VAL_CONST_FLOAT:
        case ANVIL_VAL_CONST_DECIMAL:
        case ANVIL_VAL_CONST_STRING:
        case ANVIL_VAL_CONST_NULL:
        case ANVIL_VAL_CONST_ARRAY:
        case ANVIL_VAL_CONST_STRUCT:
        case ANVIL_VAL_CONST_SYMBOL_ADDR:
        case ANVIL_VAL_CONST_GEP:
            {
                anvil_const_dag_status_t dag =
                    anvil_value_check_constant_dag_for_module(value, mod->ctx,
                                                              mod);
                if (dag == ANVIL_CONST_DAG_VALID) return true;
                if (dag == ANVIL_CONST_DAG_NOMEM)
                    return verify_fail(error, error_len,
                                       "out of memory validating a constant DAG in function %s",
                                       func_name(func));
            }
            return verify_fail(error, error_len,
                               "function %s references a malformed constant DAG",
                               func_name(func));

        case ANVIL_VAL_GLOBAL:
            if (module_has_global_value(mod, value)) return true;
            return verify_fail(error, error_len,
                               "function %s references a global outside its module",
                               func_name(func));

        case ANVIL_VAL_FUNC:
            if (module_has_func_value(mod, value)) return true;
            return verify_fail(error, error_len,
                               "function %s references a function outside its module",
                               func_name(func));

        case ANVIL_VAL_PARAM:
            if (func_has_param_value(func, value)) return true;
            return verify_fail(error, error_len,
                               "function %s references a parameter outside the function",
                               func_name(func));

        case ANVIL_VAL_INSTR:
            if (func_has_instr_result(func, value)) return true;
            return verify_fail(error, error_len,
                               "function %s references an instruction result outside the function",
                               func_name(func));

        case ANVIL_VAL_BLOCK:
            break;
    }

    return verify_fail(error, error_len,
                       "function %s references an unsupported value kind",
                       func_name(func));
}

static anvil_type_t *memory_object_type(const anvil_value_t *value)
{
    return anvil_sem_memory_object_type(value);
}

static anvil_type_t *callee_func_type(const anvil_value_t *callee)
{
    return anvil_sem_callee_func_type(callee);
}

static bool op_is_terminator(anvil_op_t op)
{
    return op == ANVIL_OP_RET ||
           op == ANVIL_OP_BR ||
           op == ANVIL_OP_BR_COND ||
           op == ANVIL_OP_SWITCH;
}

static bool block_has_successor(const anvil_block_t *from,
                                const anvil_block_t *to)
{
    if (!from || !to || !from->last) return false;
    if (from->last->op == ANVIL_OP_BR) {
        return from->last->true_block == to;
    }
    if (from->last->op == ANVIL_OP_BR_COND) {
        return from->last->true_block == to ||
               from->last->false_block == to;
    }
    if (from->last->op == ANVIL_OP_SWITCH) {
        if (from->last->true_block == to) return true;
        for (size_t i = 0; i < from->last->num_switch_cases; i++) {
            if (from->last->switch_blocks &&
                from->last->switch_blocks[i] == to) {
                return true;
            }
        }
    }
    return false;
}

static bool phi_has_incoming_from(const anvil_instr_t *phi,
                                  const anvil_block_t *block)
{
    if (!phi || !block) return false;
    for (size_t i = 0; i < phi->num_phi_incoming; i++) {
        if (phi->phi_blocks && phi->phi_blocks[i] == block) return true;
    }
    return false;
}

/* A compact, verifier-local CFG/dominator analysis.  Basic-block predecessor
 * and dominator sets are bitsets so verification does not depend on the
 * optimizer-owned block->preds cache and remains reasonably efficient for
 * large frontend-generated functions. */
typedef struct {
    const anvil_block_t **blocks;
    size_t num_blocks;
    size_t words_per_set;
    uint64_t *preds;
    uint64_t *doms;
    bool *reachable;
} verify_cfg_t;

static void verify_cfg_destroy(verify_cfg_t *cfg)
{
    if (!cfg) return;
    free(cfg->blocks);
    free(cfg->preds);
    free(cfg->doms);
    free(cfg->reachable);
    memset(cfg, 0, sizeof(*cfg));
}

static bool size_mul_overflows(size_t lhs, size_t rhs)
{
    return rhs != 0 && lhs > SIZE_MAX / rhs;
}

static size_t verify_cfg_block_index(const verify_cfg_t *cfg,
                                     const anvil_block_t *block)
{
    if (!cfg || !block) return SIZE_MAX;
    for (size_t i = 0; i < cfg->num_blocks; i++) {
        if (cfg->blocks[i] == block) return i;
    }
    return SIZE_MAX;
}

static uint64_t *verify_cfg_set(uint64_t *sets,
                                const verify_cfg_t *cfg,
                                size_t block_index)
{
    return sets + block_index * cfg->words_per_set;
}

static const uint64_t *verify_cfg_const_set(const uint64_t *sets,
                                            const verify_cfg_t *cfg,
                                            size_t block_index)
{
    return sets + block_index * cfg->words_per_set;
}

static void bitset_add(uint64_t *set, size_t index)
{
    set[index / 64] |= UINT64_C(1) << (index % 64);
}

static bool bitset_contains(const uint64_t *set, size_t index)
{
    return (set[index / 64] & (UINT64_C(1) << (index % 64))) != 0;
}

static bool verify_cfg_add_edge(verify_cfg_t *cfg,
                                size_t from,
                                const anvil_block_t *to)
{
    size_t to_index = verify_cfg_block_index(cfg, to);
    if (to_index == SIZE_MAX) return false;
    bitset_add(verify_cfg_set(cfg->preds, cfg, to_index), from);
    return true;
}

static bool verify_cfg_build(const anvil_func_t *func,
                             verify_cfg_t *cfg,
                             char *error,
                             size_t error_len)
{
    memset(cfg, 0, sizeof(*cfg));

    size_t num_blocks = 0;
    for (const anvil_block_t *block = func->blocks; block; block = block->next) {
        if (num_blocks == SIZE_MAX) {
            return verify_fail(error, error_len,
                               "function %s has too many basic blocks",
                               func_name(func));
        }
        num_blocks++;
    }
    if (num_blocks == 0) {
        return verify_fail(error, error_len,
                           "function %s has no basic blocks",
                           func_name(func));
    }

    if (num_blocks > SIZE_MAX - 63) {
        return verify_fail(error, error_len,
                           "CFG analysis size overflow in function %s",
                           func_name(func));
    }
    size_t words = (num_blocks + 63) / 64;
    if (size_mul_overflows(num_blocks, sizeof(*cfg->blocks)) ||
        size_mul_overflows(num_blocks, words) ||
        size_mul_overflows(num_blocks * words, sizeof(*cfg->preds))) {
        return verify_fail(error, error_len,
                           "CFG analysis size overflow in function %s",
                           func_name(func));
    }

    anvil_ctx_t *ctx = func->parent ? func->parent->ctx : NULL;
    cfg->blocks = anvil_ctx_malloc(ctx, num_blocks * sizeof(*cfg->blocks));
    cfg->preds = anvil_ctx_calloc(ctx, num_blocks * words,
                                  sizeof(*cfg->preds));
    cfg->doms = anvil_ctx_calloc(ctx, num_blocks * words,
                                 sizeof(*cfg->doms));
    cfg->reachable = anvil_ctx_calloc(ctx, num_blocks,
                                      sizeof(*cfg->reachable));
    if (!cfg->blocks || !cfg->preds || !cfg->doms || !cfg->reachable) {
        verify_cfg_destroy(cfg);
        return verify_fail(error, error_len,
                           "out of memory while analyzing CFG of function %s",
                           func_name(func));
    }
    cfg->num_blocks = num_blocks;
    cfg->words_per_set = words;

    size_t index = 0;
    for (const anvil_block_t *block = func->blocks; block; block = block->next) {
        cfg->blocks[index++] = block;
    }

    for (size_t i = 0; i < num_blocks; i++) {
        const anvil_instr_t *term = cfg->blocks[i]->last;
        if (term->op == ANVIL_OP_BR) {
            verify_cfg_add_edge(cfg, i, term->true_block);
        } else if (term->op == ANVIL_OP_BR_COND) {
            verify_cfg_add_edge(cfg, i, term->true_block);
            verify_cfg_add_edge(cfg, i, term->false_block);
        } else if (term->op == ANVIL_OP_SWITCH) {
            verify_cfg_add_edge(cfg, i, term->true_block);
            for (size_t j = 0; j < term->num_switch_cases; j++) {
                verify_cfg_add_edge(cfg, i, term->switch_blocks[j]);
            }
        }
    }

    size_t entry_index = verify_cfg_block_index(cfg, func->entry);
    if (entry_index == SIZE_MAX) {
        verify_cfg_destroy(cfg);
        return verify_fail(error, error_len,
                           "function %s entry block is not in the CFG",
                           func_name(func));
    }
    cfg->reachable[entry_index] = true;

    bool changed;
    do {
        changed = false;
        for (size_t block = 0; block < num_blocks; block++) {
            if (cfg->reachable[block]) continue;
            const uint64_t *preds =
                verify_cfg_const_set(cfg->preds, cfg, block);
            for (size_t pred = 0; pred < num_blocks; pred++) {
                if (cfg->reachable[pred] && bitset_contains(preds, pred)) {
                    cfg->reachable[block] = true;
                    changed = true;
                    break;
                }
            }
        }
    } while (changed);

    for (size_t i = 0; i < num_blocks; i++) {
        if (!cfg->reachable[i]) {
            const char *unreachable_name = block_name(cfg->blocks[i]);
            verify_cfg_destroy(cfg);
            return verify_fail(error, error_len,
                               "block %s in function %s is unreachable from the entry block",
                               unreachable_name, func_name(func));
        }
    }

    /* Entry is dominated only by itself.  Every other reachable block starts
     * with the universal set and is iteratively intersected over predecessors. */
    bitset_add(verify_cfg_set(cfg->doms, cfg, entry_index), entry_index);
    for (size_t block = 0; block < num_blocks; block++) {
        if (block == entry_index) continue;
        uint64_t *dom = verify_cfg_set(cfg->doms, cfg, block);
        for (size_t word = 0; word < words; word++) dom[word] = UINT64_MAX;
        if (num_blocks % 64 != 0) {
            dom[words - 1] &= (UINT64_C(1) << (num_blocks % 64)) - 1;
        }
    }

    uint64_t *next_dom = anvil_ctx_malloc(ctx, words * sizeof(*next_dom));
    if (!next_dom) {
        verify_cfg_destroy(cfg);
        return verify_fail(error, error_len,
                           "out of memory while computing dominators for function %s",
                           func_name(func));
    }

    do {
        changed = false;
        for (size_t block = 0; block < num_blocks; block++) {
            if (block == entry_index) continue;

            for (size_t word = 0; word < words; word++) {
                next_dom[word] = UINT64_MAX;
            }

            bool saw_pred = false;
            const uint64_t *preds =
                verify_cfg_const_set(cfg->preds, cfg, block);
            for (size_t pred = 0; pred < num_blocks; pred++) {
                if (!bitset_contains(preds, pred)) continue;
                const uint64_t *pred_dom =
                    verify_cfg_const_set(cfg->doms, cfg, pred);
                for (size_t word = 0; word < words; word++) {
                    next_dom[word] &= pred_dom[word];
                }
                saw_pred = true;
            }

            /* Reachability guarantees a non-entry block has a predecessor. */
            if (!saw_pred) {
                free(next_dom);
                verify_cfg_destroy(cfg);
                return verify_fail(error, error_len,
                                   "reachable block %s in function %s has no predecessor",
                                   block_name(cfg->blocks[block]), func_name(func));
            }
            bitset_add(next_dom, block);

            uint64_t *dom = verify_cfg_set(cfg->doms, cfg, block);
            if (memcmp(dom, next_dom, words * sizeof(*dom)) != 0) {
                memcpy(dom, next_dom, words * sizeof(*dom));
                changed = true;
            }
        }
    } while (changed);

    free(next_dom);
    return true;
}

static bool verify_cfg_dominates(const verify_cfg_t *cfg,
                                 const anvil_block_t *definition,
                                 const anvil_block_t *use)
{
    size_t definition_index = verify_cfg_block_index(cfg, definition);
    size_t use_index = verify_cfg_block_index(cfg, use);
    if (definition_index == SIZE_MAX || use_index == SIZE_MAX) return false;
    return bitset_contains(verify_cfg_const_set(cfg->doms, cfg, use_index),
                           definition_index);
}

static bool instr_precedes(const anvil_instr_t *definition,
                           const anvil_instr_t *use)
{
    if (!definition || !use || definition->parent != use->parent) return false;
    for (const anvil_instr_t *instr = definition; instr; instr = instr->next) {
        if (instr == use) return instr != definition;
    }
    return false;
}

static bool verify_ssa_use(const anvil_func_t *func,
                           const verify_cfg_t *cfg,
                           const anvil_instr_t *use,
                           const anvil_value_t *value,
                           char *error,
                           size_t error_len)
{
    if (value && (value->kind == ANVIL_VAL_CONST_ARRAY ||
                  value->kind == ANVIL_VAL_CONST_STRUCT)) {
        for (size_t i = 0; i < value->data.aggregate.num_elements; i++) {
            if (!verify_ssa_use(func, cfg, use,
                                value->data.aggregate.elements[i],
                                error, error_len)) {
                return false;
            }
        }
        return true;
    }
    if (!value || value->kind != ANVIL_VAL_INSTR) return true;

    const anvil_instr_t *definition = value->data.instr;
    if (definition->parent == use->parent) {
        if (instr_precedes(definition, use)) return true;
        return verify_fail(error, error_len,
                           "instruction result in block %s of function %s is used before its definition",
                           block_name(use->parent), func_name(func));
    }
    if (verify_cfg_dominates(cfg, definition->parent, use->parent)) return true;
    return verify_fail(error, error_len,
                       "instruction result from block %s does not dominate its use in block %s of function %s",
                       block_name(definition->parent), block_name(use->parent),
                       func_name(func));
}

static bool verify_phi_value_dominates_edge(const anvil_func_t *func,
                                            const verify_cfg_t *cfg,
                                            const anvil_instr_t *phi,
                                            const anvil_value_t *value,
                                            const anvil_block_t *pred,
                                            char *error,
                                            size_t error_len)
{
    if (value && (value->kind == ANVIL_VAL_CONST_ARRAY ||
                  value->kind == ANVIL_VAL_CONST_STRUCT)) {
        for (size_t i = 0; i < value->data.aggregate.num_elements; i++) {
            if (!verify_phi_value_dominates_edge(
                    func, cfg, phi, value->data.aggregate.elements[i], pred,
                    error, error_len)) {
                return false;
            }
        }
        return true;
    }
    if (!value || value->kind != ANVIL_VAL_INSTR) return true;

    const anvil_instr_t *definition = value->data.instr;
    if (definition->parent == pred) {
        if (definition != pred->last && instr_precedes(definition, pred->last)) {
            return true;
        }
    } else if (verify_cfg_dominates(cfg, definition->parent, pred)) {
        return true;
    }

    return verify_fail(error, error_len,
                       "PHI in block %s of function %s has a value from block %s that does not dominate incoming edge from %s",
                       block_name(phi->parent), func_name(func),
                       block_name(definition->parent), block_name(pred));
}

static bool verify_phi_edge_use(const anvil_func_t *func,
                                const verify_cfg_t *cfg,
                                const anvil_instr_t *phi,
                                size_t incoming_index,
                                char *error,
                                size_t error_len)
{
    return verify_phi_value_dominates_edge(
        func, cfg, phi, phi->operands[incoming_index],
        phi->phi_blocks[incoming_index], error, error_len);
}

static bool verify_ssa_dominance(const anvil_func_t *func,
                                 const verify_cfg_t *cfg,
                                 char *error,
                                 size_t error_len)
{
    for (const anvil_block_t *block = func->blocks; block; block = block->next) {
        for (const anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_PHI) {
                for (size_t i = 0; i < instr->num_phi_incoming; i++) {
                    if (!verify_phi_edge_use(func, cfg, instr, i,
                                             error, error_len)) {
                        return false;
                    }
                }
                continue;
            }
            for (size_t i = 0; i < instr->num_operands; i++) {
                if (!verify_ssa_use(func, cfg, instr, instr->operands[i],
                                    error, error_len)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool verify_same_type_operands(const anvil_module_t *mod,
                                      const anvil_func_t *func,
                                      const anvil_instr_t *instr,
                                      const char *kind,
                                      char *error,
                                      size_t error_len)
{
    if (instr->num_operands != 2 || !instr->result) {
        return verify_fail(error, error_len,
                           "%s in function %s must have two operands and a result",
                           kind, func_name(func));
    }

    anvil_value_t *lhs = instr->operands[0];
    anvil_value_t *rhs = instr->operands[1];
    if (!verify_value_ref(mod, func, lhs, error, error_len) ||
        !verify_value_ref(mod, func, rhs, error, error_len)) {
        return false;
    }
    if (!type_equal(lhs->type, rhs->type) ||
        !type_equal(instr->result->type, lhs->type)) {
        return verify_fail(error, error_len,
                           "%s in function %s has operand type mismatch",
                           kind, func_name(func));
    }
    return true;
}

static bool verify_binop(const anvil_module_t *mod,
                         const anvil_func_t *func,
                         const anvil_instr_t *instr,
                         char *error,
                         size_t error_len)
{
    if (!verify_same_type_operands(mod, func, instr, "binary instruction",
                                   error, error_len)) {
        return false;
    }

    if (anvil_sem_binary_types(instr->op,
                               instr->operands[0]->type,
                               instr->operands[1]->type,
                               instr->result->type)) return true;

    return verify_fail(error, error_len,
                       "binary instruction in function %s uses an invalid operand type",
                       func_name(func));
}

static bool verify_shift(const anvil_module_t *mod,
                         const anvil_func_t *func,
                         const anvil_instr_t *instr,
                         char *error,
                         size_t error_len)
{
    if (instr->num_operands != 2 || !instr->result) {
        return verify_fail(error, error_len,
                           "shift in function %s must have two operands and a result",
                           func_name(func));
    }
    anvil_value_t *value = instr->operands[0];
    anvil_value_t *amount = instr->operands[1];
    if (!verify_value_ref(mod, func, value, error, error_len) ||
        !verify_value_ref(mod, func, amount, error, error_len)) {
        return false;
    }
    if (!type_is_integer(value->type) ||
        value->type->kind == ANVIL_TYPE_I1 ||
        !type_is_integer(amount->type) ||
        amount->type->kind == ANVIL_TYPE_I1 ||
        !type_equal(instr->result->type, value->type)) {
        return verify_fail(error, error_len,
                           "shift in function %s has invalid operand type",
                           func_name(func));
    }
    return true;
}

static bool verify_cmp(const anvil_module_t *mod,
                       const anvil_func_t *func,
                       const anvil_instr_t *instr,
                       char *error,
                       size_t error_len)
{
    if (instr->num_operands != 2 || !instr->result) {
        return verify_fail(error, error_len,
                           "comparison in function %s must have two operands and a result",
                           func_name(func));
    }
    anvil_value_t *lhs = instr->operands[0];
    anvil_value_t *rhs = instr->operands[1];
    if (!verify_value_ref(mod, func, lhs, error, error_len) ||
        !verify_value_ref(mod, func, rhs, error, error_len)) {
        return false;
    }
    if (!anvil_sem_cmp_types(instr->op, lhs->type, rhs->type,
                             instr->result->type)) {
        return verify_fail(error, error_len,
                           "comparison in function %s has operand/result type mismatch",
                           func_name(func));
    }
    if (instr->op == ANVIL_OP_FCMP &&
        (unsigned)instr->fcmp_pred > (unsigned)ANVIL_FCMP_TRUE) {
        return verify_fail(error, error_len,
                           "floating comparison in function %s has invalid predicate",
                           func_name(func));
    }
    return true;
}

static bool verify_unop(const anvil_module_t *mod,
                        const anvil_func_t *func,
                        const anvil_instr_t *instr,
                        char *error,
                        size_t error_len)
{
    if (instr->num_operands != 1 || !instr->result) {
        return verify_fail(error, error_len,
                           "unary instruction in function %s must have one operand and a result",
                           func_name(func));
    }
    anvil_value_t *src = instr->operands[0];
    if (!verify_value_ref(mod, func, src, error, error_len)) return false;
    if (!type_equal(src->type, instr->result->type)) {
        return verify_fail(error, error_len,
                           "unary instruction in function %s has result type mismatch",
                           func_name(func));
    }

    if (anvil_sem_unary_types(instr->op, src->type,
                              instr->result->type)) return true;

    return verify_fail(error, error_len,
                       "unary instruction in function %s uses an invalid operand type",
                       func_name(func));
}

static bool verify_cast(const anvil_module_t *mod,
                        const anvil_func_t *func,
                        const anvil_instr_t *instr,
                        char *error,
                        size_t error_len)
{
    if (instr->num_operands != 1 || !instr->result) {
        return verify_fail(error, error_len,
                           "cast in function %s must have one operand and a result",
                           func_name(func));
    }
    anvil_value_t *src = instr->operands[0];
    anvil_type_t *dst_type = instr->result->type;
    if (!verify_value_ref(mod, func, src, error, error_len)) return false;

    if (anvil_sem_cast_types(instr->op, src->type, dst_type)) return true;

    return verify_fail(error, error_len,
                       "cast in function %s uses incompatible source/result types",
                       func_name(func));
}

static bool verify_memory(const anvil_module_t *mod,
                          const anvil_func_t *func,
                          const anvil_instr_t *instr,
                          char *error,
                          size_t error_len)
{
    if (instr->op == ANVIL_OP_LOAD) {
        if (instr->num_operands != 1 || !instr->result) {
            return verify_fail(error, error_len,
                               "load in function %s must have one address and a result",
                               func_name(func));
        }
        anvil_value_t *addr = instr->operands[0];
        if (!verify_value_ref(mod, func, addr, error, error_len)) return false;
        anvil_type_t *object_type = memory_object_type(addr);
        if (!object_type || !type_equal(object_type, instr->result->type)) {
            return verify_fail(error, error_len,
                               "load in function %s has address/result type mismatch",
                               func_name(func));
        }
        return true;
    }

    if (instr->op == ANVIL_OP_STORE) {
        if (instr->num_operands != 2 || instr->result) {
            return verify_fail(error, error_len,
                               "store in function %s must have value/address operands and no result",
                               func_name(func));
        }
        anvil_value_t *value = instr->operands[0];
        anvil_value_t *addr = instr->operands[1];
        if (!verify_value_ref(mod, func, value, error, error_len) ||
            !verify_value_ref(mod, func, addr, error, error_len)) {
            return false;
        }
        anvil_type_t *object_type = memory_object_type(addr);
        if (!object_type || !type_equal(object_type, value->type)) {
            return verify_fail(error, error_len,
                               "store in function %s has value/address type mismatch",
                               func_name(func));
        }
        return true;
    }

    return verify_fail(error, error_len,
                       "internal verifier error in function %s",
                       func_name(func));
}

static bool verify_gep(const anvil_module_t *mod,
                       const anvil_func_t *func,
                       const anvil_instr_t *instr,
                       char *error,
                       size_t error_len)
{
    if (instr->num_operands < 2 || !instr->result ||
        instr->result->type->kind != ANVIL_TYPE_PTR || !instr->aux_type) {
        return verify_fail(error, error_len,
                           "GEP in function %s must have source type, base, index, and pointer result",
                           func_name(func));
    }
    if (!verify_value_ref(mod, func, instr->operands[0], error, error_len)) {
        return false;
    }
    anvil_type_t *base_type = memory_object_type(instr->operands[0]);
    if (!base_type || !type_equal(base_type, instr->aux_type)) {
        return verify_fail(error, error_len,
                           "GEP in function %s base/source element types do not match",
                           func_name(func));
    }
    anvil_type_t *current = instr->aux_type;
    int64_t constant_offset = 0;
    for (size_t i = 1; i < instr->num_operands; i++) {
        anvil_value_t *index = instr->operands[i];
        if (!verify_value_ref(mod, func, index, error, error_len)) return false;
        anvil_gep_step_t step;
        if (!type_is_integer(index->type) ||
            !anvil_gep_analyze_step(&current, index, i - 1, &step)) {
            return verify_fail(error, error_len,
                               "GEP in function %s has an invalid typed index walk",
                               func_name(func));
        }
        if (index->kind == ANVIL_VAL_CONST_INT) {
            int64_t ignored;
            if (!anvil_gep_const_step_offset(&step, index, &ignored)) {
                return verify_fail(error, error_len,
                                   "GEP in function %s has a constant byte-offset overflow",
                                   func_name(func));
            }
            if (!anvil_gep_accumulate_offset(&constant_offset, ignored)) {
                return verify_fail(error, error_len,
                                   "GEP in function %s has accumulated offset overflow",
                                   func_name(func));
            }
        }
    }
    if (!type_equal(instr->result->type->data.pointee, current)) {
        return verify_fail(error, error_len,
                           "GEP in function %s has an incorrect inferred result type",
                           func_name(func));
    }
    return true;
}

static bool verify_struct_gep(const anvil_module_t *mod,
                              const anvil_func_t *func,
                              const anvil_instr_t *instr,
                              char *error,
                              size_t error_len)
{
    if (instr->num_operands != 2 || !instr->result ||
        instr->result->type->kind != ANVIL_TYPE_PTR ||
        !instr->aux_type || instr->aux_type->kind != ANVIL_TYPE_STRUCT ||
        !instr->aux_type->data.struc.complete) {
        return verify_fail(error, error_len,
                           "struct GEP in function %s is malformed",
                           func_name(func));
    }

    anvil_value_t *base = instr->operands[0];
    anvil_value_t *index = instr->operands[1];
    if (!verify_value_ref(mod, func, base, error, error_len) ||
        !verify_value_ref(mod, func, index, error, error_len)) {
        return false;
    }
    if (!base->type || base->type->kind != ANVIL_TYPE_PTR ||
        !type_equal(base->type->data.pointee, instr->aux_type) ||
        index->kind != ANVIL_VAL_CONST_INT ||
        index->data.i < 0 ||
        (size_t)index->data.i >= instr->aux_type->data.struc.num_fields) {
        return verify_fail(error, error_len,
                           "struct GEP in function %s has invalid base or field index",
                           func_name(func));
    }

    anvil_type_t *field_type =
        instr->aux_type->data.struc.fields[(size_t)index->data.i];
    if (!type_equal(instr->result->type->data.pointee, field_type)) {
        return verify_fail(error, error_len,
                           "struct GEP in function %s has field/result type mismatch",
                           func_name(func));
    }
    return true;
}

static bool verify_call(const anvil_module_t *mod,
                        const anvil_func_t *func,
                        const anvil_instr_t *instr,
                        char *error,
                        size_t error_len)
{
    if (instr->num_operands < 1) {
        return verify_fail(error, error_len,
                           "call in function %s must have a callee operand",
                           func_name(func));
    }
    anvil_value_t *callee = instr->operands[0];
    if (!verify_value_ref(mod, func, callee, error, error_len)) return false;

    anvil_type_t *fn_type = callee_func_type(callee);
    if (!fn_type || fn_type->owner_ctx != mod->ctx ||
        !fn_type->data.func.ret ||
        fn_type->data.func.ret->owner_ctx != mod->ctx ||
        fn_type->data.func.ret->kind == ANVIL_TYPE_FUNC ||
        (fn_type->data.func.ret->kind != ANVIL_TYPE_VOID &&
         !anvil_sem_type_is_sized(fn_type->data.func.ret)) ||
        (fn_type->data.func.num_params > 0 &&
         !fn_type->data.func.params)) {
        return verify_fail(error, error_len,
                           "call in function %s targets a non-function or malformed signature",
                           func_name(func));
    }
    anvil_cc_t effective_cc = ANVIL_CC_DEFAULT;
    if (!anvil_cc_resolve(mod->ctx, fn_type->data.func.cc, &effective_cc) ||
        effective_cc != fn_type->data.func.cc ||
        instr->call_cc != effective_cc) {
        return verify_fail(error, error_len,
                           "call in function %s has an invalid or mismatched calling convention",
                           func_name(func));
    }

    size_t num_args = instr->num_operands - 1;
    size_t num_fixed = fn_type->data.func.num_params;
    if ((!fn_type->data.func.variadic && num_args != num_fixed) ||
        (fn_type->data.func.variadic && num_args < num_fixed)) {
        return verify_fail(error, error_len,
                           "call in function %s has wrong argument count",
                           func_name(func));
    }

    for (size_t i = 0; i < num_args; i++) {
        anvil_value_t *arg = instr->operands[i + 1];
        if (!verify_value_ref(mod, func, arg, error, error_len)) return false;
        if (i < num_fixed &&
            (!fn_type->data.func.params[i] ||
             fn_type->data.func.params[i]->owner_ctx != mod->ctx ||
             !anvil_sem_type_is_sized(fn_type->data.func.params[i]) ||
             !type_equal(arg->type, fn_type->data.func.params[i]))) {
            return verify_fail(error, error_len,
                               "call in function %s has argument type mismatch",
                               func_name(func));
        }
    }

    anvil_type_t *ret_type = fn_type->data.func.ret;
    if (type_is_void(ret_type)) {
        if (!instr->result) return true;
    } else if (instr->result && type_equal(instr->result->type, ret_type)) {
        return true;
    }

    return verify_fail(error, error_len,
                       "call in function %s has return type mismatch",
                       func_name(func));
}

static bool verify_select(const anvil_module_t *mod,
                          const anvil_func_t *func,
                          const anvil_instr_t *instr,
                          char *error,
                          size_t error_len)
{
    if (instr->num_operands != 3 || !instr->result) {
        return verify_fail(error, error_len,
                           "select in function %s must have cond/then/else operands and a result",
                           func_name(func));
    }
    anvil_value_t *cond = instr->operands[0];
    anvil_value_t *then_val = instr->operands[1];
    anvil_value_t *else_val = instr->operands[2];
    if (!verify_value_ref(mod, func, cond, error, error_len) ||
        !verify_value_ref(mod, func, then_val, error, error_len) ||
        !verify_value_ref(mod, func, else_val, error, error_len)) {
        return false;
    }
    if (!type_is_bool_like(cond->type) ||
        !type_equal(then_val->type, else_val->type) ||
        !type_equal(instr->result->type, then_val->type)) {
        return verify_fail(error, error_len,
                           "select in function %s has operand/result type mismatch",
                           func_name(func));
    }
    return true;
}

static bool verify_phi(const anvil_module_t *mod,
                       const anvil_func_t *func,
                       const anvil_instr_t *instr,
                       char *error,
                       size_t error_len)
{
    if (!instr->result || instr->num_phi_incoming == 0 ||
        instr->num_operands != instr->num_phi_incoming ||
        !instr->phi_blocks) {
        return verify_fail(error, error_len,
                           "PHI in function %s is malformed",
                           func_name(func));
    }

    for (size_t i = 0; i < instr->num_phi_incoming; i++) {
        anvil_value_t *incoming = instr->operands[i];
        anvil_block_t *incoming_block = instr->phi_blocks[i];
        if (!verify_value_ref(mod, func, incoming, error, error_len)) {
            return false;
        }
        if (!type_equal(incoming->type, instr->result->type)) {
            return verify_fail(error, error_len,
                               "PHI in function %s has incoming value type mismatch",
                               func_name(func));
        }
        if (!block_belongs_to_func(func, incoming_block) ||
            !block_has_successor(incoming_block, instr->parent)) {
            return verify_fail(error, error_len,
                               "PHI in function %s references a non-predecessor block",
                               func_name(func));
        }
        for (size_t j = 0; j < i; j++) {
            if (instr->phi_blocks[j] == incoming_block) {
                return verify_fail(error, error_len,
                                   "PHI in function %s has duplicate incoming predecessor %s",
                                   func_name(func), block_name(incoming_block));
            }
        }
    }

    for (const anvil_block_t *pred = func->blocks; pred; pred = pred->next) {
        if (block_has_successor(pred, instr->parent) &&
            !phi_has_incoming_from(instr, pred)) {
            return verify_fail(error, error_len,
                               "PHI in function %s is missing an incoming predecessor",
                               func_name(func));
        }
    }

    return true;
}

static bool verify_branch(const anvil_func_t *func,
                          const anvil_instr_t *instr,
                          char *error,
                          size_t error_len)
{
    if (instr->op == ANVIL_OP_BR) {
        if (instr->num_operands == 0 &&
            !instr->result &&
            block_belongs_to_func(func, instr->true_block)) {
            return true;
        }
        return verify_fail(error, error_len,
                           "branch in function %s has invalid target",
                           func_name(func));
    }

    if (instr->op == ANVIL_OP_BR_COND) {
        if (instr->num_operands != 1 || instr->result) {
            return verify_fail(error, error_len,
                               "conditional branch in function %s is malformed",
                               func_name(func));
        }
        anvil_value_t *cond = instr->operands[0];
        if (!verify_value_ref(func->parent, func, cond, error, error_len)) {
            return false;
        }
        if (!type_is_bool_like(cond->type)) {
            return verify_fail(error, error_len,
                               "conditional branch in function %s requires a boolean condition",
                               func_name(func));
        }
        if (block_belongs_to_func(func, instr->true_block) &&
            block_belongs_to_func(func, instr->false_block)) {
            return true;
        }
        return verify_fail(error, error_len,
                           "conditional branch in function %s has invalid target",
                           func_name(func));
    }

    return verify_fail(error, error_len,
                       "internal verifier error in function %s",
                       func_name(func));
}

static bool verify_switch(const anvil_module_t *mod,
                          const anvil_func_t *func,
                          const anvil_instr_t *instr,
                          char *error,
                          size_t error_len)
{
    if (instr->num_operands != instr->num_switch_cases + 1 ||
        instr->num_operands < 1 ||
        instr->result ||
        instr->false_block ||
        !block_belongs_to_func(func, instr->true_block)) {
        return verify_fail(error, error_len,
                           "switch in function %s is malformed",
                           func_name(func));
    }

    anvil_value_t *selector = instr->operands[0];
    if (!verify_value_ref(mod, func, selector, error, error_len)) return false;
    if (!type_is_integer(selector->type) ||
        selector->type->kind == ANVIL_TYPE_I1) {
        return verify_fail(error, error_len,
                           "switch in function %s requires an integer selector",
                           func_name(func));
    }

    for (size_t i = 0; i < instr->num_switch_cases; i++) {
        anvil_value_t *case_value = instr->operands[i + 1];
        anvil_block_t *case_block = instr->switch_blocks
            ? instr->switch_blocks[i]
            : NULL;
        if (!verify_value_ref(mod, func, case_value, error, error_len)) {
            return false;
        }
        if (case_value->kind != ANVIL_VAL_CONST_INT ||
            !type_equal(case_value->type, selector->type)) {
            return verify_fail(error, error_len,
                               "switch in function %s has case type mismatch",
                               func_name(func));
        }
        if (!block_belongs_to_func(func, case_block)) {
            return verify_fail(error, error_len,
                               "switch in function %s has a case with invalid target",
                               func_name(func));
        }
        for (size_t j = 0; j < i; j++) {
            if (instr->operands[j + 1] &&
                instr->operands[j + 1]->data.u == case_value->data.u) {
                return verify_fail(error, error_len,
                                   "switch in function %s has duplicate case value",
                                   func_name(func));
            }
        }
    }

    return true;
}

static bool verify_ret(const anvil_func_t *func,
                       const anvil_instr_t *instr,
                       char *error,
                       size_t error_len)
{
    anvil_type_t *ret_type = func->type->data.func.ret;
    if (type_is_void(ret_type)) {
        if (instr->num_operands == 0 && !instr->result) return true;
        return verify_fail(error, error_len,
                           "return in function %s returns a value from void function",
                           func_name(func));
    }

    if (instr->num_operands != 1 || instr->result) {
        return verify_fail(error, error_len,
                           "return in function %s must return one value",
                           func_name(func));
    }
    anvil_value_t *value = instr->operands[0];
    if (!verify_value_ref(func->parent, func, value, error, error_len)) {
        return false;
    }
    if (!value || !type_equal(value->type, ret_type)) {
        return verify_fail(error, error_len,
                           "return in function %s has result type mismatch",
                           func_name(func));
    }
    return true;
}

static bool verify_alloca(const anvil_module_t *mod,
                          const anvil_func_t *func,
                          const anvil_instr_t *instr,
                          char *error,
                          size_t error_len)
{
    if (!instr->result || instr->result->type->kind != ANVIL_TYPE_PTR ||
        !anvil_sem_type_is_sized(instr->result->type->data.pointee)) {
        return verify_fail(error, error_len,
                           "alloca in function %s must produce a pointer",
                           func_name(func));
    }
    if (instr->num_operands == 0) return true;
    if (instr->num_operands == 1) {
        if (!verify_value_ref(mod, func, instr->operands[0], error, error_len)) {
            return false;
        }
        if (type_is_integer(instr->operands[0]->type) &&
            instr->operands[0]->type->kind != ANVIL_TYPE_I1) return true;
    }
    return verify_fail(error, error_len,
                       "dynamic alloca in function %s requires one integer count",
                       func_name(func));
}

static bool verify_instr(const anvil_module_t *mod,
                         const anvil_func_t *func,
                         const anvil_instr_t *instr,
                         char *error,
                         size_t error_len)
{
    if (!instr) {
        return verify_fail(error, error_len,
                           "function %s contains a null instruction",
                           func_name(func));
    }
    if (instr->op < 0 || instr->op >= ANVIL_OP_COUNT) {
        return verify_fail(error, error_len,
                           "function %s contains an invalid opcode",
                           func_name(func));
    }
    if (instr->result) {
        if (instr->result->kind != ANVIL_VAL_INSTR ||
            instr->result->data.instr != instr ||
            !instr->result->type ||
            instr->result->owner_ctx != mod->ctx ||
            instr->result->type->owner_ctx != mod->ctx) {
            return verify_fail(error, error_len,
                               "function %s contains malformed instruction result",
                               func_name(func));
        }
    }

    switch (instr->op) {
        case ANVIL_OP_ADD:
        case ANVIL_OP_SUB:
        case ANVIL_OP_MUL:
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
        case ANVIL_OP_SMOD:
        case ANVIL_OP_UMOD:
        case ANVIL_OP_AND:
        case ANVIL_OP_OR:
        case ANVIL_OP_XOR:
        case ANVIL_OP_FADD:
        case ANVIL_OP_FSUB:
        case ANVIL_OP_FMUL:
        case ANVIL_OP_FDIV:
            return verify_binop(mod, func, instr, error, error_len);

        case ANVIL_OP_SHL:
        case ANVIL_OP_SHR:
        case ANVIL_OP_SAR:
            return verify_shift(mod, func, instr, error, error_len);

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
            return verify_cmp(mod, func, instr, error, error_len);

        case ANVIL_OP_NEG:
        case ANVIL_OP_NOT:
        case ANVIL_OP_FNEG:
        case ANVIL_OP_FABS:
            return verify_unop(mod, func, instr, error, error_len);

        case ANVIL_OP_TRUNC:
        case ANVIL_OP_ZEXT:
        case ANVIL_OP_SEXT:
        case ANVIL_OP_FPTRUNC:
        case ANVIL_OP_FPEXT:
        case ANVIL_OP_FPTOSI:
        case ANVIL_OP_FPTOUI:
        case ANVIL_OP_SITOFP:
        case ANVIL_OP_UITOFP:
        case ANVIL_OP_PTRTOINT:
        case ANVIL_OP_INTTOPTR:
        case ANVIL_OP_BITCAST:
            return verify_cast(mod, func, instr, error, error_len);

        case ANVIL_OP_LOAD:
        case ANVIL_OP_STORE:
            return verify_memory(mod, func, instr, error, error_len);

        case ANVIL_OP_ALLOCA:
            return verify_alloca(mod, func, instr, error, error_len);

        case ANVIL_OP_GEP:
            return verify_gep(mod, func, instr, error, error_len);

        case ANVIL_OP_STRUCT_GEP:
            return verify_struct_gep(mod, func, instr, error, error_len);

        case ANVIL_OP_CALL:
            return verify_call(mod, func, instr, error, error_len);

        case ANVIL_OP_SELECT:
            return verify_select(mod, func, instr, error, error_len);

        case ANVIL_OP_PHI:
            return verify_phi(mod, func, instr, error, error_len);

        case ANVIL_OP_BR:
        case ANVIL_OP_BR_COND:
            return verify_branch(func, instr, error, error_len);

        case ANVIL_OP_SWITCH:
            return verify_switch(mod, func, instr, error, error_len);

        case ANVIL_OP_RET:
            return verify_ret(func, instr, error, error_len);

        case ANVIL_OP_NOP:
            return instr->num_operands == 0 && !instr->result;

        case ANVIL_OP_COUNT:
            break;
    }

    return verify_fail(error, error_len,
                       "function %s uses invalid source opcode",
                       func_name(func));
}

static bool verify_function_shape(const anvil_func_t *func,
                                  char *error,
                                  size_t error_len)
{
    if (!func) {
        return verify_fail(error, error_len, "module contains a null function");
    }
    if (!func->parent) {
        return verify_fail(error, error_len,
                           "function %s has no parent module",
                           func_name(func));
    }
    if (!func->type || func->type->kind != ANVIL_TYPE_FUNC) {
        return verify_fail(error, error_len,
                           "function %s has non-function type",
                           func_name(func));
    }
    anvil_cc_t effective_cc = ANVIL_CC_DEFAULT;
    if (!anvil_cc_resolve(func->parent->ctx, func->type->data.func.cc,
                          &effective_cc) ||
        effective_cc != func->type->data.func.cc) {
        return verify_fail(error, error_len,
                           "function %s has a calling convention incompatible with its target",
                           func_name(func));
    }
    if (!func->parent->ctx || func->owner_ctx != func->parent->ctx ||
        func->type->owner_ctx != func->parent->ctx ||
        !func->value || func->value->owner_ctx != func->parent->ctx ||
        func->value->owner_module != func->parent ||
        func->value->kind != ANVIL_VAL_FUNC ||
        func->value->data.func != func ||
        !func->name || !func->value->name ||
        strcmp(func->name, func->value->name) != 0 ||
        !func->value->type ||
        func->value->type->owner_ctx != func->parent->ctx ||
        func->value->type->kind != ANVIL_TYPE_PTR ||
        func->value->type->data.pointee != func->type) {
        return verify_fail(error, error_len,
                           "function %s has a malformed callable value",
                           func_name(func));
    }
    if ((unsigned)func->linkage > (unsigned)ANVIL_LINK_WEAK) {
        return verify_fail(error, error_len,
                           "function %s has invalid linkage", func_name(func));
    }
    if (func->num_params != func->type->data.func.num_params) {
        return verify_fail(error, error_len,
                           "function %s parameter count does not match its type",
                           func_name(func));
    }
    if (func->num_params > 0 && !func->type->data.func.params) {
        return verify_fail(error, error_len,
                           "function %s has a malformed function type",
                           func_name(func));
    }
    anvil_type_t *return_type = func->type->data.func.ret;
    if (!return_type || return_type->owner_ctx != func->parent->ctx ||
        return_type->kind == ANVIL_TYPE_FUNC ||
        (return_type->kind != ANVIL_TYPE_VOID &&
         !anvil_sem_type_is_sized(return_type))) {
        return verify_fail(error, error_len,
                           "function %s has an invalid return type",
                           func_name(func));
    }
    for (size_t i = 0; i < func->type->data.func.num_params; i++) {
        anvil_type_t *param_type = func->type->data.func.params[i];
        if (!param_type || param_type->owner_ctx != func->parent->ctx ||
            param_type->kind == ANVIL_TYPE_VOID ||
            param_type->kind == ANVIL_TYPE_FUNC ||
            !anvil_sem_type_is_sized(param_type)) {
            return verify_fail(error, error_len,
                               "function %s has an invalid parameter type",
                               func_name(func));
        }
    }
    if (func->is_declaration) {
        return true;
    }
    for (size_t i = 0; i < func->num_params; i++) {
        anvil_value_t *param = func->params ? func->params[i] : NULL;
        if (!param ||
            param->kind != ANVIL_VAL_PARAM ||
            param->owner_ctx != func->parent->ctx ||
            param->owner_module != func->parent ||
            !param->type || param->type->owner_ctx != func->parent->ctx ||
            param->data.param.func != func ||
            param->data.param.index != i ||
            !type_equal(param->type, func->type->data.func.params[i])) {
            return verify_fail(error, error_len,
                               "function %s has malformed parameter %zu",
                               func_name(func), i);
        }
    }
    return true;
}

bool anvil_func_verify(const anvil_func_t *func, char *error, size_t error_len)
{
    if (error && error_len > 0) error[0] = '\0';
    if (!verify_function_shape(func, error, error_len)) return false;

    if (func->is_declaration) {
        if (!func->entry && !func->blocks) return true;
        return verify_fail(error, error_len,
                           "declaration %s must not have a body",
                           func_name(func));
    }

    if (!func->entry || !func->blocks) {
        return verify_fail(error, error_len,
                           "function %s must have an entry block",
                           func_name(func));
    }
    if (!block_belongs_to_func(func, func->entry)) {
        return verify_fail(error, error_len,
                           "function %s entry block is not in the function",
                           func_name(func));
    }

    const anvil_module_t *mod = func->parent;
    for (const anvil_block_t *block = func->blocks; block; block = block->next) {
        if (block->parent != func) {
            return verify_fail(error, error_len,
                               "function %s has a block with wrong parent",
                               func_name(func));
        }
        if (!block->first || !block->last) {
            return verify_fail(error, error_len,
                               "block %s in function %s is missing a terminator",
                               block_name(block), func_name(func));
        }

        bool saw_non_phi = false;
        bool saw_terminator = false;
        for (const anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->parent != block) {
                return verify_fail(error, error_len,
                                   "block %s in function %s has instruction with wrong parent",
                                   block_name(block), func_name(func));
            }
            if (saw_terminator) {
                return verify_fail(error, error_len,
                                   "block %s in function %s has instructions after a terminator",
                                   block_name(block), func_name(func));
            }
            if (instr->op == ANVIL_OP_PHI && saw_non_phi) {
                return verify_fail(error, error_len,
                                   "PHI in function %s must appear before non-PHI instructions",
                                   func_name(func));
            }
            if (instr->op != ANVIL_OP_PHI) saw_non_phi = true;

            if (!verify_instr(mod, func, instr, error, error_len)) return false;

            if (op_is_terminator(instr->op)) saw_terminator = true;
        }

        if (!saw_terminator) {
            return verify_fail(error, error_len,
                               "block %s in function %s is missing a terminator",
                               block_name(block), func_name(func));
        }
    }

    verify_cfg_t cfg;
    if (!verify_cfg_build(func, &cfg, error, error_len)) return false;
    bool valid_ssa = verify_ssa_dominance(func, &cfg, error, error_len);
    verify_cfg_destroy(&cfg);
    return valid_ssa;
}

bool anvil_module_verify(const anvil_module_t *mod, char *error, size_t error_len)
{
    if (error && error_len > 0) error[0] = '\0';
    if (!mod) return verify_fail(error, error_len, "module is null");
    if (!mod->ctx) {
        return verify_fail(error, error_len,
                           "module %s has no context",
                           mod->name ? mod->name : "<anon>");
    }

    for (const anvil_global_t *global = mod->globals; global; global = global->next) {
        if (!global->value || !global->value->type ||
            global->value->kind != ANVIL_VAL_GLOBAL ||
            global->value->owner_ctx != mod->ctx ||
            global->value->owner_module != mod ||
            !global->value->name ||
            anvil_module_lookup_symbol(mod, global->value->name) != global->value ||
            global->value->type->owner_ctx != mod->ctx) {
            return verify_fail(error, error_len,
                               "module %s contains a malformed global",
                               mod->name ? mod->name : "<anon>");
        }
        if ((unsigned)global->value->data.global.linkage >
                (unsigned)ANVIL_LINK_COMMON ||
            (global->value->data.global.is_declaration &&
             global->value->data.global.init)) {
            return verify_fail(error, error_len,
                               "module %s contains a global with invalid declaration state",
                               mod->name ? mod->name : "<anon>");
        }
        anvil_const_dag_status_t dag = global->value->data.global.init
            ? anvil_value_check_constant_dag_for_module(
                  global->value->data.global.init, mod->ctx, mod)
            : ANVIL_CONST_DAG_VALID;
        if (dag == ANVIL_CONST_DAG_NOMEM) {
            return verify_fail(error, error_len,
                               "out of memory validating a global initializer");
        }
        if (global->value->data.global.init &&
            (dag != ANVIL_CONST_DAG_VALID ||
             !type_equal(global->value->data.global.init->type,
                         global->value->type))) {
            return verify_fail(error, error_len,
                               "module %s has an invalid global initializer constant DAG",
                               mod->name ? mod->name : "<anon>");
        }
    }

    for (const anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (func->parent != mod || !func->value ||
            func->value->owner_module != mod || !func->value->name ||
            anvil_module_lookup_symbol(mod, func->value->name) != func->value) {
            return verify_fail(error, error_len,
                               "module %s contains a function with wrong parent",
                               mod->name ? mod->name : "<anon>");
        }
        if (!anvil_func_verify(func, error, error_len)) return false;
    }

    if (mod->num_symbols != mod->num_funcs + mod->num_globals) {
        return verify_fail(error, error_len,
                           "module %s has inconsistent symbol cardinality",
                           mod->name ? mod->name : "<anon>");
    }

    return true;
}
