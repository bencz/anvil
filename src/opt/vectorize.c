/*
 * Superword-level parallelism (SLP).
 *
 * In one block, `lanes` stores of one element type to consecutive
 * addresses of one root (a seed) whose stored values form a tree the target
 * computes lane by lane become one vector tree and one vector store.  A
 * bundle (one scalar per lane) is
 *
 *   - a splat: the same value in every lane, available before the seed;
 *   - a vector load: loads of consecutive addresses of one root;
 *   - one operation on every lane, its operand bundles again bundles.
 *
 * The vector tree is computed at the seed's first store: loads moved there
 * and stores moved there must not pass an access that may overlap them, a
 * call or a volatile access.  Scalars read outside the tree stay (DCE
 * removes the rest), and a bundle already built for an earlier seed of the
 * block is reused: its vector holds those scalars lane by lane.  Legality
 * and costs come from vector_operation_cost; floating-point lanes need the
 * function's fp_vectorization.
 */
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

#include <stdlib.h>
#include <string.h>

enum {
    SLP_VECTOR_BYTES = 16,
    SLP_MAX_LANES = 16,
    SLP_BUNDLES = 32,   /* per seed */
    SLP_BUILT = 64,     /* reusable bundles per block */
    SLP_DEPTH = 8,
    SLP_WINDOW = 256,   /* instructions between a seed's stores */
};

typedef enum { SLP_SPLAT, SLP_LOAD, SLP_OP, SLP_BUILT_ALREADY } slp_kind_t;

typedef struct {
    slp_kind_t kind;
    anvil_value_t *scalars[SLP_MAX_LANES];
    size_t operands[2];
    size_t num_operands;
    anvil_value_t *vector;      /* SLP_BUILT_ALREADY: the earlier vector */
} slp_bundle;

typedef struct {
    anvil_value_t *scalars[SLP_MAX_LANES];
    size_t lanes;
    anvil_value_t *vector;
} slp_built;

typedef struct {
    anvil_func_t *func;
    anvil_ctx_t *ctx;
    anvil_block_t *block;
    const anvil_def_use_t *uses;
    anvil_instr_t **order;      /* the block's instructions, in order */
    size_t order_count;
    anvil_type_t *element, *vector;
    size_t lanes;
    anvil_instr_t *stores[SLP_MAX_LANES];   /* by lane */
    size_t anchor_at;           /* the first of them, by position */
    slp_bundle bundles[SLP_BUNDLES];
    size_t count;
    slp_built *built;           /* the block's earlier bundles */
    size_t *built_count;
} slp_tree;

static size_t position_of(const slp_tree *tree, const anvil_instr_t *instr)
{
    for (size_t at = 0; at < tree->order_count; at++)
        if (tree->order[at] == instr)
            return at;
    return SIZE_MAX;
}

/* An instruction of this block, or SIZE_MAX. */
static size_t block_position(const slp_tree *tree, const anvil_value_t *value)
{
    if (value->kind != ANVIL_VAL_INSTR || !value->data.instr || value->data.instr->parent != tree->block)
        return SIZE_MAX;
    return position_of(tree, value->data.instr);
}

/* Defined before the anchor: outside the block (it dominates the block's
 * users) or earlier in it. */
static bool available(const slp_tree *tree, const anvil_value_t *value)
{
    size_t at = block_position(tree, value);
    if (at != SIZE_MAX)
        return at < tree->anchor_at;
    return value->kind != ANVIL_VAL_INSTR || (value->data.instr && value->data.instr->parent != tree->block);
}

static size_t use_count(const slp_tree *tree, const anvil_value_t *value)
{
    size_t definition = anvil_def_use_definition(tree->uses, value);
    return definition == SIZE_MAX ? SIZE_MAX : tree->uses->use_offsets[definition + 1] - tree->uses->use_offsets[definition];
}

static bool writes_memory(const anvil_instr_t *instr)
{
    return instr->op == ANVIL_OP_STORE || instr->op == ANVIL_OP_CALL || anvil_op_is_atomic(instr->op) || instr->memory_access.is_volatile;
}

static bool reads_memory(const anvil_instr_t *instr)
{
    return instr->op == ANVIL_OP_LOAD || instr->op == ANVIL_OP_CALL || anvil_op_is_atomic(instr->op) || instr->memory_access.is_volatile;
}

/* `instr`, an access between two points the tree moves something across,
 * leaves `address` (size bytes) alone. */
