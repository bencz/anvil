/*
 * ANVIL - Preparation of optimized IR for code generation
 *
 * Runs after the optimization pipeline (whose CSE/GVN would merge what it
 * duplicates), before a backend lowers the module.
 *
 * Late loop strength reduction (anvil_opt_loop_strength_late): pointer
 * streams for targets whose accesses advance their own base, once the
 * loop vectorizer, which wants base[i], is done.
 *
 * Address sinking: a GEP whose loads and stores sit in other blocks than
 * the GEP itself is recomputed in each of those blocks.  A backend folds
 * address arithmetic into the accesses of its own block only (MachineIR
 * has no dominance), so an address shared across blocks would otherwise
 * stay a computed register on every path, and the access through it
 * unfolded.  In SSA the GEP's operands dominate every block using it, so
 * the copy is always valid.  An index that is an integer extension from
 * another block is recomputed along with it, so that each block's address
 * can absorb its own extension (a target with extending index modes).
 *
 * Last, narrow signed loads whose sign no reader observes are marked
 * (sign_unobserved) for the backends to load zero-extended.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include "loop_utils.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct {
    anvil_instr_t *gep;
    anvil_instr_t *user;
    size_t operand;
} sink_use_t;

typedef struct {
    anvil_value_t **keys;
    size_t *uses;   /* every operand reference to the GEP */
    size_t capacity;
} gep_table_t;

static size_t gep_slot(const gep_table_t *table, const anvil_value_t *value)
{
    size_t slot = ((uintptr_t)value >> 4) & (table->capacity - 1);
    while (table->keys[slot] && table->keys[slot] != value)
        slot = (slot + 1) & (table->capacity - 1);
    return slot;
}

static bool is_address(const anvil_instr_t *instr)
{
    return (instr->op == ANVIL_OP_GEP || instr->op == ANVIL_OP_STRUCT_GEP) && instr->result;
}

/* Operand `operand` of `user` is the address it accesses. */
static bool accesses_through(const anvil_instr_t *user, size_t operand)
{
    return (user->op == ANVIL_OP_LOAD && operand == 0) || (user->op == ANVIL_OP_STORE && operand == 1);
}

static bool sink_addresses(anvil_func_t *func)
{
    anvil_ctx_t *ctx = func->parent ? func->parent->ctx : NULL;
    size_t geps = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next)
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
            geps += is_address(instr);
    if (!ctx || !geps)
        return true;

    gep_table_t table = {NULL, NULL, 16};
    while (table.capacity < 2 * geps)
        table.capacity *= 2;
    table.keys = calloc(table.capacity, sizeof(*table.keys));
    table.uses = calloc(table.capacity, sizeof(*table.uses));
    sink_use_t *moves = NULL;
    size_t num_moves = 0, cap_moves = 0;
    bool ok = table.keys && table.uses;
    for (anvil_block_t *block = func->blocks; ok && block; block = block->next)
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
            if (is_address(instr))
                table.keys[gep_slot(&table, instr->result)] = instr->result;

    /* Every reference to a GEP, and the accesses in other blocks. */
    for (anvil_block_t *block = func->blocks; ok && block; block = block->next) {
        for (anvil_instr_t *user = block->first; ok && user; user = user->next) {
            for (size_t operand = 0; operand < user->num_operands; operand++) {
                anvil_value_t *value = user->operands[operand];
                if (!value || value->kind != ANVIL_VAL_INSTR || table.keys[gep_slot(&table, value)] != value)
                    continue;
                table.uses[gep_slot(&table, value)]++;
                anvil_instr_t *gep = value->data.instr;
                if (user->op == ANVIL_OP_PHI || gep->parent == block || !accesses_through(user, operand))
                    continue;
                if (num_moves == cap_moves) {
                    size_t grown_cap = cap_moves ? cap_moves * 2 : 16;
                    sink_use_t *grown = realloc(moves, grown_cap * sizeof(*grown));
                    ok = grown != NULL;
                    if (!ok)
                        break;
                    moves = grown;
                    cap_moves = grown_cap;
                }
                moves[num_moves++] = (sink_use_t){gep, user, operand};
            }
        }
    }

    /* One copy per GEP and block, ahead of the block's first such access. */
    bool *done = ok && num_moves ? calloc(num_moves, sizeof(*done)) : NULL;
    ok = ok && (!num_moves || done);
    for (size_t m = 0; ok && m < num_moves; m++) {
        if (done[m])
            continue;
        anvil_instr_t *gep = moves[m].gep;
        anvil_block_t *block = moves[m].user->parent;
        anvil_instr_t *first = NULL;
        for (anvil_instr_t *instr = block->first; instr && !first; instr = instr->next)
            for (size_t k = m; k < num_moves && !first; k++)
                if (!done[k] && moves[k].gep == gep && moves[k].user == instr)
                    first = instr;
        anvil_instr_t *copy = first ? anvil_instr_create(ctx, gep->op, gep->result->type, gep->result->name) : NULL;
        ok = copy != NULL;
        for (size_t operand = 0; ok && operand < gep->num_operands; operand++) {
            anvil_value_t *value = gep->operands[operand];
            anvil_instr_t *cast = value && value->kind == ANVIL_VAL_INSTR ? value->data.instr : NULL;
            if (operand > 0 && cast && (cast->op == ANVIL_OP_SEXT || cast->op == ANVIL_OP_ZEXT) && cast->parent != block &&
                cast->num_operands == 1) {
                anvil_instr_t *widened = anvil_instr_create(ctx, cast->op, cast->result->type, cast->result->name);
                ok = widened && anvil_instr_add_operand(widened, cast->operands[0]);
                if (!ok)
                    break;
                anvil_opt_insert_before(first, widened);
                value = widened->result;
            }
            ok = anvil_instr_add_operand(copy, value);
        }
        if (!ok)
            break;
        copy->aux_type = gep->aux_type;
        anvil_opt_insert_before(first, copy);
        for (size_t k = m; k < num_moves; k++) {
            if (done[k] || moves[k].gep != gep || moves[k].user->parent != block)
                continue;
            moves[k].user->operands[moves[k].operand] = copy->result;
            table.uses[gep_slot(&table, gep->result)]--;
            done[k] = true;
        }
    }

    /* A GEP all of whose accesses moved goes. */
    for (size_t m = 0; ok && m < num_moves; m++) {
        anvil_instr_t *gep = moves[m].gep;
        size_t slot = gep_slot(&table, gep->result);
        if (gep->parent && table.uses[slot] == 0) {
            anvil_opt_erase_instr(gep);
            table.uses[slot] = SIZE_MAX;
        }
    }

    free(done);
    free(moves);
    free(table.keys);
    free(table.uses);
    return ok;
}

