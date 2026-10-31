/*
 * ANVIL - Loop strength reduction of addresses (ANVIL_PASS_LOOP_STRENGTH)
 *
 * In a loop whose induction variable i steps by one, `gep T base, i, ...`
 * with base loop-invariant computes base + i * sizeof(T) every iteration
 * (an extension of i and a multiply when sizeof(T) is no scaled-index
 * size).  Each such stream gets a pointer PHI instead, starting at
 * `gep T base, start` and advancing by one T in the latch, and the access
 * becomes `gep T p, 0, ...`: an add per iteration replaces the multiply.
 *
 * An index narrower than a pointer is extended by its signedness, so the
 * rewrite is exact only while i cannot wrap inside the loop: the header
 * tests i < n (signed for a signed i, unsigned for an unsigned one) with n
 * loop-invariant, so every i the body sees is below n.
 *
 * The same proof widens a narrow induction variable first: I = ext(i)
 * steps by itself, so its extensions and indices read I, the header tests
 * I < ext(n), and other readers take trunc(I) (a register's low half), so
 * no extension runs per iteration.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

#include <stdlib.h>

enum { LS_STREAM_LIMIT = 8 };

typedef struct {
    anvil_value_t *base;
    anvil_type_t *element;
    anvil_value_t *index;   /* the IV, or its extension */
    anvil_instr_t *phi;     /* the stream's pointer */
} ls_stream_t;

static bool in_loop(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, const anvil_value_t *value)
{
    if (value->kind != ANVIL_VAL_INSTR || !value->data.instr || !value->data.instr->parent)
        return false;
    size_t block = anvil_opt_cfg_index(cfg, value->data.instr->parent);
    return block != SIZE_MAX && anvil_loop_contains(loops, loop, block);
}

/* sizeof(T) a scaled index cannot absorb (1, 2, 4, 8 can). */
static bool needs_multiply(anvil_type_t *element)
{
    size_t size = element ? element->size : 0;
    return size != 0 && size != 1 && size != 2 && size != 4 && size != 8;
}

/* Whether `index` (the IV or an extension of it) addresses with no wrap
 * inside the loop; `bounded`/`unsigned_bound`: the header's i < n kind. */
static bool exact_index(const anvil_value_t *index, const anvil_value_t *iv, size_t pointer_bits, bool bounded, bool unsigned_bound)
{
    const anvil_type_t *iv_type = iv->type;
    bool narrow = anvil_type_bit_width(iv_type) < pointer_bits;
    if (index == iv)
        return !narrow || (bounded && unsigned_bound == !iv_type->is_signed);
    if (index->kind != ANVIL_VAL_INSTR || index->data.instr->num_operands != 1 || index->data.instr->operands[0] != iv)
        return false;
    anvil_op_t op = index->data.instr->op;
    return bounded && ((op == ANVIL_OP_SEXT && iv_type->is_signed && !unsigned_bound) || (op == ANVIL_OP_ZEXT && !iv_type->is_signed && unsigned_bound));
}

/* `value` extended to `wide` at the end of the preheader (a constant
 * directly). */
static anvil_value_t *widen_value(anvil_ctx_t *ctx, anvil_block_t *preheader, anvil_op_t extend, anvil_type_t *wide, anvil_value_t *value,
                                  const char *name)
{
    if (value->kind == ANVIL_VAL_CONST_INT)
        return anvil_opt_make_const_int(ctx, wide, extend == ANVIL_OP_SEXT ? value->data.i : (int64_t)anvil_const_int_unsigned_value(value));
    anvil_instr_t *extension = anvil_instr_create(ctx, extend, wide, name);
    if (!extension || !anvil_instr_add_operand(extension, value))
        return NULL;
    anvil_opt_insert_before(preheader->last, extension);
    return extension->result;
}

/* Replaces the narrow induction variable `phi` (start, step i + 1, header
 * test i < n proving no wrap) by one of pointer width; see the header. */
