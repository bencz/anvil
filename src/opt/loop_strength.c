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
 *
 * Last, an index read only to address invariant bases counts up to zero
 * (count_to_zero): the loop's test becomes a test of the increment's own
 * result against zero.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include "loop_utils.h"

#include <stdlib.h>

enum { LS_STREAM_LIMIT = 8 };

enum { LS_LATE_ROUNDS = 4 };

enum { LS_STREAM_GEPS = 8 };

/* An address as root + sum of variable indexes + constant, in elements of
 * one type (a chain of GEPs of that element type, as LICM and
 * count_to_zero leave base[i + c] and base[n]). */
enum { LS_FORM_VARS = 2 };

typedef struct {
    const anvil_value_t *root;
    const anvil_value_t *vars[LS_FORM_VARS];
    size_t num_vars;
    int64_t constant;
} ls_form_t;

static void address_form(const anvil_value_t *address, const anvil_type_t *element, ls_form_t *form)
{
    form->num_vars = 0;
    form->constant = 0;
    for (unsigned depth = 0; depth < 8 && address->kind == ANVIL_VAL_INSTR && address->data.instr->op == ANVIL_OP_GEP; depth++) {
        const anvil_instr_t *gep = address->data.instr;
        if (gep->num_operands != 2 || !gep->aux_type || !anvil_types_equal(gep->aux_type, element))
            break;
        const anvil_value_t *index = gep->operands[1];
        if (index->kind == ANVIL_VAL_CONST_INT)
            form->constant += index->data.i;
        else if (form->num_vars < LS_FORM_VARS)
            form->vars[form->num_vars++] = index;
        else
            break;
        address = gep->operands[0];
    }
    form->root = address;
}

/* Whether two forms differ only in their constants. */
static bool same_family(const ls_form_t *a, const ls_form_t *b)
{
    if (a->root != b->root || a->num_vars != b->num_vars)
        return false;
    if (a->num_vars == 2)
        return (a->vars[0] == b->vars[0] && a->vars[1] == b->vars[1]) || (a->vars[0] == b->vars[1] && a->vars[1] == b->vars[0]);
    return a->num_vars == 0 || a->vars[0] == b->vars[0];
}

typedef struct {
    anvil_value_t *base;
    ls_form_t form;         /* of base */
    anvil_type_t *element;
    anvil_value_t *index;   /* the IV, or its extension */
    anvil_instr_t *phi;     /* the stream's pointer */
    anvil_instr_t *origin, *advance;
    anvil_instr_t *geps[LS_STREAM_GEPS];
    size_t num_geps;        /* SIZE_MAX: more than LS_STREAM_GEPS */
} ls_stream_t;

static bool in_loop(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, const anvil_value_t *value)
{
    if (value->kind != ANVIL_VAL_INSTR || !value->data.instr || !value->data.instr->parent)
        return false;
    size_t block = anvil_opt_cfg_index(cfg, value->data.instr->parent);
    return block != SIZE_MAX && anvil_loop_contains(loops, loop, block);
}

/* sizeof(T) no scaled index absorbs (only powers of two up to 8 may). */
static bool needs_multiply(anvil_type_t *element)
{
    size_t size = element ? element->size : 0;
    return size != 0 && size != 1 && size != 2 && size != 4 && size != 8;
}

/* The width of every access through `address` (loads and stores of it or
 * of a pointer cast of it, in its block), 0 when they differ or there is
 * none. */
static size_t access_bytes(const anvil_instr_t *address)
{
    size_t bytes = 0;
    const anvil_value_t *through[2] = {address->result, NULL};
    for (const anvil_instr_t *user = address->next; user; user = user->next) {
        for (size_t t = 0; t < 2 && through[t]; t++) {
            size_t width = 0;
            if (user->op == ANVIL_OP_LOAD && user->num_operands == 1 && user->operands[0] == through[t] && user->result)
                width = user->result->type->size;
            else if (user->op == ANVIL_OP_STORE && user->num_operands == 2 && user->operands[1] == through[t])
                width = user->operands[0]->type->size;
            else if (t == 0 && user->op == ANVIL_OP_BITCAST && user->num_operands == 1 && user->operands[0] == through[0])
                through[1] = user->result;
            if (width && bytes && width != bytes)
                return 0;
            if (width)
                bytes = width;
        }
    }
    return bytes;
}

/* Whether a pointer stream beats indexing `element`s through `address`
 * on this target: always when no scaled index absorbs sizeof(T) (a
 * multiply otherwise); else only where the index does not fold into the
 * access and the target's accesses of that width can advance their own
 * base (the stream's increment then costs nothing), and only `late`: once
 * the optimizations that want base[i] (the loop vectorizer) are done. */