/* A narrow signed load is extended by its sign, which costs an instruction
 * where the target's loads only zero-extend (a PowerPC byte).  When every
 * reader sees only its low bits, or both sides of an equality alike, the
 * zero-extended value serves as well: == and != (operands extended alike),
 * zero extension, truncation, and the stored value of a store. */
static bool reads_low_bits(const anvil_instr_t *user, size_t operand)
{
    switch (user->op) {
    case ANVIL_OP_CMP_EQ:
    case ANVIL_OP_CMP_NE:
    case ANVIL_OP_ZEXT:
    case ANVIL_OP_TRUNC:
        return true;
    case ANVIL_OP_STORE:
        return operand == 0;
    default:
        return false;
    }
}

/* A same-width integer bitcast (a signedness change) passes the question
 * on to its own readers. */
static bool sign_relay(const anvil_instr_t *instr)
{
    return instr->op == ANVIL_OP_BITCAST && instr->num_operands == 1 && instr->result && anvil_type_is_integer(instr->result->type) &&
           anvil_type_is_integer(instr->operands[0]->type) &&
           anvil_type_bit_width(instr->result->type) == anvil_type_bit_width(instr->operands[0]->type);
}

static void mark_sign_unobserved(anvil_func_t *func)
{
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            anvil_type_t *type = instr->result ? instr->result->type : NULL;
            instr->sign_unobserved = (instr->op == ANVIL_OP_LOAD && type && anvil_type_is_integer(type) && type->kind != ANVIL_TYPE_I1 &&
                                      type->is_signed && anvil_type_bit_width(type) < 64) ||
                                     sign_relay(instr);
        }
    }
    /* Clear what some reader observes, through relays, to a fixed point. */
    for (bool changed = true; changed;) {
        changed = false;
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
                for (size_t o = 0; o < instr->num_operands; o++) {
                    anvil_value_t *operand = instr->operands[o];
                    if (operand && operand->kind == ANVIL_VAL_INSTR && operand->data.instr && operand->data.instr->sign_unobserved &&
                        !reads_low_bits(instr, o) && !(sign_relay(instr) && instr->sign_unobserved)) {
                        operand->data.instr->sign_unobserved = false;
                        changed = true;
                    }
                }
            }
        }
    }
    /* The flag is the loads' alone. */
    for (anvil_block_t *block = func->blocks; block; block = block->next)
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
            if (instr->op != ANVIL_OP_LOAD)
                instr->sign_unobserved = false;
}