static bool apart(const anvil_instr_t *instr, const anvil_value_t *address, size_t size)
{
    if (instr->op == ANVIL_OP_STORE && !instr->memory_access.is_volatile && instr->num_operands == 2)
        return anvil_memory_alias(instr->operands[1], instr->operands[0]->type->size, address, size) == ANVIL_ALIAS_NO;
    if (instr->op == ANVIL_OP_LOAD && !instr->memory_access.is_volatile && instr->num_operands == 1)
        return anvil_memory_alias(instr->operands[0], instr->result->type->size, address, size) == ANVIL_ALIAS_NO;
    if (instr->op == ANVIL_OP_CALL)
        return (anvil_opt_call_effects(instr) & (ANVIL_EFFECT_WRITE_MEMORY | ANVIL_EFFECT_READ_MEMORY | ANVIL_EFFECT_OBSERVABLE)) == 0;
    return false;
}

/* Whether the lanes' addresses are root + offset0 + k * size. */
static bool consecutive(const slp_tree *tree, anvil_value_t *const *addresses, const anvil_value_t **root, int64_t *offset)
{
    for (size_t lane = 0; lane < tree->lanes; lane++) {
        const anvil_value_t *base;
        int64_t at;
        if (!anvil_memory_offset(addresses[lane], &base, &at))
            return false;
        if (lane == 0) {
            *root = base;
            *offset = at;
        } else if (base != *root || at != *offset + (int64_t)(lane * tree->element->size)) {
            return false;
        }
    }
    return (*root)->type && (*root)->type->kind == ANVIL_TYPE_PTR;
}

static bool lane_operation(anvil_op_t op, bool floating)
{
    switch (op) {
    case ANVIL_OP_FADD:
    case ANVIL_OP_FSUB:
    case ANVIL_OP_FMUL:
    case ANVIL_OP_FDIV:
        return floating;
    case ANVIL_OP_ADD:
    case ANVIL_OP_SUB:
    case ANVIL_OP_MUL:
    case ANVIL_OP_AND:
    case ANVIL_OP_OR:
    case ANVIL_OP_XOR:
        return !floating;
    default:
        return false;
    }
}

/* Whether the function stores, other than through the seed, a narrower
 * value into the vector at `root` + `offset` (as seen through any root of
 * the same pointer type): a vector load right after such stores cannot
 * take their data forwarded and waits for them to reach the cache. */
static bool narrow_stores_into(const slp_tree *tree, const anvil_value_t *root, int64_t offset)
{
    for (anvil_block_t *block = tree->func->blocks; block; block = block->next) {
        for (const anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_STORE || instr->num_operands != 2 || instr->operands[0]->type->size >= SLP_VECTOR_BYTES)
                continue;
            bool seed = false;
            for (size_t lane = 0; lane < tree->lanes; lane++)
                seed |= instr == tree->stores[lane];
            const anvil_value_t *base;
            int64_t at;
            if (seed || !anvil_memory_offset(instr->operands[1], &base, &at) || !anvil_types_equal(base->type, root->type))
                continue;
            if (at + (int64_t)instr->operands[0]->type->size > offset && at < offset + SLP_VECTOR_BYTES)
                return true;
        }
    }
    return false;
}

static size_t find_bundle(const slp_tree *tree, anvil_value_t *const *scalars)
{
    for (size_t index = 0; index < tree->count; index++)
        if (!memcmp(tree->bundles[index].scalars, scalars, tree->lanes * sizeof(*scalars)))
            return index;
    return SIZE_MAX;
}

static anvil_value_t *built_vector(const slp_tree *tree, anvil_value_t *const *scalars)
{
    for (size_t index = 0; index < *tree->built_count; index++)
        if (tree->built[index].lanes == tree->lanes && !memcmp(tree->built[index].scalars, scalars, tree->lanes * sizeof(*scalars)))
            return tree->built[index].vector;
    return NULL;
}