static bool stream_pays(const anvil_arch_info_t *arch, anvil_type_t *element, const anvil_instr_t *address, bool late)
{
    if (needs_multiply(element))
        return true;
    if (!late || !arch || !element)
        return false;
    unsigned log2 = element->size == 1 ? 0 : element->size == 2 ? 1 : element->size == 4 ? 2 : 3;
    size_t bytes = access_bytes(address);
    bool folds = ((arch->index_scales >> log2) & 1) || (arch->index_scale_access && bytes == element->size);
    return !folds && arch->address_increments != 0 && bytes && bytes <= arch->address_increment_bytes;
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

/* Replaces the narrow induction variable `phi` (start, step i + c or
 * i - c, c `amount`) by one of pointer width; see the header.  `test` (the
 * header's i < n proving no wrap) has its operands widened too; without
 * one the step is no-wrap and the test reads the narrow view. */
static anvil_pass_result_t widen_iv(anvil_func_t *func, anvil_instr_t *phi, anvil_value_t *start, anvil_instr_t *step, anvil_value_t *amount,
                                    anvil_instr_t *test, anvil_block_t *header, anvil_block_t *preheader)
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
    anvil_value_t *bound_wide = test ? widen_value(ctx, preheader, extend, wide, test->operands[1], "iv.bound") : start_wide;
    anvil_instr_t *widened = anvil_instr_create(ctx, ANVIL_OP_PHI, wide, "iv.wide");
    anvil_instr_t *next = anvil_instr_create(ctx, step->op, wide, "iv.next");
    anvil_instr_t *narrow = anvil_instr_create(ctx, ANVIL_OP_TRUNC, iv->type, "iv.narrow");
    anvil_value_t *delta = widen_value(ctx, preheader, extend, wide, amount, "iv.step");
    if (!start_wide || !bound_wide || !widened || !next || !narrow || !delta || !anvil_instr_add_operand(next, widened->result) ||
        !anvil_instr_add_operand(next, delta) || !anvil_instr_add_operand(narrow, widened->result))
        return ANVIL_PASS_RUN_ERROR;
    /* Exact in the wide type as it was in the narrow one; I is always i
     * extended, so its narrow views are exact truncations. */
    next->no_wrap = step->no_wrap;
    narrow->no_wrap = true;
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
    step->no_wrap = true;
    step->operands[0] = next->result;
    step->num_operands = 1;
    if (!latch || !anvil_phi_add_incoming(widened->result, start_wide, preheader) || !anvil_phi_add_incoming(widened->result, next->result, latch))
        return ANVIL_PASS_RUN_ERROR;
    if (test) {
        test->operands[0] = widened->result;
        test->operands[1] = bound_wide;
    }

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

/* Where a stream's increment goes, when every access through it sits in
 * the latch: on a target whose accesses can advance their base after the
 * access (post-increment) right after the last access; on one that only
 * advances first (pre-increment) right before the first, which with the
 * others then reads the advanced pointer (the stream starts one step
 * early).
 * Each pair then makes one access instruction.  Otherwise the increment
 * stays at the latch's end. */
static bool place_advance(anvil_ctx_t *ctx, const anvil_arch_info_t *arch, ls_stream_t *stream, anvil_block_t *latch, anvil_block_t *preheader,
                          anvil_value_t *amount)
{
    if (!arch || !arch->address_increments || stream->num_geps == 0 || stream->num_geps == SIZE_MAX)
        return true;
    for (size_t g = 0; g < stream->num_geps; g++)
        if (stream->geps[g]->parent != latch)
            return true;
    /* The first and the last access through any of its addresses. */
    anvil_instr_t *first = NULL, *last = NULL;
    for (anvil_instr_t *instr = latch->first; instr; instr = instr->next) {
        for (size_t g = 0; g < stream->num_geps; g++) {
            for (size_t o = 0; o < instr->num_operands; o++) {
                const anvil_value_t *operand = instr->operands[o];
                bool through = operand == stream->geps[g]->result ||
                               (operand->kind == ANVIL_VAL_INSTR && operand->data.instr->op == ANVIL_OP_BITCAST &&
                                operand->data.instr->operands[0] == stream->geps[g]->result);
                if (through && (instr->op == ANVIL_OP_LOAD || instr->op == ANVIL_OP_STORE)) {
                    last = instr;
                    if (!first)
                        first = instr;
                }
            }
        }
    }
    if (!first || !last)
        return true;
    anvil_opt_erase_instr(stream->advance);
    /* Post-increment where the target has it (the last access advances,
     * however many read the stream); pre-increment otherwise. */
    if (!(arch->address_increments & ANVIL_ADDRESS_POST_INCREMENT)) {
        /* p starts one step before base[start] (a signed index: the
         * IV's type may be unsigned); the accesses read p + step. */
        anvil_type_t *signed_index = arch->ptr_size == 8 ? anvil_type_i64(ctx) : anvil_type_i32(ctx);
        anvil_instr_t *earlier = anvil_instr_create(ctx, ANVIL_OP_GEP, stream->origin->result->type, "stream.before");
        anvil_value_t *back = anvil_opt_make_const_int(ctx, signed_index, -amount->data.i);
        if (!earlier || !back || !anvil_instr_add_operand(earlier, stream->origin->result) || !anvil_instr_add_operand(earlier, back))
            return false;
        earlier->aux_type = stream->element;
        anvil_opt_insert_before(preheader->last, earlier);
        for (size_t item = 0; item < stream->phi->num_phi_incoming; item++)
            if (stream->phi->operands[item] == stream->origin->result)
                stream->phi->operands[item] = earlier->result;
        /* advance; the addresses (and pointer casts of them) the first
         * access needs, moved down to it; the access: one update-form
         * instruction. */
        anvil_opt_insert_before(first, stream->advance);
        for (size_t g = 0; g < stream->num_geps; g++) {
            anvil_instr_t *gep = stream->geps[g];
            gep->operands[0] = stream->advance->result;
            (void)preheader;
            bool before_first = false;
            for (anvil_instr_t *scan = gep; scan; scan = scan->next)
                before_first |= scan == first;
            if (!before_first)
                continue;
            anvil_instr_t *cast = gep->next && gep->next->op == ANVIL_OP_BITCAST && gep->next->num_operands == 1 &&
                                          gep->next->operands[0] == gep->result ? gep->next : NULL;
            anvil_opt_erase_instr(gep);
            anvil_opt_insert_before(first, gep);
            if (cast) {
                anvil_opt_erase_instr(cast);
                anvil_opt_insert_before(first, cast);
            }
        }
        return true;
    }
    if (last->next)
        anvil_opt_insert_before(last->next, stream->advance);
    else
        anvil_opt_insert_before(latch->last, stream->advance);
    return true;
}

enum { LS_ZERO_BASES = 4 };

static anvil_instr_t *ls_binary(anvil_ctx_t *ctx, anvil_op_t op, anvil_type_t *type, anvil_value_t *a, anvil_value_t *b, const char *name)
{
    anvil_instr_t *instr = anvil_instr_create(ctx, op, type, name);
    return instr && anvil_instr_add_operand(instr, a) && anvil_instr_add_operand(instr, b) ? instr : NULL;
}

/* A counted loop whose index i (start s, step c, test i < n or i u< n with
 * c = 1, or i != n) is read only to index invariant bases (`gep T b, i`),
 * if at all,
 * counts j = i - n up to zero instead: the bases become b + n before the
 * loop, the test j != 0, which the targets decide from the flags (or
 * counter) of the increment itself; n is no longer read in the loop.  The
 * entry keeps the original test: j starts at s < n ? s - n : 0.  j is zero
 * exactly when i reaches n (address arithmetic is modular, so b + n + j
 * is b + i). */
static anvil_pass_result_t count_to_zero(anvil_func_t *func, const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop,
                                         anvil_block_t *header, anvil_block_t *preheader, anvil_block_t *latch, anvil_instr_t *test,
                                         bool continues_on_true, size_t pointer_bits)
{
    if (!test || !continues_on_true || test->num_operands != 2 || test->parent != header ||
        (test->op != ANVIL_OP_CMP_LT && test->op != ANVIL_OP_CMP_ULT && test->op != ANVIL_OP_CMP_NE))
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_instr_t *phi = NULL;
    anvil_value_t *bound = NULL;
    for (size_t side = 0; side < 2 && !phi; side++) {
        anvil_value_t *candidate = test->operands[side];
        /* i < n only with i on the left. */
        if ((side == 1 && test->op != ANVIL_OP_CMP_NE) || candidate->kind != ANVIL_VAL_INSTR || candidate->data.instr->op != ANVIL_OP_PHI ||
            candidate->data.instr->parent != header || in_loop(cfg, loops, loop, test->operands[1 - side]))
            continue;
        phi = candidate->data.instr;
        bound = test->operands[1 - side];
    }
    if (!phi || phi->num_phi_incoming != 2 || !anvil_type_is_integer(phi->result->type) ||
        anvil_type_bit_width(phi->result->type) != pointer_bits || (bound->kind == ANVIL_VAL_CONST_INT && bound->data.i == 0))
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_value_t *iv = phi->result, *start = NULL, *next = NULL;
    for (size_t item = 0; item < 2; item++) {
        if (phi->phi_blocks[item] == preheader)
            start = phi->operands[item];
        else if (phi->phi_blocks[item] == latch)
            next = phi->operands[item];
    }
    anvil_instr_t *step = next && next->kind == ANVIL_VAL_INSTR ? next->data.instr : NULL;
    if (!start || !step || step->op != ANVIL_OP_ADD || step->num_operands != 2 || !in_loop(cfg, loops, loop, next))
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_value_t *amount = step->operands[0] == iv ? step->operands[1] : step->operands[1] == iv ? step->operands[0] : NULL;
    if (!amount || amount->kind != ANVIL_VAL_CONST_INT || amount->data.i <= 0 || (amount->data.i != 1 && test->op != ANVIL_OP_CMP_NE))
        return ANVIL_PASS_RUN_UNCHANGED;

    /* Every other reader: the step, the test (read only by the header's
     * branch), or `gep T b, i` with b invariant. */
    struct {
        anvil_value_t *base;
        anvil_type_t *element;
        anvil_instr_t *rebased;
    } bases[LS_ZERO_BASES];
    size_t num_bases = 0, geps = 0, outside = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            for (size_t operand = 0; operand < instr->num_operands; operand++) {
                anvil_value_t *value = instr->operands[operand];
                if (value == test->result && instr != header->last)
                    return ANVIL_PASS_RUN_UNCHANGED;
                if (value == next && instr != phi)
                    return ANVIL_PASS_RUN_UNCHANGED;
                if (value != iv || instr == step || instr == test)
                    continue;
                /* After the loop (left only through the header's test) the
                 * index is known before it: see below. */
                size_t where = anvil_opt_cfg_index(cfg, instr->parent);
                if (where != SIZE_MAX && !anvil_loop_contains(loops, loop, where) && loops->loops[loop].exit_edge_count == 1) {
                    outside++;
                    continue;
                }
                if (instr->op != ANVIL_OP_GEP || instr->num_operands != 2 || operand != 1 || !instr->aux_type ||
                    !in_loop(cfg, loops, loop, instr->result) || in_loop(cfg, loops, loop, instr->operands[0]))
                    return ANVIL_PASS_RUN_UNCHANGED;
                size_t found = 0;
                while (found < num_bases && !(bases[found].base == instr->operands[0] && anvil_types_equal(bases[found].element, instr->aux_type)))
                    found++;
                if (found == num_bases) {
                    if (num_bases == LS_ZERO_BASES)
                        return ANVIL_PASS_RUN_UNCHANGED;
                    bases[num_bases].base = instr->operands[0];
                    bases[num_bases].element = instr->aux_type;
                    bases[num_bases++].rebased = instr;  /* the type to copy */
                }
                geps++;
            }
        }
    }
    (void)geps;  /* none: a pure counter (pointer streams took the addresses) */

    anvil_ctx_t *ctx = func->owner_ctx;
    /* The counter is signed (it runs through negative values; an unsigned
     * index's distance is reinterpreted). */
    anvil_type_t *type = iv->type;
    anvil_type_t *counting = type->is_signed ? type : pointer_bits == 64 ? anvil_type_i64(ctx) : anvil_type_i32(ctx);
    anvil_value_t *zero = anvil_opt_make_const_int(ctx, counting, 0);
    anvil_instr_t *distance = zero ? ls_binary(ctx, ANVIL_OP_SUB, type, start, bound, "count.start") : NULL;
    if (!distance)
        return ANVIL_PASS_RUN_ERROR;
    anvil_opt_insert_before(preheader->last, distance);
    anvil_value_t *initial = distance->result;
    if (counting != type) {
        anvil_instr_t *signed_distance = anvil_instr_create(ctx, ANVIL_OP_BITCAST, counting, "count.start");
        if (!signed_distance || !anvil_instr_add_operand(signed_distance, distance->result))
            return ANVIL_PASS_RUN_ERROR;
        anvil_opt_insert_before(preheader->last, signed_distance);
        initial = signed_distance->result;
    }
    /* The index after the loop: n once i != n failed; for i < n, n when the
     * loop ran, s when it did not. */
    anvil_value_t *final = bound;
    /* 0 u< n fails only for n = 0, where 0 - n is the 0 chosen anyway (and
     * n the index after the loop). */
    bool from_zero = test->op == ANVIL_OP_CMP_ULT && anvil_opt_is_zero(start);
    if (test->op != ANVIL_OP_CMP_NE && !from_zero) {
        anvil_instr_t *enter = ls_binary(ctx, test->op, anvil_type_i1(ctx), start, bound, "count.enter");
        anvil_instr_t *chosen = enter ? anvil_instr_create(ctx, ANVIL_OP_SELECT, counting, "count.start") : NULL;
        if (!chosen || !anvil_instr_add_operand(chosen, enter->result) || !anvil_instr_add_operand(chosen, initial) ||
            !anvil_instr_add_operand(chosen, zero))
            return ANVIL_PASS_RUN_ERROR;
        anvil_opt_insert_before(preheader->last, enter);
        anvil_opt_insert_before(preheader->last, chosen);
        initial = chosen->result;
        if (outside) {
            anvil_instr_t *last = anvil_instr_create(ctx, ANVIL_OP_SELECT, type, "count.final");
            if (!last || !anvil_instr_add_operand(last, enter->result) || !anvil_instr_add_operand(last, bound) || !anvil_instr_add_operand(last, start))
                return ANVIL_PASS_RUN_ERROR;
            anvil_opt_insert_before(preheader->last, last);
            final = last->result;
        }
    }
    for (size_t item = 0; item < num_bases; item++) {
        anvil_instr_t *model = bases[item].rebased;
        anvil_instr_t *rebased = anvil_instr_create(ctx, ANVIL_OP_GEP, model->result->type, "count.base");
        if (!rebased || !anvil_instr_copy_attributes(rebased, model) || !anvil_instr_add_operand(rebased, bases[item].base) ||
            !anvil_instr_add_operand(rebased, bound))
            return ANVIL_PASS_RUN_ERROR;
        anvil_opt_insert_before(preheader->last, rebased);
        bases[item].rebased = rebased;
    }
    anvil_instr_t *counter = anvil_instr_create(ctx, ANVIL_OP_PHI, counting, "count");
    anvil_value_t *stride = anvil_opt_make_const_int(ctx, counting, amount->data.i);
    anvil_instr_t *advance = counter && stride ? ls_binary(ctx, ANVIL_OP_ADD, counting, counter->result, stride, "count.next") : NULL;
    if (!advance)
        return ANVIL_PASS_RUN_ERROR;
    anvil_opt_insert_before(header->first, counter);
    /* Last in the latch: the test that follows reads its flags. */
    anvil_opt_insert_before(latch->last, advance);
    if (!anvil_phi_add_incoming(counter->result, initial, preheader) || !anvil_phi_add_incoming(counter->result, advance->result, latch))
        return ANVIL_PASS_RUN_ERROR;

    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        size_t where = anvil_opt_cfg_index(cfg, block);
        if (where == SIZE_MAX || !anvil_loop_contains(loops, loop, where))
            continue;  /* after the loop: the index's final value, below */
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_GEP || instr->num_operands != 2 || instr->operands[1] != iv)
                continue;
            size_t found = 0;
            while (!(bases[found].base == instr->operands[0] && anvil_types_equal(bases[found].element, instr->aux_type)))
                found++;
            instr->operands[0] = bases[found].rebased->result;
            instr->operands[1] = counter->result;
        }
    }
    test->op = ANVIL_OP_CMP_NE;
    test->operands[0] = counter->result;
    test->operands[1] = zero;
    if (outside) {
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            size_t index = anvil_opt_cfg_index(cfg, block);
            if (index == SIZE_MAX || anvil_loop_contains(loops, loop, index))
                continue;
            for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
                for (size_t operand = 0; operand < instr->num_operands; operand++)
                    if (instr->operands[operand] == iv)
                        instr->operands[operand] = final;
        }
    }
    anvil_opt_erase_instr(phi);
    anvil_opt_erase_instr(step);
    return ANVIL_PASS_RUN_CHANGED;
}