static anvil_pass_result_t widen_iv(anvil_func_t *func, anvil_instr_t *phi, anvil_value_t *start, anvil_instr_t *step, anvil_instr_t *test,
                                    anvil_block_t *header, anvil_block_t *preheader)
{
    anvil_ctx_t *ctx = func->owner_ctx;
    anvil_value_t *iv = phi->result;
    bool is_signed = iv->type->is_signed;
    anvil_op_t extend = is_signed ? ANVIL_OP_SEXT : ANVIL_OP_ZEXT;
    anvil_type_t *wide = is_signed ? anvil_type_i64(ctx) : anvil_type_u64(ctx);
    /* Worth it only where an extension or an index reads i. */
    bool worth = false;
    for (anvil_block_t *block = func->blocks; block && !worth; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr && !worth; instr = instr->next) {
            for (size_t operand = 0; operand < instr->num_operands && !worth; operand++)
                worth = instr->operands[operand] == iv && ((instr->op == extend && anvil_types_equal(instr->result->type, wide)) ||
                                                           (instr->op == ANVIL_OP_GEP && operand >= 1));
        }
    }
    if (!worth || !wide)
        return ANVIL_PASS_RUN_UNCHANGED;

    anvil_value_t *start_wide = widen_value(ctx, preheader, extend, wide, start, "iv.start");
    anvil_value_t *bound_wide = widen_value(ctx, preheader, extend, wide, test->operands[1], "iv.bound");
    anvil_instr_t *widened = anvil_instr_create(ctx, ANVIL_OP_PHI, wide, "iv.wide");
    anvil_instr_t *next = anvil_instr_create(ctx, ANVIL_OP_ADD, wide, "iv.next");
    anvil_instr_t *narrow = anvil_instr_create(ctx, ANVIL_OP_TRUNC, iv->type, "iv.narrow");
    anvil_value_t *one = anvil_opt_make_const_int(ctx, wide, 1);
    if (!start_wide || !bound_wide || !widened || !next || !narrow || !one || !anvil_instr_add_operand(next, widened->result) ||
        !anvil_instr_add_operand(next, one) || !anvil_instr_add_operand(narrow, widened->result))
        return ANVIL_PASS_RUN_ERROR;
    anvil_opt_insert_before(header->first, widened);
    anvil_instr_t *body = header->first;
    while (body && body->op == ANVIL_OP_PHI)
        body = body->next;
    if (!body)
        return ANVIL_PASS_RUN_ERROR;
    anvil_opt_insert_before(body, narrow);
    /* The step becomes the narrow view of the wide one, in its place (it
     * dominates the latch's end). */
    anvil_block_t *latch = NULL;
    for (size_t item = 0; item < phi->num_phi_incoming; item++) {
        if (phi->phi_blocks[item] != preheader)
            latch = phi->phi_blocks[item];
    }
    anvil_opt_insert_before(step, next);
    step->op = ANVIL_OP_TRUNC;
    step->operands[0] = next->result;
    step->num_operands = 1;
    if (!latch || !anvil_phi_add_incoming(widened->result, start_wide, preheader) || !anvil_phi_add_incoming(widened->result, next->result, latch))
        return ANVIL_PASS_RUN_ERROR;
    test->operands[0] = widened->result;
    test->operands[1] = bound_wide;

    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr == phi)
                continue;
            for (size_t operand = 0; operand < instr->num_operands; operand++) {
                if (instr->operands[operand] != iv)
                    continue;
                /* An extension's readers take I; the extension itself (now
                 * dead) reads the narrow view so nothing names i. */
                if (instr->op == extend && anvil_types_equal(instr->result->type, wide))
                    anvil_opt_replace_uses_in_func(func, instr->result, widened->result);
                instr->operands[operand] = instr->op == ANVIL_OP_GEP && operand >= 1 ? widened->result : narrow->result;
            }
        }
    }
    anvil_opt_erase_instr(phi);
    return ANVIL_PASS_RUN_CHANGED;
}