/* The bundle of `scalars`, built bottom-up; SIZE_MAX when it cannot be. */
static size_t build_bundle(slp_tree *tree, anvil_value_t *const *scalars, unsigned depth)
{
    size_t known = find_bundle(tree, scalars);
    if (known != SIZE_MAX)
        return known;
    if (tree->count == SLP_BUNDLES || depth > SLP_DEPTH)
        return SIZE_MAX;
    for (size_t lane = 0; lane < tree->lanes; lane++)
        if (!anvil_types_equal(scalars[lane]->type, tree->element))
            return SIZE_MAX;
    slp_bundle bundle = {.kind = SLP_SPLAT};
    memcpy(bundle.scalars, scalars, tree->lanes * sizeof(*scalars));

    bool same = true;
    for (size_t lane = 1; lane < tree->lanes; lane++)
        same &= scalars[lane] == scalars[0];
    anvil_value_t *earlier = built_vector(tree, scalars);
    if (earlier && available(tree, earlier)) {
        bundle.kind = SLP_BUILT_ALREADY;
        bundle.vector = earlier;
    } else if (same) {
        if (!available(tree, scalars[0]) || scalars[0]->kind == ANVIL_VAL_GLOBAL)
            return SIZE_MAX;
        bundle.kind = SLP_SPLAT;
    } else {
        /* Instructions of this block, one operation. */
        anvil_instr_t *first = scalars[0]->kind == ANVIL_VAL_INSTR ? scalars[0]->data.instr : NULL;
        if (!first || first->parent != tree->block)
            return SIZE_MAX;
        for (size_t lane = 1; lane < tree->lanes; lane++) {
            const anvil_instr_t *instr = scalars[lane]->kind == ANVIL_VAL_INSTR ? scalars[lane]->data.instr : NULL;
            if (!instr || instr->parent != tree->block || instr->op != first->op || instr->num_operands != first->num_operands)
                return SIZE_MAX;
        }
        if (first->op == ANVIL_OP_LOAD) {
            anvil_value_t *addresses[SLP_MAX_LANES];
            for (size_t lane = 0; lane < tree->lanes; lane++) {
                const anvil_instr_t *load = scalars[lane]->data.instr;
                if (load->memory_access.is_volatile || load->num_operands != 1)
                    return SIZE_MAX;
                addresses[lane] = load->operands[0];
            }
            const anvil_value_t *root;
            int64_t offset;
            if (!consecutive(tree, addresses, &root, &offset) || !available(tree, root) || narrow_stores_into(tree, root, offset))
                return SIZE_MAX;
            /* Each load moves to the anchor: nothing between may write
             * its element (the anchor's own store included when the load
             * came after it). */
            for (size_t lane = 0; lane < tree->lanes; lane++) {
                size_t at = position_of(tree, scalars[lane]->data.instr);
                size_t low = at < tree->anchor_at ? at + 1 : tree->anchor_at, high = at < tree->anchor_at ? tree->anchor_at : at;
                for (size_t between = low; between < high; between++)
                    if (writes_memory(tree->order[between]) && !apart(tree->order[between], addresses[lane], tree->element->size))
                        return SIZE_MAX;
            }
            bundle.kind = SLP_LOAD;
        } else {
            bool floating = anvil_type_is_floating(tree->element);
            if (first->num_operands != 2 || !lane_operation(first->op, floating) || (floating && !tree->func->fp_vectorization))
                return SIZE_MAX;
            bundle.kind = SLP_OP;
            bundle.num_operands = 2;
            for (size_t operand = 0; operand < 2; operand++) {
                anvil_value_t *children[SLP_MAX_LANES];
                for (size_t lane = 0; lane < tree->lanes; lane++)
                    children[lane] = scalars[lane]->data.instr->operands[operand];
                size_t child = build_bundle(tree, children, depth + 1);
                if (child == SIZE_MAX)
                    return SIZE_MAX;
                bundle.operands[operand] = child;
            }
        }
    }
    if (tree->count == SLP_BUNDLES)
        return SIZE_MAX;
    tree->bundles[tree->count] = bundle;
    return tree->count++;
}

/* Whether a scalar of bundle `index` goes away once the vector tree
 * replaces it: read only by its parent in the tree, which goes too (the
 * stored values: only by their store). */
static bool lane_freed(const slp_tree *tree, size_t index, const bool *freed)
{
    (void)freed;
    for (size_t lane = 0; lane < tree->lanes; lane++)
        if (use_count(tree, tree->bundles[index].scalars[lane]) != 1)
            return false;
    return true;
}