/* ------------------------------------------------------------------------
 * Induction variables kept only for addressing.  In a one-block loop on a
 * target whose accesses advance their base, an IV read in the loop only by
 * its step and as the index of invariant bases (directly or through a cast
 * of it) lives on as the streams' pointers: after the loop it is
 * start + (p - p0) / sizeof(T) from one stream p, and the loop keeps no
 * counter of its own.
 * ------------------------------------------------------------------------ */

static bool is_cast(anvil_op_t op)
{
    return op == ANVIL_OP_TRUNC || op == ANVIL_OP_SEXT || op == ANVIL_OP_ZEXT || op == ANVIL_OP_BITCAST;
}

/* Whether `gep` indexes the elements of an invariant base by `index` in a
 * form a stream takes (see reduce_loop) with accesses the target advances. */
typedef struct {
    const anvil_value_t *iv;
    size_t pointer_bits;
    bool bounded, unsigned_bound, unit;
} ls_exactness_t;

static bool streamable_gep(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, const anvil_arch_info_t *arch,
                           const anvil_instr_t *gep, const anvil_value_t *index, const ls_exactness_t *exact)
{
    if (gep->op != ANVIL_OP_GEP || gep->num_operands < 2 || !gep->aux_type || in_loop(cfg, loops, loop, gep->operands[0]) ||
        gep->operands[0]->kind == ANVIL_VAL_CONST_INT)
        return false;
    size_t level = gep->num_operands >= 3 && gep->aux_type->kind == ANVIL_TYPE_ARRAY && anvil_opt_is_zero(gep->operands[1]) ? 2 : 1;
    if (gep->operands[level] != index)
        return false;
    for (size_t o = 0; o < gep->num_operands; o++)
        if (o != level && gep->operands[o] == index)
            return false;
    size_t bytes = access_bytes(gep);
    if (!bytes || bytes > arch->address_increment_bytes)
        return false;
    /* As reduce_loop takes it: an index that cannot wrap in the loop. */
    if (!exact_index(index, exact->iv, exact->pointer_bits, exact->bounded && exact->unit, exact->unsigned_bound))
        return false;
    return exact->unit || (index == exact->iv && anvil_type_bit_width(exact->iv->type) == exact->pointer_bits);
}