/* An internal function called only directly is the module's own business:
 * where the target offers a cheaper convention for such functions
 * (anvil_ctx_local_call_convention: i386 passes the first integer
 * arguments in registers), its type and every call take it. */
static bool use_local_conventions(anvil_module_t *mod)
{
    anvil_ctx_t *ctx = mod->ctx;
    anvil_cc_t local = anvil_ctx_local_call_convention(ctx);
    if (local == ANVIL_CC_DEFAULT)
        return true;
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        anvil_type_t *type = func->type;
        if (func->is_declaration || func->linkage != ANVIL_LINK_INTERNAL || !type || type->kind != ANVIL_TYPE_FUNC || type->data.func.variadic ||
            !func->value)
            continue;
        anvil_cc_t current = type->data.func.cc, resolved;
        if (!anvil_cc_resolve(ctx, ANVIL_CC_DEFAULT, &resolved) || current != resolved || current == local)
            continue;
        bool plain = true;
        for (size_t p = 0; p < type->data.func.num_params && plain; p++)
            plain = anvil_type_func_param_attr(type, p) == ANVIL_PARAM_PLAIN;
        if (!plain || !anvil_opt_direct_calls(mod, func, &current, NULL))
            continue;
        anvil_type_t *changed = anvil_type_func_cc(ctx, type->data.func.ret, type->data.func.params, type->data.func.num_params, false, local);
        anvil_type_t *pointer = changed ? anvil_type_ptr(ctx, changed) : NULL;
        if (!pointer)
            return false;
        func->type = changed;
        func->value->type = pointer;
        for (anvil_func_t *caller = mod->funcs; caller; caller = caller->next)
            for (anvil_block_t *block = caller->blocks; block; block = block->next)
                for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
                    if (instr->op == ANVIL_OP_CALL && instr->num_operands && instr->operands[0] == func->value)
                        instr->call_cc = local;
    }
    return true;
}

/* Internal functions nothing alive refers to (inlined everywhere, or never
 * called) go: the live ones are those the module exports, those an
 * initializer names, and those a live function's instructions name. */
static bool remove_dead_functions(anvil_module_t *mod)
{
    size_t count = 0;
    for (anvil_func_t *func = mod->funcs; func; func = func->next)
        count++;
    if (!count)
        return true;
    anvil_func_t **funcs = malloc(count * sizeof(*funcs));
    bool *live = calloc(count, sizeof(*live));
    size_t *work = malloc(count * sizeof(*work));
    if (!funcs || !live || !work) {
        free(funcs);
        free(live);
        free(work);
        return false;
    }
    size_t at = 0, pending = 0;
    for (anvil_func_t *func = mod->funcs; func; func = func->next)
        funcs[at++] = func;
    for (size_t f = 0; f < count; f++) {
        bool root = funcs[f]->linkage != ANVIL_LINK_INTERNAL || funcs[f]->is_declaration || !funcs[f]->value;
        for (const anvil_global_t *global = mod->globals; !root && global; global = global->next)
            root = global->value && anvil_opt_initializer_names(global->value->data.global.init, funcs[f]->value);
        if (root) {
            live[f] = true;
            work[pending++] = f;
        }
    }
    while (pending) {
        anvil_func_t *func = funcs[work[--pending]];
        for (anvil_block_t *block = func->blocks; block; block = block->next)
            for (const anvil_instr_t *instr = block->first; instr; instr = instr->next)
                for (size_t o = 0; o < instr->num_operands; o++) {
                    const anvil_value_t *operand = instr->operands[o];
                    if (operand->kind == ANVIL_VAL_CONST_SYMBOL_ADDR || operand->kind == ANVIL_VAL_CONST_GEP)
                        operand = operand->data.reloc.symbol;
                    if (!operand || operand->kind != ANVIL_VAL_FUNC)
                        continue;
                    for (size_t f = 0; f < count; f++)
                        if (!live[f] && funcs[f]->value == operand) {
                            live[f] = true;
                            work[pending++] = f;
                        }
                }
    }
    for (size_t f = 0; f < count; f++)
        if (!live[f])
            anvil_module_unlink_func(mod, funcs[f]);
    free(funcs);
    free(live);
    free(work);
    return true;
}

bool anvil_opt_prepare_codegen(anvil_module_t *mod)
{
    if (!mod)
        return false;
    if (!remove_dead_functions(mod))
        return false;
    if (!use_local_conventions(mod))
        return false;
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (func->is_declaration)
            continue;
        if (anvil_opt_loop_strength_late(func) == ANVIL_PASS_RUN_ERROR || !sink_addresses(func))
            return false;
        mark_sign_unobserved(func);
    }
    return true;
}