/* Vector cost against the scalar instructions and stores it removes. */
static bool profitable(slp_tree *tree, size_t root)
{
    anvil_ctx_t *ctx = tree->ctx;
    unsigned store = anvil_vector_operation_cost(ctx, ANVIL_OP_STORE, tree->vector);
    unsigned load = anvil_vector_operation_cost(ctx, ANVIL_OP_LOAD, tree->vector);
    unsigned splat = anvil_vector_operation_cost(ctx, ANVIL_OP_SPLAT, tree->vector);
    if (!store)
        return false;
    /* Freed scalars, from the root down: a bundle's scalars die when each is
     * read once and its parent bundle dies. */
    bool freed[SLP_BUNDLES] = {0};
    freed[root] = lane_freed(tree, root, freed);
    unsigned vector_cost = store, scalar_cost = (unsigned)tree->lanes;
    size_t operations = 0;
    for (size_t index = tree->count; index-- > 0;) {
        const slp_bundle *bundle = &tree->bundles[index];
        if (bundle->kind == SLP_OP) {
            const anvil_instr_t *first = bundle->scalars[0]->data.instr;
            unsigned cost = anvil_vector_operation_cost(ctx, first->op, tree->vector);
            if (!cost)
                return false;
            vector_cost += cost;
            operations++;
            for (size_t operand = 0; operand < bundle->num_operands; operand++) {
                size_t child = bundle->operands[operand];
                if (freed[index] && tree->bundles[child].kind != SLP_SPLAT && tree->bundles[child].kind != SLP_BUILT_ALREADY)
                    freed[child] = lane_freed(tree, child, freed);
            }
        } else if (bundle->kind == SLP_LOAD) {
            if (!load)
                return false;
            vector_cost += load;
        } else if (bundle->kind == SLP_SPLAT) {
            if (!splat)
                return false;
            vector_cost += splat;
        }
        if (freed[index] && (bundle->kind == SLP_OP || bundle->kind == SLP_LOAD))
            scalar_cost += (unsigned)tree->lanes;
    }
    /* A tie still trades `lanes` stores for one. */
    return operations > 0 && vector_cost <= scalar_cost;
}

static anvil_instr_t *emit(slp_tree *tree, anvil_instr_t *before, anvil_op_t op, anvil_type_t *type, anvil_value_t *const *operands, size_t count)
{
    anvil_instr_t *instr = anvil_instr_create(tree->ctx, op, type, op == ANVIL_OP_STORE ? NULL : "slp");
    if (!instr || (count && !anvil_instr_add_operands(instr, operands, count)))
        return NULL;
    anvil_opt_insert_before(before, instr);
    return instr;
}

/* A pointer to the vector at `root` + `offset` (bytes), before `anchor`. */
static anvil_value_t *vector_address(slp_tree *tree, anvil_instr_t *anchor, anvil_value_t *lane0, const anvil_value_t *root, int64_t offset)
{
    anvil_type_t *pointer = anvil_type_ptr(tree->ctx, tree->vector);
    if (!pointer)
        return NULL;
    anvil_value_t *address = lane0;
    if (!available(tree, lane0)) {
        /* Rebuilt from the root, which is. */
        anvil_type_t *i8 = anvil_type_i8(tree->ctx), *bytes = anvil_type_ptr(tree->ctx, i8);
        anvil_value_t *base = (anvil_value_t *)root;
        anvil_instr_t *cast = bytes ? emit(tree, anchor, ANVIL_OP_BITCAST, bytes, &base, 1) : NULL;
        const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(tree->ctx);
        anvil_value_t *displacement = arch ? anvil_opt_make_const_int(tree->ctx, arch->ptr_size == 8 ? anvil_type_i64(tree->ctx) : anvil_type_i32(tree->ctx), offset) : NULL;
        anvil_value_t *gep_ops[] = {cast ? cast->result : NULL, displacement};
        anvil_instr_t *gep = cast && displacement ? emit(tree, anchor, ANVIL_OP_GEP, bytes, gep_ops, 2) : NULL;
        if (!gep)
            return NULL;
        gep->aux_type = i8;
        address = gep->result;
    }
    anvil_instr_t *cast = emit(tree, anchor, ANVIL_OP_BITCAST, pointer, &address, 1);
    return cast ? cast->result : NULL;
}

/* The bundles' vectors, children first, before the anchor; then the vector
 * store, and the seed's stores go.  Each allocation precedes its use, so a
 * failure leaves unused instructions behind, valid IR. */