/* Whether the only readers of `iv` (the PHI of a loop of `header` and
 * `latch` only) are its step (itself read only by the PHI), indexes of
 * streamable GEPs, casts of it read only so or after the loop, `test` (if
 * any) and instructions after the loop other than PHIs. */
static bool iv_only_addresses(anvil_func_t *func, const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop,
                              const anvil_arch_info_t *arch, const anvil_instr_t *phi, const anvil_instr_t *step, const ls_exactness_t *exact,
                              const anvil_block_t *latch, const anvil_instr_t *test)
{
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            for (size_t o = 0; o < instr->num_operands; o++) {
                const anvil_value_t *operand = instr->operands[o];
                if (operand == step->result && instr != phi && !(test && instr == test))
                    return false;
                if (operand != phi->result || instr == step || (test && instr == test))
                    continue;
                bool inside = block == phi->parent || block == latch;
                if (!inside) {
                    if (instr->op == ANVIL_OP_PHI)
                        return false;
                    continue;
                }
                if (streamable_gep(cfg, loops, loop, arch, instr, phi->result, exact))
                    continue;
                if (!is_cast(instr->op) || instr->num_operands != 1)
                    return false;
                /* The cast's own readers: streamable GEPs, or after the
                 * loop. */
                for (anvil_block_t *b = func->blocks; b; b = b->next) {
                    for (anvil_instr_t *user = b->first; user; user = user->next) {
                        for (size_t u = 0; u < user->num_operands; u++) {
                            if (user->operands[u] != instr->result)
                                continue;
                            if (b == phi->parent || b == latch ? !streamable_gep(cfg, loops, loop, arch, user, instr->result, exact)
                                                               : user->op == ANVIL_OP_PHI)
                                return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

/* Whether a GEP in the loop (`phi`'s block and `latch`) still indexes by
 * the IV or a cast of it. */
static bool iv_indexes_left(const anvil_instr_t *phi, const anvil_instr_t *step, const anvil_block_t *latch)
{
    for (anvil_instr_t *instr = phi->parent->first; instr; instr = instr->next ? instr->next : (instr->parent != latch ? latch->first : NULL)) {
        if (instr->op != ANVIL_OP_GEP)
            continue;
        for (size_t o = 0; o < instr->num_operands; o++) {
            const anvil_value_t *operand = instr->operands[o];
            if (operand == phi->result || operand == step->result ||
                (operand->kind == ANVIL_VAL_INSTR && operand->data.instr && is_cast(operand->data.instr->op) &&
                 operand->data.instr->operands[0] == phi->result))
                return true;
        }
    }
    return false;
}

/* Whether the loop's blocks are `header` and `latch` only. */
static bool loop_of(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, const anvil_block_t *header, const anvil_block_t *latch)
{
    for (size_t member = 0; member < cfg->count; member++)
        if (anvil_loop_contains(loops, loop, member) && cfg->blocks[member] != header && cfg->blocks[member] != latch)
            return false;
    return true;
}

static bool has_readers(anvil_func_t *func, const anvil_value_t *value)
{
    for (anvil_block_t *block = func->blocks; block; block = block->next)
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
            for (size_t o = 0; o < instr->num_operands; o++)
                if (instr->operands[o] == value)
                    return true;
    return false;
}

/* `iv` rebuilt before `user` from stream `stream`: start + (p - p0) >> log2(sizeof(T)). */
static anvil_value_t *iv_from_stream(anvil_ctx_t *ctx, const anvil_arch_info_t *arch, const ls_stream_t *stream, anvil_block_t *preheader,
                                     anvil_value_t *start, anvil_type_t *iv_type, anvil_instr_t *user, anvil_value_t **first_address)
{
    anvil_type_t *intptr = arch->ptr_size == 8 ? anvil_type_i64(ctx) : anvil_type_i32(ctx);
    anvil_value_t *initial = NULL;
    for (size_t item = 0; item < stream->phi->num_phi_incoming; item++)
        if (stream->phi->phi_blocks[item] == preheader)
            initial = stream->phi->operands[item];
    if (!initial)
        return NULL;
    if (!*first_address) {
        anvil_instr_t *origin = anvil_instr_create(ctx, ANVIL_OP_PTRTOINT, intptr, "stream.base");
        if (!origin || !anvil_instr_add_operand(origin, initial))
            return NULL;
        anvil_opt_insert_before(preheader->last, origin);
        *first_address = origin->result;
    }
    anvil_instr_t *now = anvil_instr_create(ctx, ANVIL_OP_PTRTOINT, intptr, "stream.at");
    if (!now || !anvil_instr_add_operand(now, stream->phi->result))
        return NULL;
    anvil_opt_insert_before(user, now);
    anvil_instr_t *bytes = ls_binary(ctx, ANVIL_OP_SUB, intptr, now->result, *first_address, "stream.bytes");
    if (!bytes)
        return NULL;
    anvil_opt_insert_before(user, bytes);
    anvil_value_t *count = bytes->result;
    unsigned log2 = 0;
    while ((UINT64_C(1) << log2) < stream->element->size)
        log2++;
    if (log2) {
        anvil_value_t *shift = anvil_opt_make_const_int(ctx, intptr, log2);
        anvil_instr_t *elements = shift ? ls_binary(ctx, ANVIL_OP_SAR, intptr, count, shift, "stream.count") : NULL;
        if (!elements)
            return NULL;
        anvil_opt_insert_before(user, elements);
        count = elements->result;
    }
    if (!anvil_types_equal(iv_type, intptr)) {
        bool narrower = anvil_type_bit_width(iv_type) < anvil_type_bit_width(intptr);
        anvil_instr_t *fit = anvil_instr_create(ctx, narrower ? ANVIL_OP_TRUNC : iv_type->is_signed ? ANVIL_OP_SEXT : ANVIL_OP_ZEXT, iv_type, "stream.iv");
        if (!fit || !anvil_instr_add_operand(fit, count))
            return NULL;
        anvil_opt_insert_before(user, fit);
        count = fit->result;
    }
    if (anvil_opt_is_zero(start))
        return count;
    anvil_instr_t *sum = ls_binary(ctx, ANVIL_OP_ADD, iv_type, start, count, "stream.iv");
    if (!sum)
        return NULL;
    anvil_opt_insert_before(user, sum);
    return sum->result;
}

/* After the streams replaced every in-loop index: `phi`'s readers after
 * the loop (and those of its casts) read it rebuilt from a stream, and the
 * IV, its step and its casts go. */
static bool eliminate_iv(anvil_func_t *func, const anvil_arch_info_t *arch, const ls_stream_t *stream, anvil_block_t *preheader,
                         anvil_block_t *latch, anvil_instr_t *phi, anvil_instr_t *step, anvil_value_t *start)
{
    anvil_ctx_t *ctx = func->owner_ctx;
    anvil_value_t *first_address = NULL;
    anvil_block_t *body = phi->parent;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        if (block == body || block == latch)
            continue;
        for (anvil_instr_t *user = block->first; user; user = user->next) {
            for (size_t o = 0; o < user->num_operands; o++) {
                anvil_value_t *operand = user->operands[o];
                anvil_instr_t *cast = operand->kind == ANVIL_VAL_INSTR && operand->data.instr &&
                                              (operand->data.instr->parent == body || operand->data.instr->parent == latch) &&
                                              is_cast(operand->data.instr->op) && operand->data.instr->operands[0] == phi->result
                                          ? operand->data.instr
                                          : NULL;
                if (operand != phi->result && !cast)
                    continue;
                anvil_value_t *value = iv_from_stream(ctx, arch, stream, preheader, start, phi->result->type, user, &first_address);
                if (!value)
                    return false;
                if (cast) {
                    anvil_instr_t *again = anvil_instr_create(ctx, cast->op, cast->result->type, "stream.cast");
                    if (!again || !anvil_instr_add_operand(again, value))
                        return false;
                    anvil_opt_insert_before(user, again);
                    value = again->result;
                }
                user->operands[o] = value;
            }
        }
    }
    /* What is left in the loop reads only the IV: casts (now unread), the
     * step and the PHI. */
    anvil_block_t *blocks[] = {body, latch};
    for (size_t b = 0; b < (latch == body ? 1u : 2u); b++) {
        for (anvil_instr_t *instr = blocks[b]->first; instr;) {
            anvil_instr_t *next = instr->next;
            if (instr != step && is_cast(instr->op) && instr->num_operands == 1 && instr->operands[0] == phi->result && !has_readers(func, instr->result))
                anvil_opt_erase_instr(instr);
            instr = next;
        }
    }
    anvil_opt_erase_instr(step);
    anvil_opt_erase_instr(phi);
    return true;
}

/* A value within 2^34 of zero: a constant of at most 32 bits, an extension
 * of a value of at most 32 bits, or a no-wrap sum or difference of two
 * such (a short way down). */
static bool narrow_range(const anvil_value_t *value, unsigned depth)
{
    if (value->kind == ANVIL_VAL_CONST_INT)
        return value->data.i >= INT32_MIN && value->data.i <= (int64_t)UINT32_MAX;
    if (value->kind != ANVIL_VAL_INSTR || !value->data.instr)
        return false;
    const anvil_instr_t *instr = value->data.instr;
    if ((instr->op == ANVIL_OP_SEXT || instr->op == ANVIL_OP_ZEXT) && instr->num_operands == 1)
        return anvil_type_bit_width(instr->operands[0]->type) <= 32;
    if ((instr->op == ANVIL_OP_ADD || instr->op == ANVIL_OP_SUB) && instr->num_operands == 2 && instr->no_wrap && depth < 2)
        return narrow_range(instr->operands[0], depth + 1) && narrow_range(instr->operands[1], depth + 1);
    return false;
}

/* The preheader and latch values of a header PHI with two of them. */
static bool phi_values(const anvil_instr_t *phi, const anvil_block_t *preheader, const anvil_block_t *latch, anvil_value_t **start,
                       anvil_instr_t **step)
{
    *start = NULL;
    *step = NULL;
    if (phi->op != ANVIL_OP_PHI || phi->num_phi_incoming != 2)
        return false;
    for (size_t item = 0; item < 2; item++) {
        if (phi->phi_blocks[item] == preheader)
            *start = phi->operands[item];
        else if (phi->phi_blocks[item] == latch && phi->operands[item]->kind == ANVIL_VAL_INSTR)
            *step = phi->operands[item]->data.instr;
    }
    return *start && *step && (*step)->parent == latch;
}

/* The constant step of `phi` by `step` (i + c, c + i, i - c), 0 if none. */
static int64_t phi_step(const anvil_instr_t *phi, const anvil_instr_t *step)
{
    if (step->num_operands != 2)
        return 0;
    const anvil_value_t *a = step->operands[0], *b = step->operands[1];
    if (step->op == ANVIL_OP_ADD && a == phi->result && b->kind == ANVIL_VAL_CONST_INT)
        return b->data.i;
    if (step->op == ANVIL_OP_ADD && b == phi->result && a->kind == ANVIL_VAL_CONST_INT)
        return a->data.i;
    if (step->op == ANVIL_OP_SUB && a == phi->result && b->kind == ANVIL_VAL_CONST_INT && b->data.i != INT64_MIN)
        return -b->data.i;
    return 0;
}

/* converge_to_count's counter for 32-bit indexes: d from the starts
 * extended to 64 bits (exact for any of them), T = d > 0 ? (d + 1) >> 1 : 0
 * as 32 unsigned bits (at most 2^31), and k u< T. */
static anvil_pass_result_t converge_narrow(anvil_func_t *func, anvil_block_t *preheader, anvil_block_t *header, anvil_block_t *latch,
                                           anvil_instr_t *test, anvil_value_t *const *starts)
{
    anvil_ctx_t *ctx = func->owner_ctx;
    anvil_type_t *i64 = anvil_type_i64(ctx), *u32 = anvil_type_u32(ctx);
    anvil_value_t *wide[2];
    for (size_t side = 0; side < 2; side++) {
        anvil_instr_t *extend = anvil_instr_create(ctx, starts[side]->type->is_signed ? ANVIL_OP_SEXT : ANVIL_OP_ZEXT, i64, "meet.start");
        if (!extend || !anvil_instr_add_operand(extend, starts[side]))
            return ANVIL_PASS_RUN_ERROR;
        anvil_opt_insert_before(preheader->last, extend);
        wide[side] = extend->result;
    }
    anvil_value_t *zero = anvil_opt_make_const_int(ctx, i64, 0), *one = anvil_opt_make_const_int(ctx, i64, 1);
    anvil_value_t *none = anvil_opt_make_const_int(ctx, u32, 0), *step = anvil_opt_make_const_int(ctx, u32, 1);
    anvil_instr_t *distance = zero && one && none && step ? ls_binary(ctx, ANVIL_OP_SUB, i64, wide[1], wide[0], "meet.distance") : NULL;
    anvil_instr_t *rounded = distance ? ls_binary(ctx, ANVIL_OP_ADD, i64, distance->result, one, "meet.rounded") : NULL;
    anvil_instr_t *half = rounded ? ls_binary(ctx, ANVIL_OP_SAR, i64, rounded->result, one, "meet.half") : NULL;
    anvil_instr_t *narrow = half ? anvil_instr_create(ctx, ANVIL_OP_TRUNC, u32, "meet.half") : NULL;
    anvil_instr_t *apart = narrow ? ls_binary(ctx, ANVIL_OP_CMP_LT, anvil_type_i1(ctx), zero, distance->result, "meet.apart") : NULL;
    anvil_instr_t *trips = apart ? anvil_instr_create(ctx, ANVIL_OP_SELECT, u32, "meet.trips") : NULL;
    if (!trips || !anvil_instr_add_operand(narrow, half->result) || !anvil_instr_add_operand(trips, apart->result) ||
        !anvil_instr_add_operand(trips, narrow->result) || !anvil_instr_add_operand(trips, none))
        return ANVIL_PASS_RUN_ERROR;
    distance->no_wrap = rounded->no_wrap = true;
    anvil_instr_t *made[] = {distance, rounded, half, narrow, apart, trips};
    for (size_t m = 0; m < sizeof(made) / sizeof(made[0]); m++)
        anvil_opt_insert_before(preheader->last, made[m]);
    anvil_instr_t *counter = anvil_instr_create(ctx, ANVIL_OP_PHI, u32, "meet");
    anvil_instr_t *advance = counter ? ls_binary(ctx, ANVIL_OP_ADD, u32, counter->result, step, "meet.next") : NULL;
    if (!advance)
        return ANVIL_PASS_RUN_ERROR;
    advance->no_wrap = true;
    anvil_opt_insert_before(header->first, counter);
    anvil_opt_insert_before(latch->last, advance);
    if (!anvil_phi_add_incoming(counter->result, none, preheader) || !anvil_phi_add_incoming(counter->result, advance->result, latch))
        return ANVIL_PASS_RUN_ERROR;
    test->op = ANVIL_OP_CMP_ULT;
    test->operands[0] = counter->result;
    test->operands[1] = trips->result;
    return ANVIL_PASS_RUN_CHANGED;
}

/* Two indexes meeting: the header tests i < j (or i u< j) with i stepping
 * by 1 and j by -1, both read in the loop only as addresses (and by their
 * steps and the test).  On a target with counter branches the test becomes
 * k < T for a new counter k from 0, T = (d + 1) >> 1 with d = j0 - i0
 * before the loop (iteration k has i = i0 + k, j = j0 - k, and i < j
 * exactly while 2k < d, so while k < T; with d <= 0 no k is).  Exact when d cannot wrap: pointer-wide
 * indexes starting from values of at most 32 bits (narrow_range), or 32-bit
 * ones (converge_narrow).  The
 * indexes then become streams and k a pure counter. */
static anvil_pass_result_t converge_to_count(anvil_func_t *func, const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop,
                                             const anvil_arch_info_t *arch, anvil_block_t *header, anvil_block_t *preheader,
                                             anvil_block_t *latch, anvil_instr_t *test, bool continues_on_true, size_t pointer_bits)
{
    if (!test || !continues_on_true || test->parent != header || test->num_operands != 2 ||
        (test->op != ANVIL_OP_CMP_LT && test->op != ANVIL_OP_CMP_ULT) || !loop_of(cfg, loops, loop, header, latch))
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_instr_t *phis[2] = {NULL, NULL}, *steps[2] = {NULL, NULL};
    anvil_value_t *starts[2] = {NULL, NULL};
    for (size_t side = 0; side < 2; side++) {
        const anvil_value_t *operand = test->operands[side];
        if (operand->kind != ANVIL_VAL_INSTR || operand->data.instr->parent != header || !anvil_type_is_integer(operand->type) ||
            anvil_type_bit_width(operand->type) != pointer_bits || !phi_values(operand->data.instr, preheader, latch, &starts[side], &steps[side]) ||
            (pointer_bits == 64 && !narrow_range(starts[side], 0)) || phi_step(operand->data.instr, steps[side]) != (side == 0 ? 1 : -1))
            return ANVIL_PASS_RUN_UNCHANGED;
        phis[side] = operand->data.instr;
    }
    if (phis[0] == phis[1])
        return ANVIL_PASS_RUN_UNCHANGED;
    /* Worth it only when both indexes become streams. */
    for (size_t side = 0; side < 2; side++) {
        ls_exactness_t exactness = {.iv = phis[side]->result, .pointer_bits = pointer_bits, .bounded = false, .unsigned_bound = false, .unit = side == 0};
        if (!iv_only_addresses(func, cfg, loops, loop, arch, phis[side], steps[side], &exactness, latch, test))
            return ANVIL_PASS_RUN_UNCHANGED;
    }

    anvil_ctx_t *ctx = func->owner_ctx;
    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_value_t *values[2];
    if (pointer_bits == 32)
        return converge_narrow(func, preheader, header, latch, test, starts);
    for (size_t side = 0; side < 2; side++) {
        values[side] = starts[side];
        if (!starts[side]->type->is_signed) {
            anvil_instr_t *as_signed = anvil_instr_create(ctx, ANVIL_OP_BITCAST, i64, "meet.start");
            if (!as_signed || !anvil_instr_add_operand(as_signed, starts[side]))
                return ANVIL_PASS_RUN_ERROR;
            anvil_opt_insert_before(preheader->last, as_signed);
            values[side] = as_signed->result;
        }
    }
    anvil_value_t *zero = anvil_opt_make_const_int(ctx, i64, 0), *one = anvil_opt_make_const_int(ctx, i64, 1);
    /* d = j0 - i0 (j0 itself when i0 is 0). */
    bool from_zero = anvil_opt_is_zero(values[0]);
    anvil_instr_t *distance = zero && one && !from_zero ? ls_binary(ctx, ANVIL_OP_SUB, i64, values[1], values[0], "meet.distance") : NULL;
    anvil_value_t *d = from_zero ? values[1] : distance ? distance->result : NULL;
    anvil_instr_t *rounded = d && one ? ls_binary(ctx, ANVIL_OP_ADD, i64, d, one, "meet.rounded") : NULL;
    /* (d + 1) >> 1 is <= 0 exactly when d <= 0: the signed k < T then
     * fails at once, as i0 < j0 did. */
    anvil_instr_t *trips = rounded ? ls_binary(ctx, ANVIL_OP_SAR, i64, rounded->result, one, "meet.trips") : NULL;
    if (!trips)
        return ANVIL_PASS_RUN_ERROR;
    rounded->no_wrap = true;
    if (distance) {
        distance->no_wrap = true;
        anvil_opt_insert_before(preheader->last, distance);
    }
    anvil_opt_insert_before(preheader->last, rounded);
    anvil_opt_insert_before(preheader->last, trips);
    anvil_instr_t *counter = anvil_instr_create(ctx, ANVIL_OP_PHI, i64, "meet");
    anvil_instr_t *advance = counter ? ls_binary(ctx, ANVIL_OP_ADD, i64, counter->result, one, "meet.next") : NULL;
    if (!advance)
        return ANVIL_PASS_RUN_ERROR;
    advance->no_wrap = true;
    anvil_opt_insert_before(header->first, counter);
    anvil_opt_insert_before(latch->last, advance);
    if (!anvil_phi_add_incoming(counter->result, zero, preheader) || !anvil_phi_add_incoming(counter->result, advance->result, latch))
        return ANVIL_PASS_RUN_ERROR;
    test->op = ANVIL_OP_CMP_LT;
    test->operands[0] = counter->result;
    test->operands[1] = trips->result;
    return ANVIL_PASS_RUN_CHANGED;
}

/* Whether `user` reads `value` only as the address of its access. */
static bool addresses_with(const anvil_instr_t *user, const anvil_value_t *value)
{
    if (user->op == ANVIL_OP_LOAD)
        return user->num_operands == 1;
    if (user->op == ANVIL_OP_STORE)
        return user->num_operands == 2 && user->operands[0] != value;
    return false;
}

/* Whether every reader of `value` reads it as an access's address. */
static bool only_addressed(anvil_func_t *func, const anvil_value_t *value)
{
    for (anvil_block_t *block = func->blocks; block; block = block->next)
        for (const anvil_instr_t *user = block->first; user; user = user->next)
            for (size_t o = 0; o < user->num_operands; o++)
                if (user->operands[o] == value && !addresses_with(user, value))
                    return false;
    return true;
}

/* The byte step of `phi` advanced by `step` = gep T phi, c in the latch
 * (c * sizeof(T)), 0 if it is no such pointer. */
static int64_t pointer_step(const anvil_instr_t *phi, const anvil_instr_t *step)
{
    if (step->op != ANVIL_OP_GEP || step->num_operands != 2 || step->operands[0] != phi->result || !step->aux_type ||
        step->operands[1]->kind != ANVIL_VAL_CONST_INT || step->aux_type->size == 0 || step->aux_type->size > INT32_MAX)
        return 0;
    int64_t count = step->operands[1]->data.i;
    return count > -(INT64_C(1) << 32) && count < (INT64_C(1) << 32) ? count * (int64_t)step->aux_type->size : 0;
}

/* Pointers advancing in lockstep (p += c, q += c, ... by the same bytes,
 * read in the loop only as addresses, directly or through a constant
 * displacement where the target folds one) cost one add each.  Where an
 * index folds into the access and no access advances its own base, they
 * become p0 + i, q0 + i over one byte index i: one add for all of them,
 * the addresses folding base + index. */
static anvil_pass_result_t share_lockstep_index(anvil_func_t *func, const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop,
                                                const anvil_arch_info_t *arch, anvil_block_t *header, anvil_block_t *preheader,
                                                anvil_block_t *latch)
{
    enum { LS_LOCKSTEP = 8 };
    if (!arch || arch->address_increments || !(arch->index_scales & 1))
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_instr_t *phis[LS_LOCKSTEP], *steps[LS_LOCKSTEP];
    anvil_value_t *starts[LS_LOCKSTEP];
    size_t count = 0;
    int64_t bytes = 0;
    for (anvil_instr_t *phi = header->first; phi && phi->op == ANVIL_OP_PHI && count < LS_LOCKSTEP; phi = phi->next) {
        anvil_value_t *start;
        anvil_instr_t *step;
        if (!phi->result || phi->result->type->kind != ANVIL_TYPE_PTR || !phi_values(phi, preheader, latch, &start, &step))
            continue;
        int64_t advance = pointer_step(phi, step);
        if (!advance || (bytes && advance != bytes))
            continue;
        /* The step feeds the PHI alone; in the loop the pointer only
         * addresses (a constant displacement too, where it folds). */
        bool only_phi = true, only_addresses = true;
        for (anvil_block_t *block = func->blocks; block && (only_phi || only_addresses); block = block->next) {
            size_t index = anvil_opt_cfg_index(cfg, block);
            bool inside = index != SIZE_MAX && anvil_loop_contains(loops, loop, index);
            for (const anvil_instr_t *user = block->first; user; user = user->next) {
                for (size_t o = 0; o < user->num_operands; o++) {
                    if (user->operands[o] == step->result && user != phi)
                        only_phi = false;
                    if (user->operands[o] != phi->result || !inside || user == step)
                        continue;
                    if (addresses_with(user, phi->result))
                        continue;
                    bool displaced = arch->index_displacement && user->op == ANVIL_OP_GEP && user->num_operands == 2 && o == 0 &&
                                     user->operands[1]->kind == ANVIL_VAL_CONST_INT && only_addressed(func, user->result);
                    if (!displaced)
                        only_addresses = false;
                }
            }
        }
        if (!only_phi || !only_addresses)
            continue;
        bytes = advance;
        phis[count] = phi;
        steps[count] = step;
        starts[count++] = start;
    }
    if (count < 2)
        return ANVIL_PASS_RUN_UNCHANGED;

    anvil_ctx_t *ctx = func->owner_ctx;
    anvil_type_t *index_type = arch->ptr_size == 8 ? anvil_type_i64(ctx) : anvil_type_i32(ctx), *byte = anvil_type_i8(ctx);
    anvil_value_t *zero = anvil_opt_make_const_int(ctx, index_type, 0), *amount = anvil_opt_make_const_int(ctx, index_type, bytes);
    anvil_instr_t *index = zero && amount ? anvil_instr_create(ctx, ANVIL_OP_PHI, index_type, "lockstep") : NULL;
    anvil_instr_t *next = index ? ls_binary(ctx, ANVIL_OP_ADD, index_type, index->result, amount, "lockstep.next") : NULL;
    if (!next)
        return ANVIL_PASS_RUN_ERROR;
    anvil_opt_insert_before(header->first, index);
    anvil_opt_insert_before(latch->last, next);
    if (!anvil_phi_add_incoming(index->result, zero, preheader) || !anvil_phi_add_incoming(index->result, next->result, latch))
        return ANVIL_PASS_RUN_ERROR;
    anvil_instr_t *after_phis = header->first;
    while (after_phis && after_phis->op == ANVIL_OP_PHI)
        after_phis = after_phis->next;
    for (size_t k = 0; k < count; k++) {
        anvil_instr_t *address = anvil_instr_create(ctx, ANVIL_OP_GEP, phis[k]->result->type, "lockstep.address");
        if (!address || !anvil_instr_add_operand(address, starts[k]) || !anvil_instr_add_operand(address, index->result))
            return ANVIL_PASS_RUN_ERROR;
        address->aux_type = byte;
        anvil_opt_insert_before(after_phis, address);
        anvil_opt_erase_instr(steps[k]);
        anvil_opt_replace_uses_in_func(func, phis[k]->result, address->result);
        anvil_opt_erase_instr(phis[k]);
    }
    return ANVIL_PASS_RUN_CHANGED;
}

static anvil_pass_result_t reduce_loop(anvil_func_t *func, const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, bool late)
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
    /* The test decides the loop: true stays in, false leaves (a false
     * edge that stays in, as before a merged `&&`, proves nothing). */
    size_t false_index = header->last->false_block ? anvil_opt_cfg_index(cfg, header->last->false_block) : SIZE_MAX;
    bool continues_on_true = true_index != SIZE_MAX && anvil_loop_contains(loops, loop, true_index) && false_index != SIZE_MAX &&
                             !anvil_loop_contains(loops, loop, false_index);

    anvil_ctx_t *ctx = func->owner_ctx;
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
    size_t pointer_bits = arch ? arch->ptr_size * 8 : 64;
    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    if (late && arch && arch->counter_branches && arch->address_increments && (pointer_bits == 64 || pointer_bits == 32)) {
        anvil_pass_result_t met = converge_to_count(func, cfg, loops, loop, arch, header, preheader, latch, test, continues_on_true, pointer_bits);
        if (met != ANVIL_PASS_RUN_UNCHANGED)
            return met;  /* the header's PHIs changed: the next round */
    }

    if (late) {
        anvil_pass_result_t shared = share_lockstep_index(func, cfg, loops, loop, arch, header, preheader, latch);
        if (shared != ANVIL_PASS_RUN_UNCHANGED)
            return shared;  /* the header's PHIs changed: the next round */
    }

    /* Narrow induction variables first: bounded by the header's test with
     * step 1, or stepped by a constant without wrapping (a no-wrap step). */
    for (anvil_instr_t *phi = header->first; phi && phi->op == ANVIL_OP_PHI; phi = phi->next) {
        if (pointer_bits != 64 || phi->num_phi_incoming != 2 || !phi->result || !anvil_type_is_integer(phi->result->type) ||
            anvil_type_bit_width(phi->result->type) >= 64)
            continue;
        anvil_value_t *start = NULL, *next = NULL;
        for (size_t item = 0; item < 2; item++) {
            if (phi->phi_blocks[item] == preheader)
                start = phi->operands[item];
            else if (phi->phi_blocks[item] == latch)
                next = phi->operands[item];
        }
        anvil_instr_t *step = next && next->kind == ANVIL_VAL_INSTR ? next->data.instr : NULL;
        if (!start || !step || (step->op != ANVIL_OP_ADD && step->op != ANVIL_OP_SUB) || step->num_operands != 2)
            continue;
        if (step->op == ANVIL_OP_ADD && step->operands[1] == phi->result) {
            step->operands[1] = step->operands[0];
            step->operands[0] = phi->result;
        }
        anvil_value_t *amount = step->operands[0] == phi->result ? step->operands[1] : NULL;
        /* A constant step, or a loop-invariant one of a no-wrap step (its
         * extension computed once, before the loop). */
        if (!amount || (amount->kind != ANVIL_VAL_CONST_INT && (!step->no_wrap || in_loop(cfg, loops, loop, amount))))
            continue;
        bool bounded = step->op == ANVIL_OP_ADD && amount->kind == ANVIL_VAL_CONST_INT && anvil_opt_is_one(amount) && test && continues_on_true && test->num_operands == 2 &&
                       test->operands[0] == phi->result && !in_loop(cfg, loops, loop, test->operands[1]) &&
                       test->op == (phi->result->type->is_signed ? ANVIL_OP_CMP_LT : ANVIL_OP_CMP_ULT);
        if (!bounded && !step->no_wrap)
            continue;
        anvil_pass_result_t widened = widen_iv(func, phi, start, step, amount, bounded ? test : NULL, header, preheader);
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
        anvil_value_t *amount = !step || step->op != ANVIL_OP_ADD || step->num_operands != 2 ? NULL
                              : step->operands[0] == phi->result ? step->operands[1]
                              : step->operands[1] == phi->result ? step->operands[0] : NULL;
        /* i - c steps by -c. */
        if (step && step->op == ANVIL_OP_SUB && step->num_operands == 2 && step->operands[0] == phi->result &&
            step->operands[1]->kind == ANVIL_VAL_CONST_INT && step->operands[1]->data.i != INT64_MIN)
            amount = anvil_opt_make_const_int(ctx, phi->result->type, -step->operands[1]->data.i);
        /* Any constant step: a negative one walks the stream down. */
        if (!start || !amount || amount->kind != ANVIL_VAL_CONST_INT || amount->data.i == 0)
            continue;
        bool unit = amount->data.i == 1;
        const anvil_value_t *iv = phi->result;
        bool bounded = false, unsigned_bound = false;
        if (test && continues_on_true && test->num_operands == 2 && !in_loop(cfg, loops, loop, test->operands[1]) && test->operands[0] == iv &&
            (test->op == ANVIL_OP_CMP_LT || test->op == ANVIL_OP_CMP_ULT)) {
            bounded = true;
            unsigned_bound = test->op == ANVIL_OP_CMP_ULT;
        }
        /* An IV read only for addressing (and after the loop) goes. */
        anvil_instr_t *step_instr = next && next->kind == ANVIL_VAL_INSTR ? next->data.instr : NULL;
        ls_exactness_t exactness = {.iv = iv, .pointer_bits = pointer_bits, .bounded = bounded, .unsigned_bound = unsigned_bound, .unit = unit};
        bool one_block = late && arch && arch->address_increments && step_instr && step_instr->parent == latch && loop_of(cfg, loops, loop, header, latch);
        bool eliminate = one_block && iv_only_addresses(func, cfg, loops, loop, arch, phi, step_instr, &exactness, latch, NULL);
        /* Read also by the loop's test, i < n in the header or a count to
         * zero (i != 0 or i + 1 != 0, count_to_zero): with every index a
         * stream the IV is a pure counter, which a target with counter
         * branches decrements, tests and branches on in one instruction. */
        bool counts_up = one_block && test && continues_on_true && test->op == ANVIL_OP_CMP_NE && test->num_operands == 2 &&
                         (test->operands[0] == step_instr->result || (test->operands[0] == iv && test->parent == header)) &&
                         anvil_opt_is_zero(test->operands[1]);
        bool frees_counter = !eliminate && one_block && arch->counter_branches && unit && ((bounded && test->parent == header) || counts_up) &&
                             anvil_type_bit_width(iv->type) == pointer_bits &&
                             iv_only_addresses(func, cfg, loops, loop, arch, phi, step_instr, &exactness, latch, test);

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
                /* A step other than 1: the pointer-wide IV itself only. */
                if (gep->op != ANVIL_OP_GEP || gep->num_operands <= level || !element || !(eliminate || frees_counter || stream_pays(arch, element, gep, late)) ||
                    in_loop(cfg, loops, loop, gep->operands[0]) || gep->operands[0]->kind == ANVIL_VAL_CONST_INT ||
                    !exact_index(gep->operands[level], iv, pointer_bits, bounded && unit, unsigned_bound) ||
                    (!unit && (gep->operands[level] != iv || anvil_type_bit_width(iv->type) != pointer_bits)))
                    continue;
                anvil_value_t *index = gep->operands[level];
                /* A base a constant number of elements from a stream's base
                 * (base[i + c] once LICM or count_to_zero split it) is that
                 * stream at p + c: one pointer for a vector loop's body
                 * copies. */
                anvil_value_t *base = gep->operands[0];
                ls_form_t form;
                address_form(base, element, &form);
                int64_t offset = 0;
                size_t stream = 0;
                while (stream < num_streams &&
                       !(streams[stream].element == element && streams[stream].index == index &&
                         (streams[stream].base == base || (level == 1 && same_family(&streams[stream].form, &form)))))
                    stream++;
                if (stream < num_streams && streams[stream].base != base)
                    offset = form.constant - streams[stream].form.constant;
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
                    anvil_value_t *one = advance ? anvil_opt_make_const_int(ctx, index->type, amount->data.i) : NULL;
                    if (!one || !anvil_instr_add_operand(origin, base) || (level == 2 && !anvil_instr_add_operand(origin, gep->operands[1])) ||
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
                    streams[num_streams++] = (ls_stream_t){.base = base, .form = form, .element = element, .index = index, .phi = pointer,
                                                           .origin = origin, .advance = advance};
                }
                ls_stream_t *owner = &streams[stream];
                if (owner->num_geps < LS_STREAM_GEPS)
                    owner->geps[owner->num_geps++] = gep;
                else
                    owner->num_geps = SIZE_MAX;
                /* gep T base, i, rest / gep [N x T] base, 0, i, rest are
                 * gep T p, 0, rest (gep T p, c for base + c). */
                anvil_value_t *zero = anvil_opt_make_const_int(ctx, index->type, offset);
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
        for (size_t stream = 0; stream < num_streams; stream++)
            if (!place_advance(ctx, arch, &streams[stream], latch, preheader, amount))
                return ANVIL_PASS_RUN_ERROR;
        /* Every index became a stream (none was refused): the IV goes. */
        if (eliminate && num_streams && !iv_indexes_left(phi, step_instr, latch)) {
            if (!eliminate_iv(func, arch, &streams[0], preheader, latch, phi, step_instr, start))
                return ANVIL_PASS_RUN_ERROR;
            return ANVIL_PASS_RUN_CHANGED;  /* the header's PHIs changed */
        }
        /* The casts that fed the streams' indexes are unread now. */
        if (frees_counter) {
            anvil_block_t *blocks[] = {header, latch};
            for (size_t b = 0; b < (latch == header ? 1u : 2u); b++) {
                for (anvil_instr_t *instr = blocks[b]->first; instr;) {
                    anvil_instr_t *following = instr->next;
                    if (is_cast(instr->op) && instr->num_operands == 1 && instr->operands[0] == phi->result && !has_readers(func, instr->result))
                        anvil_opt_erase_instr(instr);
                    instr = following;
                }
            }
        }
    }
    anvil_pass_result_t counted = count_to_zero(func, cfg, loops, loop, header, preheader, latch, test, continues_on_true, pointer_bits);
    return counted == ANVIL_PASS_RUN_UNCHANGED ? result : counted;
}

/* After the pipeline (`late`, where no CFG simplification removes them
 * again) every loop entered from outside gets a preheader, the block the
 * rewrites below start their values in. */
static anvil_pass_result_t ensure_preheaders(anvil_func_t *func)
{
    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (bool created = true; created;) {
        created = false;
        anvil_opt_cfg_t cfg;
        anvil_loop_analysis_t loops;
        if (!anvil_opt_cfg_build(func, &cfg))
            return ANVIL_PASS_RUN_ERROR;
        if (!anvil_loop_analysis_build(&cfg, &loops)) {
            anvil_opt_cfg_destroy(&cfg);
            return ANVIL_PASS_RUN_ERROR;
        }
        for (size_t loop = 0; loop < loops.count && !created; loop++) {
            const anvil_loop_info_t *info = &loops.loops[loop];
            if (info->preheader != SIZE_MAX || info->latch_count != 1 || cfg.blocks[info->header] == func->entry)
                continue;
            /* Some predecessor of the header outside the loop. */
            bool entered = false;
            for (size_t at = cfg.predecessor_offsets[info->header]; at < cfg.predecessor_offsets[info->header + 1]; at++)
                entered |= !anvil_loop_contains(&loops, loop, cfg.predecessors[at]);
            if (!entered)
                continue;
            if (!anvil_opt_create_preheader(&cfg, &loops, loop)) {
                anvil_loop_analysis_destroy(&loops);
                anvil_opt_cfg_destroy(&cfg);
                return ANVIL_PASS_RUN_ERROR;
            }
            created = true;
            result = ANVIL_PASS_RUN_CHANGED;
        }
        anvil_loop_analysis_destroy(&loops);
        anvil_opt_cfg_destroy(&cfg);
        if (created)
            anvil_func_invalidate_cfg(func);
    }
    return result;
}

static anvil_pass_result_t loop_strength(anvil_func_t *func, bool late)
{
    if (!func || !func->owner_ctx || !func->entry)
        return ANVIL_PASS_RUN_ERROR;
    anvil_pass_result_t prepared = late ? ensure_preheaders(func) : ANVIL_PASS_RUN_UNCHANGED;
    if (prepared == ANVIL_PASS_RUN_ERROR)
        return prepared;
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
        anvil_pass_result_t status = reduce_loop(func, &cfg, &loops, loop, late);
        if (status != ANVIL_PASS_RUN_UNCHANGED)
            result = status;
    }
    anvil_loop_analysis_destroy(&loops);
    anvil_opt_cfg_destroy(&cfg);
    if (result == ANVIL_PASS_RUN_CHANGED)
        anvil_func_invalidate_cfg(func);
    return result == ANVIL_PASS_RUN_UNCHANGED ? prepared : result;
}

anvil_pass_result_t anvil_pass_loop_strength(anvil_func_t *func)
{
    return loop_strength(func, false);
}

/* After the optimization pipeline (anvil_opt_prepare_codegen): streams for
 * targets whose accesses advance their own base too, then the counters
 * they leave count to zero. */
anvil_pass_result_t anvil_opt_loop_strength_late(anvil_func_t *func)
{
    /* A loop rewritten once (its PHIs changed) gets another look: two
     * indexes meeting become a counter, then streams. */
    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (int round = 0; round < LS_LATE_ROUNDS; round++) {
        anvil_pass_result_t status = loop_strength(func, true);
        if (status == ANVIL_PASS_RUN_ERROR)
            return status;
        if (status != ANVIL_PASS_RUN_CHANGED)
            break;
        result = status;
    }
    return result;
}