static anvil_pass_result_t reduce_loop(anvil_func_t *func, const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop)
{
    const anvil_loop_info_t *info = &loops->loops[loop];
    if (info->latch_count != 1 || info->preheader == SIZE_MAX)
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_block_t *header = cfg->blocks[info->header], *preheader = cfg->blocks[info->preheader];
    if (!preheader->last || preheader->last->op != ANVIL_OP_BR || !header->last || header->last->op != ANVIL_OP_BR_COND)
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_block_t *latch = NULL;
    for (size_t at = cfg->predecessor_offsets[info->header]; at < cfg->predecessor_offsets[info->header + 1]; at++) {
        size_t pred = cfg->predecessors[at];
        if (anvil_loop_contains(loops, loop, pred))
            latch = cfg->blocks[pred];
    }
    if (!latch || !latch->last)
        return ANVIL_PASS_RUN_UNCHANGED;

    /* The header's test: br (i < n), in-loop successor taken when true. */
    anvil_instr_t *test = header->last->operands[0]->kind == ANVIL_VAL_INSTR ? header->last->operands[0]->data.instr : NULL;
    size_t true_index = header->last->true_block ? anvil_opt_cfg_index(cfg, header->last->true_block) : SIZE_MAX;
    bool continues_on_true = true_index != SIZE_MAX && anvil_loop_contains(loops, loop, true_index);

    anvil_ctx_t *ctx = func->owner_ctx;
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
    size_t pointer_bits = arch ? arch->ptr_size * 8 : 64;
    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;

    /* Narrow induction variables bounded by the header's test first. */
    for (anvil_instr_t *phi = header->first; phi && phi->op == ANVIL_OP_PHI; phi = phi->next) {
        if (pointer_bits != 64 || phi->num_phi_incoming != 2 || !phi->result || !anvil_type_is_integer(phi->result->type) ||
            anvil_type_bit_width(phi->result->type) >= 64 || !test || !continues_on_true || test->num_operands != 2 ||
            test->operands[0] != phi->result || in_loop(cfg, loops, loop, test->operands[1]) ||
            test->op != (phi->result->type->is_signed ? ANVIL_OP_CMP_LT : ANVIL_OP_CMP_ULT))
            continue;
        anvil_value_t *start = NULL, *next = NULL;
        for (size_t item = 0; item < 2; item++) {
            if (phi->phi_blocks[item] == preheader)
                start = phi->operands[item];
            else if (phi->phi_blocks[item] == latch)
                next = phi->operands[item];
        }
        anvil_instr_t *step = next && next->kind == ANVIL_VAL_INSTR ? next->data.instr : NULL;
        if (!start || !step || step->op != ANVIL_OP_ADD || step->num_operands != 2 ||
            !((step->operands[0] == phi->result && anvil_opt_is_one(step->operands[1])) ||
              (step->operands[1] == phi->result && anvil_opt_is_one(step->operands[0]))))
            continue;
        if (step->operands[0] != phi->result) {
            step->operands[1] = step->operands[0];
            step->operands[0] = phi->result;
        }
        anvil_pass_result_t widened = widen_iv(func, phi, start, step, test, header, preheader);
        if (widened == ANVIL_PASS_RUN_ERROR)
            return widened;
        if (widened == ANVIL_PASS_RUN_CHANGED) {
            result = widened;
            break;  /* the header's PHIs changed: the streams below see I */
        }
    }

    for (anvil_instr_t *phi = header->first; phi && phi->op == ANVIL_OP_PHI; phi = phi->next) {
        if (phi->num_phi_incoming != 2 || !phi->result || !anvil_type_is_integer(phi->result->type))
            continue;
        anvil_value_t *start = NULL, *next = NULL;
        for (size_t item = 0; item < 2; item++) {
            if (phi->phi_blocks[item] == preheader)
                start = phi->operands[item];
            else if (phi->phi_blocks[item] == latch)
                next = phi->operands[item];
        }
        const anvil_instr_t *step = next && next->kind == ANVIL_VAL_INSTR ? next->data.instr : NULL;
        if (!start || !step || step->op != ANVIL_OP_ADD ||
            !((step->operands[0] == phi->result && anvil_opt_is_one(step->operands[1])) ||
              (step->operands[1] == phi->result && anvil_opt_is_one(step->operands[0]))))
            continue;
        const anvil_value_t *iv = phi->result;
        bool bounded = false, unsigned_bound = false;
        if (test && continues_on_true && test->num_operands == 2 && !in_loop(cfg, loops, loop, test->operands[1]) && test->operands[0] == iv &&
            (test->op == ANVIL_OP_CMP_LT || test->op == ANVIL_OP_CMP_ULT)) {
            bounded = true;
            unsigned_bound = test->op == ANVIL_OP_CMP_ULT;
        }

        ls_stream_t streams[LS_STREAM_LIMIT];
        size_t num_streams = 0;
        for (size_t member = 0; member < cfg->count; member++) {
            if (!anvil_loop_contains(loops, loop, member))
                continue;
            for (anvil_instr_t *gep = cfg->blocks[member]->first; gep; gep = gep->next) {
                /* The IV indexes the base's elements (gep T base, i, ...)
                 * or, after a leading zero, an array's (gep [N x T] base,
                 * 0, i, ...): T is the stream's element. */
                size_t level = gep->op == ANVIL_OP_GEP && gep->num_operands >= 3 && gep->aux_type && gep->aux_type->kind == ANVIL_TYPE_ARRAY &&
                                       anvil_opt_is_zero(gep->operands[1])
                                   ? 2
                                   : 1;
                anvil_type_t *element = level == 2 ? gep->aux_type->data.array.elem : gep->aux_type;
                if (gep->op != ANVIL_OP_GEP || gep->num_operands <= level || !element || !needs_multiply(element) ||
                    in_loop(cfg, loops, loop, gep->operands[0]) || gep->operands[0]->kind == ANVIL_VAL_CONST_INT ||
                    !exact_index(gep->operands[level], iv, pointer_bits, bounded, unsigned_bound))
                    continue;
                anvil_value_t *index = gep->operands[level];
                size_t stream = 0;
                while (stream < num_streams &&
                       !(streams[stream].base == gep->operands[0] && streams[stream].element == element && streams[stream].index == index))
                    stream++;
                if (stream == num_streams) {
                    if (num_streams == LS_STREAM_LIMIT)
                        continue;
                    /* p0 = &base[start] (the original walk up to the IV,
                     * the start extended as the index is). */
                    anvil_value_t *first_index = (anvil_value_t *)start;
                    if (index != iv) {
                        anvil_instr_t *extension = anvil_instr_create(ctx, index->data.instr->op, index->type, "stream.start");
                        if (!extension || !anvil_instr_add_operand(extension, (anvil_value_t *)start))
                            return ANVIL_PASS_RUN_ERROR;
                        anvil_opt_insert_before(preheader->last, extension);
                        first_index = extension->result;
                    }
                    anvil_type_t *pointer_type = anvil_type_ptr(ctx, element);
                    anvil_instr_t *origin = pointer_type ? anvil_instr_create(ctx, ANVIL_OP_GEP, pointer_type, "stream.origin") : NULL;
                    anvil_instr_t *pointer = origin ? anvil_instr_create(ctx, ANVIL_OP_PHI, pointer_type, "stream") : NULL;
                    anvil_instr_t *advance = pointer ? anvil_instr_create(ctx, ANVIL_OP_GEP, pointer_type, "stream.next") : NULL;
                    anvil_value_t *one = advance ? anvil_opt_make_const_int(ctx, index->type, 1) : NULL;
                    if (!one || !anvil_instr_add_operand(origin, gep->operands[0]) || (level == 2 && !anvil_instr_add_operand(origin, gep->operands[1])) ||
                        !anvil_instr_add_operand(origin, first_index) || !anvil_instr_add_operand(advance, pointer->result) ||
                        !anvil_instr_add_operand(advance, one))
                        return ANVIL_PASS_RUN_ERROR;
                    origin->aux_type = gep->aux_type;
                    advance->aux_type = element;
                    anvil_opt_insert_before(preheader->last, origin);
                    anvil_opt_insert_before(header->first, pointer);
                    anvil_opt_insert_before(latch->last, advance);
                    /* Incoming edges once the PHI sits in its block. */
                    if (!anvil_phi_add_incoming(pointer->result, origin->result, preheader) ||
                        !anvil_phi_add_incoming(pointer->result, advance->result, latch))
                        return ANVIL_PASS_RUN_ERROR;
                    streams[num_streams++] = (ls_stream_t){.base = gep->operands[0], .element = element, .index = index, .phi = pointer};
                }
                /* gep T base, i, rest / gep [N x T] base, 0, i, rest are
                 * gep T p, 0, rest. */
                anvil_value_t *zero = anvil_opt_make_const_int(ctx, index->type, 0);
                if (!zero)
                    return ANVIL_PASS_RUN_ERROR;
                gep->operands[0] = streams[stream].phi->result;
                gep->operands[1] = zero;
                if (level == 2) {
                    for (size_t operand = 2; operand + 1 < gep->num_operands; operand++)
                        gep->operands[operand] = gep->operands[operand + 1];
                    gep->num_operands--;
                }
                gep->aux_type = element;
                result = ANVIL_PASS_RUN_CHANGED;
            }
        }
    }
    return result;
}

anvil_pass_result_t anvil_pass_loop_strength(anvil_func_t *func)
{
    if (!func || !func->owner_ctx || !func->entry)
        return ANVIL_PASS_RUN_ERROR;
    anvil_opt_cfg_t cfg;
    anvil_loop_analysis_t loops;
    if (!anvil_opt_cfg_build(func, &cfg))
        return ANVIL_PASS_RUN_ERROR;
    if (!anvil_loop_analysis_build(&cfg, &loops)) {
        anvil_opt_cfg_destroy(&cfg);
        return ANVIL_PASS_RUN_ERROR;
    }
    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (size_t loop = 0; loop < loops.count && result != ANVIL_PASS_RUN_ERROR; loop++) {
        anvil_pass_result_t status = reduce_loop(func, &cfg, &loops, loop);
        if (status != ANVIL_PASS_RUN_UNCHANGED)
            result = status;
    }
    anvil_loop_analysis_destroy(&loops);
    anvil_opt_cfg_destroy(&cfg);
    if (result == ANVIL_PASS_RUN_CHANGED)
        anvil_func_invalidate_cfg(func);
    return result;
}