static anvil_pass_result_t replace(slp_tree *tree, size_t root)
{
    anvil_instr_t *anchor = tree->order[tree->anchor_at];
    for (size_t index = 0; index < tree->count; index++) {
        slp_bundle *bundle = &tree->bundles[index];
        anvil_instr_t *made = NULL;
        switch (bundle->kind) {
        case SLP_BUILT_ALREADY:
            continue;
        case SLP_SPLAT:
            made = emit(tree, anchor, ANVIL_OP_SPLAT, tree->vector, &bundle->scalars[0], 1);
            break;
        case SLP_LOAD: {
            anvil_value_t *addresses[SLP_MAX_LANES];
            for (size_t lane = 0; lane < tree->lanes; lane++)
                addresses[lane] = bundle->scalars[lane]->data.instr->operands[0];
            const anvil_value_t *base;
            int64_t offset;
            anvil_value_t *address = consecutive(tree, addresses, &base, &offset) ? vector_address(tree, anchor, addresses[0], base, offset) : NULL;
            made = address ? emit(tree, anchor, ANVIL_OP_LOAD, tree->vector, &address, 1) : NULL;
            break;
        }
        case SLP_OP: {
            anvil_value_t *operands[] = {tree->bundles[bundle->operands[0]].vector, tree->bundles[bundle->operands[1]].vector};
            made = emit(tree, anchor, bundle->scalars[0]->data.instr->op, tree->vector, operands, 2);
            break;
        }
        }
        if (!made)
            return ANVIL_PASS_RUN_ERROR;
        bundle->vector = made->result;
    }
    anvil_value_t *addresses[SLP_MAX_LANES];
    for (size_t lane = 0; lane < tree->lanes; lane++)
        addresses[lane] = tree->stores[lane]->operands[1];
    const anvil_value_t *base;
    int64_t offset;
    anvil_value_t *address = consecutive(tree, addresses, &base, &offset) ? vector_address(tree, anchor, addresses[0], base, offset) : NULL;
    anvil_value_t *store_ops[] = {tree->bundles[root].vector, address};
    if (!address || !emit(tree, anchor, ANVIL_OP_STORE, NULL, store_ops, 2))
        return ANVIL_PASS_RUN_ERROR;
    /* Remember the bundles for the block's later seeds. */
    for (size_t index = 0; index < tree->count && *tree->built_count < SLP_BUILT; index++) {
        slp_built *entry = &tree->built[(*tree->built_count)++];
        memcpy(entry->scalars, tree->bundles[index].scalars, tree->lanes * sizeof(entry->scalars[0]));
        entry->lanes = tree->lanes;
        entry->vector = tree->bundles[index].vector;
    }
    for (size_t lane = 0; lane < tree->lanes; lane++)
        anvil_opt_erase_instr(tree->stores[lane]);
    return ANVIL_PASS_RUN_CHANGED;
}

/* The seed starting at the store at `start`: `lanes` stores of the element
 * to root + offset + k * size, k = 0..lanes-1; every store between the
 * first and a later one's position leaves those moved across it alone. */
static bool collect_seed(slp_tree *tree, size_t start)
{
    anvil_instr_t *first = tree->order[start];
    if (first->op != ANVIL_OP_STORE || first->num_operands != 2 || first->memory_access.is_volatile)
        return false;
    anvil_type_t *element = first->operands[0]->type;
    size_t size = element->size;
    if (!size || SLP_VECTOR_BYTES % size || SLP_VECTOR_BYTES / size < 2 || SLP_VECTOR_BYTES / size > SLP_MAX_LANES ||
        (!anvil_type_is_floating(element) && !anvil_type_is_integer(element)) || element->kind == ANVIL_TYPE_I1)
        return false;
    const anvil_value_t *root;
    int64_t offset;
    if (!anvil_memory_offset(first->operands[1], &root, &offset))
        return false;
    tree->element = element;
    tree->lanes = SLP_VECTOR_BYTES / size;
    memset(tree->stores, 0, sizeof(tree->stores));
    tree->stores[0] = first;
    size_t found = 1, earliest = start, last = start;
    /* The other lanes, before or after it within the window. */
    size_t low = start > SLP_WINDOW ? start - SLP_WINDOW : 0, high = start + SLP_WINDOW < tree->order_count ? start + SLP_WINDOW : tree->order_count;
    for (size_t at = low; at < high && found < tree->lanes; at++) {
        anvil_instr_t *store = tree->order[at];
        const anvil_value_t *base;
        int64_t where;
        if (at == start || store->op != ANVIL_OP_STORE || store->num_operands != 2 || store->memory_access.is_volatile ||
            !anvil_types_equal(store->operands[0]->type, element) || !anvil_memory_offset(store->operands[1], &base, &where) || base != root)
            continue;
        int64_t distance = where - offset;
        if (distance <= 0 || distance % (int64_t)size || distance / (int64_t)size >= (int64_t)tree->lanes)
            continue;
        size_t lane = (size_t)(distance / (int64_t)size);
        if (tree->stores[lane])
            return false;  /* two stores to one element */
        tree->stores[lane] = store;
        found++;
        earliest = at < earliest ? at : earliest;
        last = at > last ? at : last;
    }
    if (found != tree->lanes)
        return false;
    tree->anchor_at = earliest;
    (void)last;
    return true;
}

/* Each store moves up to the anchor: what it passes may not touch its
 * element.  The seed's other stores are apart from it, and the tree's
 * loads move to the anchor too, reading before the vector store as they
 * read before this one. */
static bool stores_movable(const slp_tree *tree)
{
    for (size_t lane = 0; lane < tree->lanes; lane++) {
        size_t at = position_of(tree, tree->stores[lane]);
        for (size_t between = tree->anchor_at + 1; between < at; between++) {
            const anvil_instr_t *instr = tree->order[between];
            bool moving = false;
            for (size_t other = 0; other < tree->lanes; other++)
                moving |= instr == tree->stores[other];
            for (size_t index = 0; index < tree->count && !moving; index++)
                if (tree->bundles[index].kind == SLP_LOAD)
                    for (size_t other = 0; other < tree->lanes; other++)
                        moving |= instr->result && tree->bundles[index].scalars[other] == instr->result;
            if (moving || (!reads_memory(instr) && !writes_memory(instr)))
                continue;
            if (!apart(instr, tree->stores[lane]->operands[1], tree->element->size))
                return false;
        }
    }
    return true;
}

static anvil_pass_result_t vectorize_block(anvil_func_t *func, anvil_block_t *block, const anvil_def_use_t *uses, slp_built *built,
                                           size_t *built_count, bool *changed)
{
    size_t count = 0;
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
        count++;
    if (count < 4)
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_instr_t **order = malloc(count * sizeof(*order));
    slp_tree *tree = calloc(1, sizeof(*tree));
    if (!order || !tree) {
        free(order);
        free(tree);
        return ANVIL_PASS_RUN_ERROR;
    }
    size_t at = 0;
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
        order[at++] = instr;

    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (size_t start = 0; start < count; start++) {
        if (!order[start] || order[start]->op != ANVIL_OP_STORE)
            continue;
        memset(tree, 0, sizeof(*tree));
        tree->func = func;
        tree->ctx = func->owner_ctx;
        tree->block = block;
        tree->uses = uses;
        tree->order = order;
        tree->order_count = count;
        tree->built = built;
        tree->built_count = built_count;
        if (!collect_seed(tree, start))
            continue;
        if (!(tree->vector = anvil_type_vector(tree->ctx, tree->element, tree->lanes))) {
            result = ANVIL_PASS_RUN_ERROR;
            break;
        }
        anvil_value_t *values[SLP_MAX_LANES];
        for (size_t lane = 0; lane < tree->lanes; lane++)
            values[lane] = tree->stores[lane]->operands[0];
        size_t root = build_bundle(tree, values, 0);
        if (root == SIZE_MAX || !stores_movable(tree) || !profitable(tree, root))
            continue;
        result = replace(tree, root);
        if (result == ANVIL_PASS_RUN_ERROR)
            break;
        *changed = true;
        /* The block changed: one seed per pass over it (the def-use
         * snapshot is stale now); the caller runs again. */
        break;
    }
    free(order);
    free(tree);
    return result;
}

anvil_pass_result_t anvil_pass_vectorize(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;
    const anvil_backend_t *backend = func->owner_ctx ? func->owner_ctx->backend : NULL;
    if (!backend || !backend->ops || !backend->ops->vector_operation_cost)
        return ANVIL_PASS_RUN_UNCHANGED;

    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    slp_built *built = calloc(SLP_BUILT, sizeof(*built));
    if (!built)
        return ANVIL_PASS_RUN_ERROR;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        size_t built_count = 0;
        /* Seed after seed, each on a fresh def-use snapshot; a block's
         * bundles stay reusable by its later seeds. */
        for (unsigned round = 0; round < 64; round++) {
            anvil_def_use_t uses;
            if (!anvil_def_use_build(func, &uses)) {
                free(built);
                return ANVIL_PASS_RUN_ERROR;
            }
            bool changed = false;
            anvil_pass_result_t status = vectorize_block(func, block, &uses, built, &built_count, &changed);
            anvil_def_use_destroy(&uses);
            if (status == ANVIL_PASS_RUN_ERROR) {
                free(built);
                return status;
            }
            if (!changed)
                break;
            result = ANVIL_PASS_RUN_CHANGED;
        }
    }
    free(built);
    return result;
}
