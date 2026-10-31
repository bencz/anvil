/*
 * Loop vectorization.
 *
 * An innermost counted loop
 *
 *     header:  i = phi [start, preheader], [i + 1, body]
 *              br (i < n | i u< n | i != n), body, exit
 *     body:    straight-line lane-wise arithmetic over base[i] accesses
 *              br header
 *
 * gets a vector copy that runs while a whole vector of iterations remains;
 * the original loop, entered with the vector loop's final index, finishes
 * the rest.  Bases whose distance is unknown are checked at run time: when
 * two accessed bases (one of them stored through) are closer than one
 * vector the vector loop runs zero iterations.  Lane counts and legality
 * come from the backend's vector_operation_cost, so nothing here knows a
 * target.
 */
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

#include <stdlib.h>

enum {
    LV_BODY_LIMIT = 64,
    LV_CHAIN_LIMIT = 4,         /* blocks of the body (the last is the latch) */
    LV_REDUCTION_LIMIT = 4,     /* accumulators besides the index */
    LV_STREAM_LIMIT = 6,
    LV_GEP_OPERANDS = 8,
    LV_VECTOR_BYTES = 16,
    /* Cost model, in the cost hook's units (a scalar operation costs 1). */
    LV_ASSUMED_TRIPS = 64,      /* trip count when it is not a constant */
    LV_LOOP_OVERHEAD = 2,       /* increment, compare and branch per iteration */
    LV_SETUP_COST = 4,          /* vector end computation */
    LV_CHECK_COST = 6,          /* one run-time overlap check */
    LV_EXIT_COST = 1,           /* the branch of one early exit */
};

/* LV_WIDE: an integer wider than the lanes whose low lane-width bits are
 * all that a later truncation to the lanes keeps (C's promotions of narrow
 * operands); it is computed in the lane type. */
/* LV_MASK: a comparison of lane values, which becomes a lane mask and may
 * only choose between lanes in a select. */
/* LV_BOUND: part of a clamp of a widened sum or difference of lanes to the
 * lanes' range (a comparison with a bound, or a select applying one bound
 * of two): the select completing the clamp computes the saturating lane
 * operation (LV_WIDE, `saturating`), these are not built. */
/* LV_ACCUM: an accumulator wider than the lanes adding one lane widened by
 * one extension (C's sum of narrow elements): it accumulates the narrow
 * lanes into a vector of wider ones (ACCUM_ZEXT/SEXT). */

/* An accumulator: a header PHI besides the index, starting at `init`,
 * updated once per iteration by `update` (an associative operation of it
 * and lane values) and read after the loop only through the PHI.  The
 * vector loop accumulates lane by lane from the operation's identity (min
 * and max: from init) and combines the lanes with `reduce` at the end. */
typedef struct {
    anvil_instr_t *phi, *update;
    anvil_value_t *init;
    anvil_op_t reduce;
    anvil_op_t combine;      /* the operation, seen through a truncation of
                                C-promoted narrow lanes (SELECT: min/max) */
    /* LV_ACCUM updates: ACCUM_ZEXT/SEXT, the update operand holding the
     * extension, the accumulator vector the target takes and its cost. */
    anvil_op_t accumulate;
    size_t lane_side;
    anvil_type_t *vector;
    unsigned cost;
} lv_reduction;
typedef enum { LV_SKIP, LV_INDEX, LV_ADDRESS, LV_VECTOR, LV_STORE, LV_WIDE, LV_MASK, LV_ACCUM, LV_BOUND } lv_class_t;

/* The bounds a clamp applies (lv_plan.clamp). */
enum { LV_CLAMP_HIGH = 1, LV_CLAMP_LOW = 2 };

typedef struct {
    anvil_func_t *func;
    anvil_ctx_t *ctx;
    anvil_block_t *preheader, *header;
    /* The body: blocks each leaving the loop on a data-dependent condition
     * (an early exit) or going on to the next, the last (the latch) going
     * back to the header.  A loop without early exits has one. */
    anvil_block_t *chain[LV_CHAIN_LIMIT];
    size_t chain_count;
    bool exit_on_true[LV_CHAIN_LIMIT];    /* which edge of chain[j]'s branch leaves */
    anvil_instr_t *phi, *increment, *compare;
    anvil_value_t *start, *limit;
    bool bounded;                     /* < or u<: the vector end needs start < n */
    anvil_type_t *element, *vector;
    size_t lanes;
    anvil_instr_t *instrs[LV_BODY_LIMIT];
    lv_class_t classes[LV_BODY_LIMIT];
    size_t count;
    size_t block_of[LV_BODY_LIMIT];       /* chain index of instrs[i] */
    /* LV_MASK: the comparison on the lanes, narrowed when the scalar one
     * compares widened lanes (see narrow_compare). */
    anvil_op_t lane_op[LV_BODY_LIMIT];
    bool narrowed[LV_BODY_LIMIT];
    /* Shifts by a loop-invariant count: true when the target takes the
     * scalar count for every lane (UNIFORM), false when it is splat. */
    bool uniform[LV_BODY_LIMIT];
    /* Clamps (LV_BOUND selects and the saturating LV_WIDE select ending
     * them): the bounds applied so far and the clamped sum's position;
     * lane_op holds the saturating operation. */
    uint8_t clamp[LV_BODY_LIMIT];
    size_t clamp_source[LV_BODY_LIMIT];
    bool saturating[LV_BODY_LIMIT];
    /* Estimated costs: one scalar iteration, one vector iteration, and the
     * preheader's broadcasts. */
    unsigned scalar_cost, vector_cost, splat_cost;
    lv_reduction reductions[LV_REDUCTION_LIMIT];
    size_t num_reductions;
    /* Distinct address streams (one GEP each) and whether one is stored. */
    anvil_instr_t *streams[LV_STREAM_LIMIT];
    bool stored[LV_STREAM_LIMIT];
    size_t num_streams;
} lv_plan;

static anvil_block_t *latch(const lv_plan *plan)
{
    return plan->chain[plan->chain_count - 1];
}

static bool in_loop(const lv_plan *plan, const anvil_value_t *value)
{
    if (value->kind != ANVIL_VAL_INSTR || !value->data.instr)
        return false;
    const anvil_block_t *block = value->data.instr->parent;
    if (block == plan->header)
        return true;
    for (size_t index = 0; index < plan->chain_count; index++) {
        if (plan->chain[index] == block)
            return true;
    }
    return false;
}

static bool is_const_one(const anvil_value_t *value)
{
    return value->kind == ANVIL_VAL_CONST_INT && value->data.i == 1;
}

static size_t body_index(const lv_plan *plan, const anvil_value_t *value)
{
    for (size_t index = 0; index < plan->count; index++) {
        if (plan->instrs[index]->result == value)
            return index;
    }
    return SIZE_MAX;
}

static lv_class_t operand_class(const lv_plan *plan, const anvil_value_t *value)
{
    if (value == plan->phi->result)
        return LV_INDEX;
    for (size_t reduction = 0; reduction < plan->num_reductions; reduction++) {
        if (value == plan->reductions[reduction].phi->result)
            return LV_VECTOR;
    }
    size_t index = body_index(plan, value);
    return index == SIZE_MAX ? LV_SKIP : plan->classes[index];
}

/* A vector lane value: computed in the body, or loop-invariant (splat). */
static bool lane_operand(const lv_plan *plan, const anvil_value_t *value)
{
    if (!in_loop(plan, value))
        return anvil_types_equal(value->type, plan->element) && value->kind != ANVIL_VAL_GLOBAL;
    return operand_class(plan, value) == LV_VECTOR;
}

/* Two addresses of one stream: the same walk up to the index. */
static bool same_stream(const anvil_instr_t *left, const anvil_instr_t *right)
{
    if (left->num_operands != right->num_operands || !anvil_types_equal(left->aux_type, right->aux_type))
        return false;
    for (size_t operand = 0; operand + 1 < left->num_operands; operand++) {
        if (left->operands[operand] != right->operands[operand])
            return false;
    }
    return true;
}

static bool note_stream(lv_plan *plan, anvil_instr_t *gep, bool stored)
{
    for (size_t index = 0; index < plan->num_streams; index++) {
        if (same_stream(plan->streams[index], gep)) {
            plan->stored[index] |= stored;
            return true;
        }
    }
    if (plan->num_streams == LV_STREAM_LIMIT)
        return false;
    plan->streams[plan->num_streams] = gep;
    plan->stored[plan->num_streams++] = stored;
    return true;
}

/* The address of one lane element: a walk whose last index is the loop
 * index and lands on the element, so consecutive iterations are adjacent. */
static anvil_instr_t *lane_address(const lv_plan *plan, const anvil_value_t *pointer)
{
    if (operand_class(plan, pointer) != LV_ADDRESS)
        return NULL;
    anvil_instr_t *gep = pointer->data.instr;
    return anvil_types_equal(gep->result->type->data.pointee, plan->element) ? gep : NULL;
}

/* Two-operand operations that act on each lane alone. */
static bool lane_arithmetic(anvil_op_t op)
{
    switch (op) {
    case ANVIL_OP_FADD:
    case ANVIL_OP_FSUB:
    case ANVIL_OP_FMUL:
    case ANVIL_OP_FDIV:
    case ANVIL_OP_ADD:
    case ANVIL_OP_SUB:
    case ANVIL_OP_MUL:
    case ANVIL_OP_AND:
    case ANVIL_OP_OR:
    case ANVIL_OP_XOR:
        return true;
    default:
        return false;
    }
}

/* Wrapping integer operations whose low N result bits depend only on the
 * low N bits of their operands. */
static bool keeps_low_bits(anvil_op_t op)
{
    return op == ANVIL_OP_ADD || op == ANVIL_OP_SUB || op == ANVIL_OP_MUL || op == ANVIL_OP_AND || op == ANVIL_OP_OR ||
           op == ANVIL_OP_XOR;
}

/* A widened operation computable in the lanes: every operand is widened
 * lane data or loop-invariant, and some operand is widened lane data. */
static bool narrowable(const lv_plan *plan, const anvil_instr_t *instr)
{
    if (!keeps_low_bits(instr->op) || instr->num_operands != 2 || !plan->element || !anvil_type_is_integer(plan->element) ||
        instr->result->type->size <= plan->element->size)
        return false;
    bool widened = false;
    for (size_t operand = 0; operand < 2; operand++) {
        const anvil_value_t *value = instr->operands[operand];
        if (operand_class(plan, value) == LV_WIDE)
            widened = true;
        else if (in_loop(plan, value) || value->kind == ANVIL_VAL_GLOBAL)
            return false;
    }
    return widened;
}

/* A widened lane value, or an invariant: an operand of a widened
 * operation. */
static bool wide_lanes(const lv_plan *plan, const anvil_value_t *value)
{
    return operand_class(plan, value) == LV_WIDE || (!in_loop(plan, value) && value->kind != ANVIL_VAL_GLOBAL);
}

/* The lane value a widening extension of lane data extends, or `value`. */
static const anvil_value_t *strip_extension(const lv_plan *plan, const anvil_value_t *value)
{
    if (operand_class(plan, value) == LV_WIDE && (value->data.instr->op == ANVIL_OP_ZEXT || value->data.instr->op == ANVIL_OP_SEXT))
        return value->data.instr->operands[0];
    return value;
}

/* Whether a constant survives widening from the lanes by `extend`. */
static bool fits_lanes(const lv_plan *plan, anvil_op_t extend, int64_t value)
{
    unsigned bits = (unsigned)plan->element->size * 8;
    if (bits >= 64)
        return true;
    if (extend == ANVIL_OP_ZEXT)
        return value >= 0 && (uint64_t)value < (UINT64_C(1) << bits);
    return value >= -(INT64_C(1) << (bits - 1)) && value < (INT64_C(1) << (bits - 1));
}

/* A comparison of lanes widened the same way (C compares promoted narrow
 * values), against each other or an invariant that fits the lanes: the
 * same comparison on the lanes, signed order becoming unsigned for
 * zero-extended lanes.  *op is the lane comparison. */
static bool narrow_compare(const lv_plan *plan, const anvil_instr_t *instr, anvil_op_t *op)
{
    if (instr->op == ANVIL_OP_FCMP || !plan->element || !anvil_type_is_integer(plan->element))
        return false;
    anvil_op_t extend = ANVIL_OP_NOP;
    for (size_t operand = 0; operand < 2; operand++) {
        const anvil_value_t *value = instr->operands[operand];
        if (in_loop(plan, value)) {
            const anvil_instr_t *wide = value->data.instr;
            if (operand_class(plan, value) != LV_WIDE || (wide->op != ANVIL_OP_ZEXT && wide->op != ANVIL_OP_SEXT) ||
                (extend != ANVIL_OP_NOP && extend != wide->op))
                return false;
            extend = wide->op;
        }
    }
    if (extend == ANVIL_OP_NOP)
        return false;
    for (size_t operand = 0; operand < 2; operand++) {
        const anvil_value_t *value = instr->operands[operand];
        if (in_loop(plan, value))
            continue;
        bool same_extension = value->kind == ANVIL_VAL_INSTR && value->data.instr->op == extend &&
                              anvil_types_equal(value->data.instr->operands[0]->type, plan->element);
        if (!same_extension && !(value->kind == ANVIL_VAL_CONST_INT && fits_lanes(plan, extend, value->data.i)))
            return false;
    }
    static const anvil_op_t unsigned_order[] = {
        [ANVIL_OP_CMP_LT] = ANVIL_OP_CMP_ULT, [ANVIL_OP_CMP_LE] = ANVIL_OP_CMP_ULE,
        [ANVIL_OP_CMP_GT] = ANVIL_OP_CMP_UGT, [ANVIL_OP_CMP_GE] = ANVIL_OP_CMP_UGE,
    };
    bool order = instr->op != ANVIL_OP_CMP_EQ && instr->op != ANVIL_OP_CMP_NE;
    bool signed_order = instr->op >= ANVIL_OP_CMP_LT && instr->op <= ANVIL_OP_CMP_GE;
    *op = extend == ANVIL_OP_ZEXT && signed_order ? unsigned_order[instr->op] : instr->op;
    /* The lane type's signedness must match the order compared. */
    bool unsigned_lanes = *op >= ANVIL_OP_CMP_ULT && *op <= ANVIL_OP_CMP_UGE;
    return !order || unsigned_lanes == !plan->element->is_signed;
}

/* acc + ext(lane) with acc a header PHI wider than the lanes (see
 * LV_ACCUM); match_reductions checks that acc is an accumulator. */
static bool accumulates_extension(const lv_plan *plan, const anvil_instr_t *instr)
{
    if (instr->op != ANVIL_OP_ADD || instr->num_operands != 2 || !plan->element || !anvil_type_is_integer(plan->element) ||
        instr->result->type->size <= plan->element->size)
        return false;
    for (size_t side = 0; side < 2; side++) {
        const anvil_value_t *acc = instr->operands[1 - side], *ext = instr->operands[side];
        bool phi = false;
        for (size_t reduction = 0; reduction < plan->num_reductions; reduction++)
            phi |= plan->reductions[reduction].phi->result == acc;
        if (phi && operand_class(plan, ext) == LV_WIDE && (ext->data.instr->op == ANVIL_OP_ZEXT || ext->data.instr->op == ANVIL_OP_SEXT) &&
            anvil_types_equal(ext->type, instr->result->type))
            return true;
    }
    return false;
}

/* A widened sum or difference of two lanes extended as the lanes' type
 * says (zero for unsigned lanes, sign for signed): the value a clamp may
 * saturate. */
static bool saturable(const lv_plan *plan, const anvil_value_t *value)
{
    if (operand_class(plan, value) != LV_WIDE || (value->data.instr->op != ANVIL_OP_ADD && value->data.instr->op != ANVIL_OP_SUB) ||
        !plan->element || !anvil_type_is_integer(plan->element))
        return false;
    anvil_op_t extend = plan->element->is_signed ? ANVIL_OP_SEXT : ANVIL_OP_ZEXT;
    for (size_t operand = 0; operand < 2; operand++) {
        const anvil_value_t *lane = value->data.instr->operands[operand];
        if (operand_class(plan, lane) != LV_WIDE || lane->data.instr->op != extend)
            return false;
    }
    return true;
}

/* A clamp's value so far: the saturable sum, or a select applying one of
 * its bounds. */
static bool clamped_value(const lv_plan *plan, const anvil_value_t *value)
{
    size_t at = body_index(plan, value);
    return saturable(plan, value) || (at != SIZE_MAX && plan->classes[at] == LV_BOUND && plan->instrs[at]->op == ANVIL_OP_SELECT);
}

/* A comparison of a clamp's value with a constant (LV_BOUND; the select
 * using it is checked in match_clamp). */
static bool clamp_test(const lv_plan *plan, const anvil_instr_t *compare)
{
    if (compare->op == ANVIL_OP_FCMP || compare->num_operands != 2)
        return false;
    for (size_t side = 0; side < 2; side++) {
        if (clamped_value(plan, compare->operands[side]) && compare->operands[1 - side]->kind == ANVIL_VAL_CONST_INT)
            return true;
    }
    return false;
}

/* select(compare, x, y) as one bound of a clamp: compare normalized to
 * `v > k` choosing on_true/on_false; a high bound is (v > k ? H : v) with
 * k in {H - 1, H}, a low one (v > k ? v : L) with k in {L - 1, L}, H and L
 * the lanes' range.  Records the bounds so far; the select completing the
 * ones the sum's range needs is the saturating operation. */
static bool match_clamp(lv_plan *plan, size_t index, const anvil_instr_t *select)
{
    const anvil_instr_t *compare = select->operands[0]->data.instr;
    size_t side = clamped_value(plan, compare->operands[0]) ? 0 : 1;
    const anvil_value_t *value = compare->operands[side];
    int64_t k = compare->operands[1 - side]->data.i;
    const anvil_value_t *on_true = select->operands[1], *on_false = select->operands[2];
    /* `v op k` (or `k op v`, mirrored) as v > k', swapping the arms of a
     * negation. */
    anvil_op_t op = compare->op;
    if (side == 1) {
        static const anvil_op_t mirror[] = {[ANVIL_OP_CMP_LT] = ANVIL_OP_CMP_GT, [ANVIL_OP_CMP_LE] = ANVIL_OP_CMP_GE,
                                            [ANVIL_OP_CMP_GT] = ANVIL_OP_CMP_LT, [ANVIL_OP_CMP_GE] = ANVIL_OP_CMP_LE,
                                            [ANVIL_OP_CMP_ULT] = ANVIL_OP_CMP_UGT, [ANVIL_OP_CMP_ULE] = ANVIL_OP_CMP_UGE,
                                            [ANVIL_OP_CMP_UGT] = ANVIL_OP_CMP_ULT, [ANVIL_OP_CMP_UGE] = ANVIL_OP_CMP_ULE};
        if (op < ANVIL_OP_CMP_LT || op > ANVIL_OP_CMP_UGE)
            return false;
        op = mirror[op];
    }
    switch (op) {
    case ANVIL_OP_CMP_GT: case ANVIL_OP_CMP_UGT: break;
    case ANVIL_OP_CMP_GE: case ANVIL_OP_CMP_UGE: k--; break;
    case ANVIL_OP_CMP_LE: case ANVIL_OP_CMP_ULE: { const anvil_value_t *t = on_true; on_true = on_false; on_false = t; break; }
    case ANVIL_OP_CMP_LT: case ANVIL_OP_CMP_ULT: { const anvil_value_t *t = on_true; on_true = on_false; on_false = t; k--; break; }
    default: return false;
    }
    /* Unsigned comparisons agree with signed ones only on non-negative
     * values: an unsigned clamp of a signed range is no saturation. */
    if (op >= ANVIL_OP_CMP_ULT && plan->element->is_signed)
        return false;

    unsigned bits = (unsigned)plan->element->size * 8;
    int64_t high = plan->element->is_signed ? (INT64_C(1) << (bits - 1)) - 1 : (INT64_C(1) << bits) - 1;
    int64_t low = plan->element->is_signed ? -(INT64_C(1) << (bits - 1)) : 0;
    uint8_t bound;
    if (on_false == value && on_true->kind == ANVIL_VAL_CONST_INT && on_true->data.i == high && (k == high - 1 || k == high))
        bound = LV_CLAMP_HIGH;
    else if (on_true == value && on_false->kind == ANVIL_VAL_CONST_INT && on_false->data.i == low && (k == low - 1 || k == low))
        bound = LV_CLAMP_LOW;
    else
        return false;

    size_t at = body_index(plan, value);
    bool from_sum = saturable(plan, value);
    size_t source = from_sum ? at : plan->clamp_source[at];
    uint8_t bounds = (uint8_t)((from_sum ? 0 : plan->clamp[at]) | bound);
    const anvil_instr_t *sum = plan->instrs[source];
    bool add = sum->op == ANVIL_OP_ADD;
    /* An unsigned sum only overflows upward, a difference only downward. */
    uint8_t needed = plan->element->is_signed ? (LV_CLAMP_HIGH | LV_CLAMP_LOW) : add ? LV_CLAMP_HIGH : LV_CLAMP_LOW;
    plan->clamp[index] = bounds;
    plan->clamp_source[index] = source;
    if ((bounds & needed) != needed) {
        plan->classes[index] = LV_BOUND;
        return true;
    }
    plan->saturating[index] = true;
    plan->lane_op[index] = plan->element->is_signed ? (add ? ANVIL_OP_ADD_SAT_S : ANVIL_OP_SUB_SAT_S)
                                                    : (add ? ANVIL_OP_ADD_SAT_U : ANVIL_OP_SUB_SAT_U);
    plan->classes[index] = LV_WIDE;
    return true;
}

static bool is_shift(anvil_op_t op)
{
    return op == ANVIL_OP_SHL || op == ANVIL_OP_SHR || op == ANVIL_OP_SAR;
}

static bool set_element(lv_plan *plan, anvil_type_t *type)
{
    if (!plan->element) {
        plan->element = type;
        return true;
    }
    return anvil_types_equal(plan->element, type);
}

/* Classifies the instructions of chain[block] (all but its branch). */
static bool classify_block(lv_plan *plan, size_t block)
{
    const anvil_block_t *current = plan->chain[block];
    for (anvil_instr_t *instr = current->first; instr && instr != current->last; instr = instr->next) {
        if (plan->count == LV_BODY_LIMIT)
            return false;
        size_t index = plan->count++;
        plan->instrs[index] = instr;
        plan->block_of[index] = block;
        lv_class_t class;
        switch (instr->op) {
        case ANVIL_OP_NOP:
            class = LV_SKIP;
            break;
        case ANVIL_OP_SEXT:
        case ANVIL_OP_ZEXT:
            if (instr->operands[0] == plan->phi->result)
                class = LV_INDEX;
            else if (plan->element && operand_class(plan, instr->operands[0]) == LV_VECTOR && anvil_type_is_integer(plan->element) &&
                     anvil_types_equal(instr->operands[0]->type, plan->element))
                class = LV_WIDE;
            else
                return false;
            break;
        case ANVIL_OP_TRUNC:
            if (operand_class(plan, instr->operands[0]) != LV_WIDE || !set_element(plan, instr->result->type))
                return false;
            class = LV_VECTOR;
            break;
        case ANVIL_OP_GEP:
            if (instr->num_operands < 2 || instr->num_operands > LV_GEP_OPERANDS ||
                operand_class(plan, instr->operands[instr->num_operands - 1]) != LV_INDEX || !note_stream(plan, instr, false))
                return false;
            for (size_t operand = 0; operand + 1 < instr->num_operands; operand++) {
                if (in_loop(plan, instr->operands[operand]))
                    return false;
            }
            class = LV_ADDRESS;
            break;
        case ANVIL_OP_LOAD: {
            anvil_instr_t *gep;
            if (instr->memory_access.is_volatile || !set_element(plan, instr->result->type) || !(gep = lane_address(plan, instr->operands[0])))
                return false;
            class = LV_VECTOR;
            break;
        }
        case ANVIL_OP_STORE: {
            anvil_instr_t *gep;
            if (instr->memory_access.is_volatile || !set_element(plan, instr->operands[0]->type) ||
                !(gep = lane_address(plan, instr->operands[1])) || !lane_operand(plan, instr->operands[0]) ||
                !note_stream(plan, gep, true))
                return false;
            class = LV_STORE;
            break;
        }
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
            if (clamp_test(plan, instr)) {
                class = LV_BOUND;
                break;
            }
            plan->lane_op[index] = instr->op;
            if (narrow_compare(plan, instr, &plan->lane_op[index])) {
                plan->narrowed[index] = true;
            } else if (!set_element(plan, instr->operands[0]->type) || !lane_operand(plan, instr->operands[0]) ||
                       !lane_operand(plan, instr->operands[1])) {
                return false;
            }
            class = LV_MASK;
            break;
        case ANVIL_OP_SELECT:
            if (operand_class(plan, instr->operands[0]) == LV_BOUND) {
                if (!match_clamp(plan, index, instr))
                    return false;
                continue;
            }
            if (operand_class(plan, instr->operands[0]) != LV_MASK)
                return false;
            if (wide_lanes(plan, instr->operands[1]) && wide_lanes(plan, instr->operands[2])) {
                /* Choosing between widened lanes is a widened lane. */
                class = LV_WIDE;
                break;
            }
            if (!set_element(plan, instr->result->type) || !lane_operand(plan, instr->operands[1]) || !lane_operand(plan, instr->operands[2]))
                return false;
            class = LV_VECTOR;
            break;
        case ANVIL_OP_FMA:
        case ANVIL_OP_FSQRT:
            if (!set_element(plan, instr->result->type))
                return false;
            for (size_t operand = 0; operand < instr->num_operands; operand++) {
                if (!lane_operand(plan, instr->operands[operand]))
                    return false;
            }
            class = LV_VECTOR;
            break;
        case ANVIL_OP_SHL:
        case ANVIL_OP_SHR:
        case ANVIL_OP_SAR:
            /* Lane counts, or one invariant count of any integer width. */
            if (!set_element(plan, instr->result->type) || !lane_operand(plan, instr->operands[0]) ||
                (in_loop(plan, instr->operands[1]) ? !lane_operand(plan, instr->operands[1])
                                                   : !anvil_type_is_integer(instr->operands[1]->type)))
                return false;
            class = LV_VECTOR;
            break;
        default:
            if (instr == plan->increment) {
                class = LV_SKIP;
                break;
            }
            if (accumulates_extension(plan, instr)) {
                class = LV_ACCUM;
                break;
            }
            if (narrowable(plan, instr)) {
                class = LV_WIDE;
                break;
            }
            if (!lane_arithmetic(instr->op) || instr->num_operands != 2 || !set_element(plan, instr->result->type) ||
                !lane_operand(plan, instr->operands[0]) || !lane_operand(plan, instr->operands[1]))
                return false;
            class = LV_VECTOR;
            break;
        }
        plan->classes[index] = class;
    }
    return true;
}

/* Whether `value` is read by a body instruction other than `allowed` or
 * `also` (either may be NULL). */
static bool read_elsewhere(const lv_plan *plan, const anvil_value_t *value, const anvil_instr_t *allowed, const anvil_instr_t *also)
{
    for (size_t index = 0; index < plan->count; index++) {
        const anvil_instr_t *instr = plan->instrs[index];
        if (instr == allowed || instr == also)
            continue;
        for (size_t operand = 0; operand < instr->num_operands; operand++) {
            if (instr->operands[operand] == value)
                return true;
        }
    }
    for (size_t block = 0; block < plan->chain_count; block++) {
        if (plan->chain[block]->last->num_operands && plan->chain[block]->last->operands[0] == value)
            return true;
    }
    return false;
}

/* min/max written as select(cmp(p, q), p or q, the other): which one. */
static anvil_op_t min_max_reduction(const lv_plan *plan, const anvil_instr_t *select, const anvil_value_t *accumulator)
{
    const anvil_value_t *cond = select->operands[0];
    size_t at = body_index(plan, cond);
    if (at == SIZE_MAX || plan->classes[at] != LV_MASK || !anvil_type_is_integer(plan->element))
        return ANVIL_OP_NOP;
    const anvil_instr_t *compare = plan->instrs[at];
    const anvil_value_t *p = strip_extension(plan, compare->operands[0]), *q = strip_extension(plan, compare->operands[1]);
    const anvil_value_t *chosen = strip_extension(plan, select->operands[1]), *other = strip_extension(plan, select->operands[2]);
    if (!((chosen == p && other == q) || (chosen == q && other == p)) || (p != accumulator && q != accumulator))
        return ANVIL_OP_NOP;
    bool greater, unsigned_order;
    switch (plan->lane_op[at]) {
    case ANVIL_OP_CMP_GT: case ANVIL_OP_CMP_GE: greater = true; unsigned_order = false; break;
    case ANVIL_OP_CMP_LT: case ANVIL_OP_CMP_LE: greater = false; unsigned_order = false; break;
    case ANVIL_OP_CMP_UGT: case ANVIL_OP_CMP_UGE: greater = true; unsigned_order = true; break;
    case ANVIL_OP_CMP_ULT: case ANVIL_OP_CMP_ULE: greater = false; unsigned_order = true; break;
    default: return ANVIL_OP_NOP;
    }
    bool maximum = greater == (chosen == p);
    if (maximum)
        return unsigned_order ? ANVIL_OP_REDUCE_UMAX : ANVIL_OP_REDUCE_SMAX;
    return unsigned_order ? ANVIL_OP_REDUCE_UMIN : ANVIL_OP_REDUCE_SMIN;
}

/* Whether every body reader of `value` is `core` or `compare`, directly or
 * through a widening extension only they read. */
static bool read_only_by(const lv_plan *plan, const anvil_value_t *value, const anvil_instr_t *core, const anvil_instr_t *compare)
{
    for (size_t index = 0; index < plan->count; index++) {
        const anvil_instr_t *instr = plan->instrs[index];
        if (instr == core || instr == compare)
            continue;
        for (size_t operand = 0; operand < instr->num_operands; operand++) {
            if (instr->operands[operand] != value)
                continue;
            bool extension = plan->classes[index] == LV_WIDE && (instr->op == ANVIL_OP_ZEXT || instr->op == ANVIL_OP_SEXT);
            if (!extension || read_elsewhere(plan, instr->result, core, compare))
                return false;
        }
    }
    for (size_t block = 0; block < plan->chain_count; block++) {
        if (plan->chain[block]->last->num_operands && plan->chain[block]->last->operands[0] == value)
            return false;
    }
    return true;
}

/* Each accumulator's update and the reduction that finishes it; nothing
 * else in the body reads the accumulator or its next value. */
static bool match_reductions(lv_plan *plan)
{
    if (plan->num_reductions && plan->chain_count > 1)
        return false;  /* an early exit would resume within a vector */
    for (size_t index = 0; index < plan->num_reductions; index++) {
        lv_reduction *reduction = &plan->reductions[index];
        anvil_instr_t *update = reduction->update;
        size_t at = body_index(plan, update->result);
        const anvil_value_t *accumulator = reduction->phi->result;
        bool accumulates = at != SIZE_MAX && plan->classes[at] == LV_ACCUM;
        if (at == SIZE_MAX || (plan->classes[at] != LV_VECTOR && !accumulates) ||
            (!accumulates && !anvil_types_equal(accumulator->type, plan->element)) || read_elsewhere(plan, update->result, NULL, NULL))
            return false;
        if (accumulates) {
            /* Sums of widened lanes: every lane adds into some wider one. */
            reduction->lane_side = update->operands[0] == accumulator ? 1 : 0;
            reduction->accumulate = update->operands[reduction->lane_side]->data.instr->op == ANVIL_OP_ZEXT ? ANVIL_OP_ACCUM_ZEXT
                                                                                                         : ANVIL_OP_ACCUM_SEXT;
            reduction->reduce = ANVIL_OP_REDUCE_ADD;
            reduction->combine = ANVIL_OP_ADD;
            if (!read_only_by(plan, accumulator, update, NULL))
                return false;
            continue;
        }
        /* C computes narrow accumulators widened and truncates. */
        anvil_instr_t *core = update;
        if (update->op == ANVIL_OP_TRUNC) {
            core = update->operands[0]->data.instr;
            if (read_elsewhere(plan, core->result, update, NULL))
                return false;
        }
        const anvil_instr_t *compare = NULL;
        reduction->combine = core->op;
        switch (core->op) {
        case ANVIL_OP_ADD: reduction->reduce = ANVIL_OP_REDUCE_ADD; break;
        case ANVIL_OP_AND: reduction->reduce = ANVIL_OP_REDUCE_AND; break;
        case ANVIL_OP_OR: reduction->reduce = ANVIL_OP_REDUCE_OR; break;
        case ANVIL_OP_XOR: reduction->reduce = ANVIL_OP_REDUCE_XOR; break;
        case ANVIL_OP_FADD:
        case ANVIL_OP_FMA:
            /* The lanes' partial sums add in another order; a contracted
             * s + a*b accumulates the products the same way. */
            if (!plan->func->fp_reassociation ||
                (core->op == ANVIL_OP_FMA && (core->operands[2] != accumulator || core->operands[0] == accumulator || core->operands[1] == accumulator)))
                return false;
            reduction->reduce = ANVIL_OP_REDUCE_FADD;
            reduction->combine = ANVIL_OP_FADD;
            break;
        case ANVIL_OP_SELECT:
            reduction->reduce = min_max_reduction(plan, core, accumulator);
            if (reduction->reduce == ANVIL_OP_NOP)
                return false;
            compare = core->operands[0]->data.instr;
            break;
        default:
            return false;
        }
        if (reduction->reduce != ANVIL_OP_REDUCE_FADD && !anvil_type_is_integer(plan->element))
            return false;
        if (core->op != ANVIL_OP_SELECT && core->op != ANVIL_OP_FMA &&
            (strip_extension(plan, core->operands[0]) == accumulator) == (strip_extension(plan, core->operands[1]) == accumulator))
            return false;
        if (!read_only_by(plan, accumulator, core, compare))
            return false;
    }
    return true;
}

static bool classify_body(lv_plan *plan)
{
    for (size_t block = 0; block < plan->chain_count; block++) {
        if (!classify_block(plan, block))
            return false;
    }

    /* Scalar index and address values feed only addresses and accesses. */
    for (size_t index = 0; index < plan->count; index++) {
        anvil_instr_t *instr = plan->instrs[index];
        for (size_t operand = 0; operand < instr->num_operands; operand++) {
            lv_class_t class = operand_class(plan, instr->operands[operand]);
            bool allowed = class == LV_SKIP || class == LV_VECTOR ||
                           (class == LV_MASK && instr->op == ANVIL_OP_SELECT && operand == 0) ||
                           (class == LV_WIDE && (plan->classes[index] == LV_WIDE || plan->classes[index] == LV_ACCUM ||
                                                 plan->classes[index] == LV_BOUND || instr->op == ANVIL_OP_TRUNC || plan->narrowed[index])) ||
                           /* A clamp's parts feed only its later parts. */
                           (class == LV_BOUND && (plan->classes[index] == LV_BOUND || plan->saturating[index])) ||
                           (class == LV_INDEX && (instr->op == ANVIL_OP_GEP || instr == plan->increment ||
                                                  instr->op == ANVIL_OP_SEXT || instr->op == ANVIL_OP_ZEXT)) ||
                           (class == LV_ADDRESS && ((instr->op == ANVIL_OP_LOAD && operand == 0) ||
                                                    (instr->op == ANVIL_OP_STORE && operand == 1)));
            if (!allowed)
                return false;
        }
    }
    /* An early exit leaves on a comparison of lane values, and nothing is
     * stored: a vector iteration may run lanes past the exit. */
    for (size_t block = 0; block + 1 < plan->chain_count; block++) {
        if (operand_class(plan, plan->chain[block]->last->operands[0]) != LV_MASK)
            return false;
    }
    for (size_t index = 0; plan->chain_count > 1 && index < plan->count; index++) {
        if (plan->classes[index] == LV_STORE)
            return false;
    }
    return plan->element != NULL;
}

/* The body's blocks from the header's in-loop successor: each either the
 * latch (back to the header) or a two-way branch whose other edge leaves
 * the loop; each entered only from the one before. */
static bool match_chain(lv_plan *plan, const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, anvil_block_t *first)
{
    anvil_block_t *block = first;
    for (;;) {
        size_t index = anvil_opt_cfg_index(cfg, block);
        if (block == plan->header || plan->chain_count == LV_CHAIN_LIMIT || index == SIZE_MAX ||
            cfg->predecessor_offsets[index + 1] - cfg->predecessor_offsets[index] != 1 || !block->last)
            return false;
        plan->chain[plan->chain_count] = block;
        anvil_instr_t *branch = block->last;
        if (branch->op == ANVIL_OP_BR) {
            plan->chain_count++;
            return branch->true_block == plan->header;
        }
        if (branch->op != ANVIL_OP_BR_COND)
            return false;
        bool true_stays = anvil_loop_contains(loops, loop, anvil_opt_cfg_index(cfg, branch->true_block));
        bool false_stays = anvil_loop_contains(loops, loop, anvil_opt_cfg_index(cfg, branch->false_block));
        if (true_stays == false_stays)
            return false;
        plan->exit_on_true[plan->chain_count++] = !true_stays;
        block = true_stays ? branch->true_block : branch->false_block;
    }
}

/* The loop shape, its induction variable and its trip condition. */
static bool match_loop(lv_plan *plan, const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t index)
{
    const anvil_loop_info_t *loop = &loops->loops[index];
    if (loop->latch_count != 1 || loop->preheader == SIZE_MAX)
        return false;
    plan->header = cfg->blocks[loop->header];
    plan->preheader = cfg->blocks[loop->preheader];
    if (plan->header->vectorized || !plan->preheader->last || plan->preheader->last->op != ANVIL_OP_BR)
        return false;

    /* PHIs (the index and accumulators), the trip test and its branch. */
    anvil_instr_t *compare = plan->header->first;
    while (compare && compare->op == ANVIL_OP_PHI)
        compare = compare->next;
    anvil_instr_t *branch = compare ? compare->next : NULL;
    if (!compare || compare == plan->header->first || !branch || branch != plan->header->last || branch->op != ANVIL_OP_BR_COND ||
        branch->operands[0] != compare->result || compare->num_operands != 2)
        return false;
    if (!match_chain(plan, cfg, loops, index, branch->true_block) || loop->member_count != plan->chain_count + 1 ||
        loop->exit_edge_count != plan->chain_count)
        return false;

    anvil_instr_t *phi = NULL;
    for (anvil_instr_t *candidate = plan->header->first; candidate != compare; candidate = candidate->next) {
        anvil_value_t *start = NULL, *next = NULL;
        if (candidate->num_phi_incoming != 2)
            return false;
        for (size_t item = 0; item < 2; item++) {
            if (candidate->phi_blocks[item] == plan->preheader)
                start = candidate->operands[item];
            else if (candidate->phi_blocks[item] == latch(plan))
                next = candidate->operands[item];
        }
        if (!start || !next || next->kind != ANVIL_VAL_INSTR)
            return false;
        bool tested = compare->operands[0] == candidate->result || compare->operands[1] == candidate->result;
        if (tested && !phi) {
            phi = candidate;
            plan->start = start;
            plan->increment = next->data.instr;
        } else if (plan->num_reductions < LV_REDUCTION_LIMIT) {
            plan->reductions[plan->num_reductions++] =
                (lv_reduction){.phi = candidate, .update = next->data.instr, .init = start, .accumulate = ANVIL_OP_NOP};
        } else {
            return false;
        }
    }
    if (!phi)
        return false;

    anvil_type_t *index_type = phi->result->type;
    if (index_type->kind != ANVIL_TYPE_I32 && index_type->kind != ANVIL_TYPE_I64 && index_type->kind != ANVIL_TYPE_U32 &&
        index_type->kind != ANVIL_TYPE_U64)
        return false;
    anvil_instr_t *increment = plan->increment;
    if (increment->op != ANVIL_OP_ADD || increment->parent != latch(plan) ||
        !((increment->operands[0] == phi->result && is_const_one(increment->operands[1])) ||
          (increment->operands[1] == phi->result && is_const_one(increment->operands[0]))))
        return false;

    /* i < n, i u< n, i != n, or the same with the operands swapped. */
    anvil_value_t *left = compare->operands[0], *right = compare->operands[1];
    switch (compare->op) {
    case ANVIL_OP_CMP_LT:
    case ANVIL_OP_CMP_ULT:
        plan->limit = left == phi->result ? right : NULL;
        plan->bounded = true;
        break;
    case ANVIL_OP_CMP_GT:
    case ANVIL_OP_CMP_UGT:
        plan->limit = right == phi->result ? left : NULL;
        plan->bounded = true;
        break;
    case ANVIL_OP_CMP_NE:
        plan->limit = left == phi->result ? right : right == phi->result ? left : NULL;
        break;
    default:
        return false;
    }
    if (!plan->limit || in_loop(plan, plan->limit) || !anvil_types_equal(plan->limit->type, index_type))
        return false;
    plan->phi = phi;
    plan->compare = compare;
    return true;
}

/* The cost of one scalar operation in the cost hook's units: the target's
 * price for the operations it prices on single values (FMA, FSQRT). */
static unsigned scalar_cost(lv_plan *plan, const anvil_instr_t *instr)
{
    unsigned target = instr->result ? anvil_vector_shape_cost(plan->ctx, instr->op, instr->result->type, ANVIL_VECTOR_SHAPE_SCALAR) : 0;
    if (target)
        return target;
    switch (instr->op) {
    case ANVIL_OP_NOP:
    case ANVIL_OP_GEP:     /* folded into the access */
    case ANVIL_OP_SEXT:
    case ANVIL_OP_ZEXT:
    case ANVIL_OP_TRUNC:
        return 0;
    case ANVIL_OP_FDIV:
        return 4;
    default:
        return 1;
    }
}

/* A shift by an invariant count: the target's scalar-count form, or a
 * splat of the count (of the lanes' type) and per-lane counts. */
static unsigned invariant_shift_cost(lv_plan *plan, size_t index)
{
    anvil_instr_t *instr = plan->instrs[index];
    unsigned uniform = anvil_vector_shape_cost(plan->ctx, instr->op, plan->vector, ANVIL_VECTOR_SHAPE_UNIFORM);
    if (uniform) {
        plan->uniform[index] = true;
        return uniform;
    }
    unsigned splat = anvil_vector_operation_cost(plan->ctx, ANVIL_OP_SPLAT, plan->vector);
    unsigned lanes = anvil_vector_operation_cost(plan->ctx, instr->op, plan->vector);
    if (!splat || !lanes || !anvil_types_equal(instr->operands[1]->type, plan->element))
        return 0;
    plan->splat_cost += splat;
    return lanes;
}

/* The accumulator vector of an LV_ACCUM reduction: the narrowest integer
 * lanes at least as wide as the scalar accumulator (summing modulo a wider
 * width truncates to the same value) the target accumulates the lanes
 * into. */
static anvil_pass_result_t choose_accumulator(lv_plan *plan, lv_reduction *reduction)
{
    anvil_type_t *scalar = reduction->phi->result->type;
    unsigned element_bits = (unsigned)plan->element->size * 8;
    for (unsigned bits = (unsigned)scalar->size * 8; bits <= 64; bits *= 2) {
        unsigned ratio = bits / element_bits;
        anvil_type_t *lane = bits == 64   ? (scalar->is_signed ? anvil_type_i64(plan->ctx) : anvil_type_u64(plan->ctx))
                             : bits == 32 ? (scalar->is_signed ? anvil_type_i32(plan->ctx) : anvil_type_u32(plan->ctx))
                             : bits == 16 ? (scalar->is_signed ? anvil_type_i16(plan->ctx) : anvil_type_u16(plan->ctx))
                                          : NULL;
        if (!lane || (ratio != 2 && ratio != 4 && ratio != 8))
            continue;
        anvil_type_t *vector = anvil_type_vector(plan->ctx, lane, LV_VECTOR_BYTES * 8 / bits);
        if (!vector)
            return ANVIL_PASS_RUN_ERROR;
        unsigned cost = anvil_vector_shape_cost(plan->ctx, reduction->accumulate, vector, anvil_vector_widen_shape(ratio));
        if (cost) {
            reduction->vector = vector;
            reduction->cost = cost;
            return ANVIL_PASS_RUN_CHANGED;
        }
    }
    return ANVIL_PASS_RUN_UNCHANGED;
}

static lv_reduction *reduction_of(lv_plan *plan, const anvil_instr_t *update)
{
    for (size_t index = 0; index < plan->num_reductions; index++) {
        if (plan->reductions[index].update == update)
            return &plan->reductions[index];
    }
    return NULL;
}

/* The vector a reduction accumulates in. */
static anvil_type_t *accumulator_vector(const lv_plan *plan, const lv_reduction *reduction)
{
    return reduction->vector ? reduction->vector : plan->vector;
}

static bool needs_overlap_check(const lv_plan *plan, size_t a, size_t b)
{
    return plan->stored[a] || plan->stored[b];
}

/* An extension of lane data or the truncation back to the lanes. */
static bool lane_identity(const lv_plan *plan, size_t index)
{
    anvil_op_t op = plan->instrs[index]->op;
    return (plan->classes[index] == LV_WIDE && (op == ANVIL_OP_ZEXT || op == ANVIL_OP_SEXT)) ||
           (plan->classes[index] == LV_VECTOR && op == ANVIL_OP_TRUNC);
}

/* RUN_CHANGED when the target can run every operation on vectors, with the
 * per-iteration costs of both loops in `plan`. */
static anvil_pass_result_t vector_costs(lv_plan *plan)
{
    size_t size = plan->element->size;
    if (!size || size > LV_VECTOR_BYTES / 2 || LV_VECTOR_BYTES % size)
        return ANVIL_PASS_RUN_UNCHANGED;
    if (anvil_type_is_floating(plan->element) && !plan->func->fp_vectorization)
        return ANVIL_PASS_RUN_UNCHANGED;
    plan->lanes = LV_VECTOR_BYTES / size;
    plan->vector = anvil_type_vector(plan->ctx, plan->element, plan->lanes);
    if (!plan->vector)
        return ANVIL_PASS_RUN_ERROR;

    for (size_t index = 0; index < plan->num_reductions; index++) {
        anvil_pass_result_t chosen = plan->reductions[index].accumulate != ANVIL_OP_NOP ? choose_accumulator(plan, &plan->reductions[index])
                                                                                        : ANVIL_PASS_RUN_CHANGED;
        if (chosen != ANVIL_PASS_RUN_CHANGED)
            return chosen;
    }

    plan->scalar_cost = plan->vector_cost = LV_LOOP_OVERHEAD;
    for (size_t index = 0; index < plan->count; index++) {
        anvil_instr_t *instr = plan->instrs[index];
        lv_class_t class = plan->classes[index];
        if (class == LV_SKIP)
            continue;
        plan->scalar_cost += scalar_cost(plan, instr);
        if (class == LV_ACCUM) {
            plan->vector_cost += reduction_of(plan, instr)->cost;
            continue;
        }
        if (plan->saturating[index]) {
            unsigned cost = anvil_vector_operation_cost(plan->ctx, plan->lane_op[index], plan->vector);
            if (!cost)
                return ANVIL_PASS_RUN_UNCHANGED;
            plan->vector_cost += cost;
            continue;
        }
        /* Widening and truncating lane data are identities on the lanes. */
        if ((class != LV_VECTOR && class != LV_STORE && class != LV_WIDE && class != LV_MASK) || lane_identity(plan, index))
            continue;
        anvil_op_t lane_op = class == LV_MASK ? plan->lane_op[index] : instr->op;
        unsigned cost = is_shift(instr->op) && !in_loop(plan, instr->operands[1]) ? invariant_shift_cost(plan, index)
                                                                                 : anvil_vector_operation_cost(plan->ctx, lane_op, plan->vector);
        if (!cost)
            return ANVIL_PASS_RUN_UNCHANGED;
        plan->vector_cost += cost;
        /* Invariant lane operands are broadcast once, before the loop. */
        size_t first = instr->op == ANVIL_OP_LOAD ? instr->num_operands : instr->op == ANVIL_OP_SELECT ? 1 : 0;
        size_t last = instr->op == ANVIL_OP_STORE || is_shift(instr->op) ? 1 : instr->num_operands;
        for (size_t operand = first; operand < last; operand++) {
            if (in_loop(plan, instr->operands[operand]))
                continue;
            unsigned splat = anvil_vector_operation_cost(plan->ctx, ANVIL_OP_SPLAT, plan->vector);
            if (!splat)
                return ANVIL_PASS_RUN_UNCHANGED;
            plan->splat_cost += splat;
        }
    }

    /* Accumulators: an identity broadcast before the loop, and after it the
     * reduction of the lanes and its combination with the start value. */
    for (size_t index = 0; index < plan->num_reductions; index++) {
        anvil_type_t *vector = accumulator_vector(plan, &plan->reductions[index]);
        unsigned reduce = anvil_vector_operation_cost(plan->ctx, plan->reductions[index].reduce, vector);
        unsigned splat = anvil_vector_operation_cost(plan->ctx, ANVIL_OP_SPLAT, vector);
        if (!reduce || !splat)
            return ANVIL_PASS_RUN_UNCHANGED;
        plan->splat_cost += splat + reduce + 1;
    }

    /* An early exit tests whether any lane leaves: the leaving comparison
     * (inverted when the false edge leaves), the OR of its mask and a
     * branch.  Lanes past the leaving one are read too, so the target must
     * guarantee that an aligned vector cannot fault where one of its bytes
     * is readable (the vector loop runs only on aligned streams). */
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(plan->ctx);
    if (plan->chain_count > 1 && (!arch || arch->access_fault_granularity < LV_VECTOR_BYTES))
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_type_t *mask = plan->chain_count > 1 ? anvil_type_vector_mask(plan->ctx, plan->vector) : NULL;
    if (plan->chain_count > 1 && !mask)
        return ANVIL_PASS_RUN_ERROR;
    for (size_t block = 0; block + 1 < plan->chain_count; block++) {
        size_t position = body_index(plan, plan->chain[block]->last->operands[0]);
        const anvil_instr_t *compare = plan->instrs[position];
        anvil_op_t op;
        anvil_fcmp_pred_t pred;
        unsigned inverse = 0;
        if (!plan->exit_on_true[block]) {
            if (!anvil_opt_inverse_compare(plan->lane_op[position], compare->fcmp_pred, &op, &pred) ||
                !(inverse = anvil_vector_operation_cost(plan->ctx, op, plan->vector)))
                return ANVIL_PASS_RUN_UNCHANGED;
        }
        unsigned reduce = anvil_vector_operation_cost(plan->ctx, ANVIL_OP_REDUCE_OR, mask);
        if (!reduce)
            return ANVIL_PASS_RUN_UNCHANGED;
        plan->scalar_cost += LV_EXIT_COST;
        plan->vector_cost += inverse + reduce + 1 + LV_EXIT_COST;
    }
    return ANVIL_PASS_RUN_CHANGED;
}

/* The trip count when start and limit are constants. */
static bool constant_trips(const lv_plan *plan, uint64_t *trips)
{
    if (plan->start->kind != ANVIL_VAL_CONST_INT || plan->limit->kind != ANVIL_VAL_CONST_INT)
        return false;
    int64_t start = plan->start->data.i, limit = plan->limit->data.i;
    *trips = limit > start ? (uint64_t)(limit - start) : 0;
    return true;
}

/* Whether the vector loop, its setup and the scalar remainder cost less
 * than the scalar loop alone for the (known or assumed) trip count. */
static bool profitable(const lv_plan *plan)
{
    uint64_t trips;
    if (!constant_trips(plan, &trips))
        trips = LV_ASSUMED_TRIPS;
    if (trips < plan->lanes)
        return false;

    uint64_t setup = LV_SETUP_COST + plan->splat_cost;
    for (size_t a = 0; a < plan->num_streams; a++) {
        for (size_t b = a + 1; b < plan->num_streams; b++)
            setup += needs_overlap_check(plan, a, b) ? LV_CHECK_COST : 0;
        setup += plan->chain_count > 1 ? LV_CHECK_COST : 0;  /* alignment */
    }
    uint64_t vector_total = setup + trips / plan->lanes * plan->vector_cost + trips % plan->lanes * plan->scalar_cost;
    return vector_total < trips * plan->scalar_cost;
}

/* ------------------------------------------------------------------------
 * Construction.  Everything is created detached first; nothing live changes
 * until every allocation has succeeded.
 * ------------------------------------------------------------------------ */

typedef struct {
    anvil_instr_t *items[LV_BODY_LIMIT * 4 + 64];
    size_t count;
} lv_list;

static anvil_instr_t *make(lv_plan *plan, lv_list *list, anvil_op_t op, anvil_type_t *type, anvil_value_t *const *operands, size_t count)
{
    if (list->count == sizeof(list->items) / sizeof(list->items[0]))
        return NULL;
    anvil_instr_t *instr = anvil_instr_create(plan->ctx, op, type ? type : plan->ctx->type_void, type ? "vec" : NULL);
    if (!instr || (count && !anvil_instr_add_operands(instr, operands, count)))
        return NULL;
    list->items[list->count++] = instr;
    return instr;
}

static anvil_value_t *index_const(lv_plan *plan, int64_t value)
{
    return anvil_opt_make_const_int(plan->ctx, plan->phi->result->type, value);
}

static anvil_instr_t *make_phi(lv_plan *plan, anvil_type_t *type)
{
    anvil_instr_t *phi = anvil_instr_create(plan->ctx, ANVIL_OP_PHI, type, "vec.index");
    if (!phi || !anvil_instr_reserve_operands(phi, 2))
        return NULL;
    phi->phi_blocks = anvil_ctx_calloc(plan->ctx, 2, sizeof(*phi->phi_blocks));
    if (!phi->phi_blocks)
        return NULL;
    phi->phi_capacity = 2;
    return phi;
}

/* A stream's address on the first iteration, as an integer. */
static anvil_value_t *first_address(lv_plan *plan, lv_list *list, const anvil_instr_t *gep, anvil_type_t *address_type)
{
    anvil_value_t *operands[LV_GEP_OPERANDS];
    size_t last = gep->num_operands - 1;
    for (size_t operand = 0; operand < last; operand++)
        operands[operand] = gep->operands[operand];
    operands[last] = plan->start;
    anvil_value_t *index = gep->operands[last];
    if (index != plan->phi->result) {
        /* An extended loop index: extend the start the same way. */
        const anvil_instr_t *extend = index->data.instr;
        anvil_instr_t *start = make(plan, list, extend->op, index->type, &plan->start, 1);
        if (!start)
            return NULL;
        operands[last] = start->result;
    }
    anvil_instr_t *address = make(plan, list, ANVIL_OP_GEP, gep->result->type, operands, gep->num_operands);
    if (!address || !anvil_instr_copy_attributes(address, gep))
        return NULL;
    anvil_instr_t *integer = make(plan, list, ANVIL_OP_PTRTOINT, address_type, &address->result, 1);
    return integer ? integer->result : NULL;
}

/* `safe` and `condition` (just `condition` when `safe` is NULL). */
static anvil_value_t *all_of(lv_plan *plan, lv_list *list, anvil_value_t *safe, anvil_value_t *condition)
{
    if (!safe)
        return condition;
    anvil_value_t *operands[] = {safe, condition};
    anvil_instr_t *both = make(plan, list, ANVIL_OP_AND, plan->ctx->type_i1, operands, 2);
    return both ? both->result : NULL;
}

/* The end of the vector iterations, computed in the preheader:
 * n - ((n - start) mod lanes), or start when the trip is empty or two bases
 * stored through overlap within one vector. */
static anvil_value_t *vector_end(lv_plan *plan, lv_list *list)
{
    anvil_ctx_t *ctx = plan->ctx;
    anvil_type_t *index_type = plan->phi->result->type;
    anvil_value_t *span_ops[] = {plan->limit, plan->start};
    anvil_instr_t *span = make(plan, list, ANVIL_OP_SUB, index_type, span_ops, 2);
    anvil_value_t *rest_ops[] = {span ? span->result : NULL, index_const(plan, (int64_t)plan->lanes - 1)};
    anvil_instr_t *rest = span ? make(plan, list, ANVIL_OP_AND, index_type, rest_ops, 2) : NULL;
    anvil_value_t *end_ops[] = {plan->limit, rest ? rest->result : NULL};
    anvil_instr_t *end = rest ? make(plan, list, ANVIL_OP_SUB, index_type, end_ops, 2) : NULL;
    if (!end)
        return NULL;

    anvil_value_t *safe = NULL;
    if (plan->bounded) {
        bool is_unsigned = plan->compare->op == ANVIL_OP_CMP_ULT || plan->compare->op == ANVIL_OP_CMP_UGT;
        anvil_value_t *ops[] = {plan->start, plan->limit};
        anvil_instr_t *nonempty = make(plan, list, is_unsigned ? ANVIL_OP_CMP_ULT : ANVIL_OP_CMP_LT, ctx->type_i1, ops, 2);
        if (!nonempty)
            return NULL;
        safe = nonempty->result;
    }

    size_t pointer_size = anvil_type_size(plan->streams[0]->result->type);
    anvil_type_t *address_type = pointer_size == 8 ? ctx->type_i64 : ctx->type_i32;
    int64_t reach = (int64_t)(plan->lanes * plan->element->size);
    anvil_value_t *addresses[LV_STREAM_LIMIT] = {0};
    for (size_t a = 0; a < plan->num_streams; a++) {
        for (size_t b = a + 1; b < plan->num_streams; b++) {
            if (!needs_overlap_check(plan, a, b))
                continue;
            for (size_t side = 0; side < 2; side++) {
                size_t stream = side ? b : a;
                if (!addresses[stream] && !(addresses[stream] = first_address(plan, list, plan->streams[stream], address_type)))
                    return NULL;
            }
            anvil_value_t *far = pointer_size == 8 ? anvil_const_i64(ctx, reach) : anvil_const_i32(ctx, (int32_t)reach);
            anvil_value_t *near = pointer_size == 8 ? anvil_const_i64(ctx, -reach) : anvil_const_i32(ctx, (int32_t)-reach);
            anvil_value_t *zero = pointer_size == 8 ? anvil_const_i64(ctx, 0) : anvil_const_i32(ctx, 0);
            anvil_value_t *distance_ops[] = {addresses[b], addresses[a]};
            anvil_instr_t *distance = make(plan, list, ANVIL_OP_SUB, address_type, distance_ops, 2);
            if (!distance)
                return NULL;
            anvil_value_t *same_ops[] = {distance->result, zero}, *above_ops[] = {distance->result, far}, *below_ops[] = {distance->result, near};
            anvil_instr_t *same = make(plan, list, ANVIL_OP_CMP_EQ, ctx->type_i1, same_ops, 2);
            anvil_instr_t *above = make(plan, list, ANVIL_OP_CMP_GE, ctx->type_i1, above_ops, 2);
            anvil_instr_t *below = make(plan, list, ANVIL_OP_CMP_LE, ctx->type_i1, below_ops, 2);
            if (!same || !above || !below)
                return NULL;
            anvil_value_t *apart_ops[] = {above->result, below->result};
            anvil_instr_t *apart = make(plan, list, ANVIL_OP_OR, ctx->type_i1, apart_ops, 2);
            anvil_value_t *ok_ops[] = {same->result, apart ? apart->result : NULL};
            anvil_instr_t *ok = apart ? make(plan, list, ANVIL_OP_OR, ctx->type_i1, ok_ops, 2) : NULL;
            if (!ok || !(safe = all_of(plan, list, safe, ok->result)))
                return NULL;
        }
    }

    /* Early exits read lanes past the one that leaves: only aligned
     * vectors, which stay within a protection block holding a byte the
     * scalar loop reads (see vector_costs). */
    for (size_t stream = 0; plan->chain_count > 1 && stream < plan->num_streams; stream++) {
        if (!addresses[stream] && !(addresses[stream] = first_address(plan, list, plan->streams[stream], address_type)))
            return NULL;
        anvil_value_t *low_ops[] = {addresses[stream], pointer_size == 8 ? anvil_const_i64(ctx, LV_VECTOR_BYTES - 1)
                                                                         : anvil_const_i32(ctx, LV_VECTOR_BYTES - 1)};
        anvil_instr_t *low = make(plan, list, ANVIL_OP_AND, address_type, low_ops, 2);
        anvil_value_t *aligned_ops[] = {low ? low->result : NULL, pointer_size == 8 ? anvil_const_i64(ctx, 0) : anvil_const_i32(ctx, 0)};
        anvil_instr_t *aligned = low ? make(plan, list, ANVIL_OP_CMP_EQ, ctx->type_i1, aligned_ops, 2) : NULL;
        if (!aligned || !(safe = all_of(plan, list, safe, aligned->result)))
            return NULL;
    }
    if (!safe)
        return end->result;
    anvil_value_t *select_ops[] = {safe, end->result, plan->start};
    anvil_instr_t *select = make(plan, list, ANVIL_OP_SELECT, index_type, select_ops, 3);
    return select ? select->result : NULL;
}

typedef struct {
    const anvil_value_t *scalar[LV_BODY_LIMIT * 2];
    anvil_value_t *vector[LV_BODY_LIMIT * 2];
    size_t count;
} lv_map;

static anvil_value_t *map_get(const lv_map *map, const anvil_value_t *scalar)
{
    for (size_t index = 0; index < map->count; index++) {
        if (map->scalar[index] == scalar)
            return map->vector[index];
    }
    return NULL;
}

static bool map_put(lv_map *map, const anvil_value_t *scalar, anvil_value_t *vector)
{
    if (map->count == sizeof(map->scalar) / sizeof(map->scalar[0]))
        return false;
    map->scalar[map->count] = scalar;
    map->vector[map->count++] = vector;
    return true;
}

/* The vector form of a lane operand; invariants are splat once in the
 * preheader. */
static anvil_value_t *lane_value(lv_plan *plan, lv_map *map, lv_list *preheader, const anvil_value_t *scalar)
{
    anvil_value_t *mapped = map_get(map, scalar);
    if (mapped || in_loop(plan, scalar))
        return mapped;
    anvil_value_t *operand = (anvil_value_t *)scalar;
    anvil_instr_t *splat = make(plan, preheader, ANVIL_OP_SPLAT, plan->vector, &operand, 1);
    return splat && map_put(map, scalar, splat->result) ? splat->result : NULL;
}

/* The lane form of an operand of a widened operation: widened lane data
 * is already in the lanes; an invariant is truncated to the lane type
 * (constants at compile time) and broadcast. */
static anvil_value_t *narrow_value(lv_plan *plan, lv_map *map, lv_list *preheader, anvil_value_t *wide)
{
    anvil_value_t *mapped = map_get(map, wide);
    if (mapped || in_loop(plan, wide))
        return mapped;
    anvil_value_t *narrow;
    if (wide->kind == ANVIL_VAL_CONST_INT) {
        narrow = anvil_opt_make_const_int(plan->ctx, plan->element, wide->data.i);
    } else {
        anvil_instr_t *truncate = make(plan, preheader, ANVIL_OP_TRUNC, plan->element, &wide, 1);
        narrow = truncate ? truncate->result : NULL;
    }
    anvil_value_t *lanes = narrow ? lane_value(plan, map, preheader, narrow) : NULL;
    return lanes && map_put(map, wide, lanes) ? lanes : NULL;
}

/* The vector body: chain[j]'s work in bodies[j], and for each early exit
 * hits[j], true when any lane leaves (the scalar loop then finds which). */
/* The lane operands of comparison instrs[position]. */
static bool mask_operands(lv_plan *plan, lv_map *map, lv_list *preheader, size_t position, anvil_value_t **operands)
{
    const anvil_instr_t *instr = plan->instrs[position];
    for (size_t operand = 0; operand < 2; operand++)
        operands[operand] = plan->narrowed[position] ? narrow_value(plan, map, preheader, instr->operands[operand])
                                                     : lane_value(plan, map, preheader, instr->operands[operand]);
    return operands[0] && operands[1];
}

static bool build_body(lv_plan *plan, lv_list *bodies, lv_list *preheader, anvil_value_t *index, anvil_value_t **hits,
                       anvil_value_t *const *accumulators, anvil_value_t **updated)
{
    lv_map map = {0};
    anvil_type_t *vector_pointer = anvil_type_ptr(plan->ctx, plan->vector);
    if (!vector_pointer || !map_put(&map, plan->phi->result, index))
        return false;
    for (size_t reduction = 0; reduction < plan->num_reductions; reduction++) {
        if (!map_put(&map, plan->reductions[reduction].phi->result, accumulators[reduction]))
            return false;
    }

    for (size_t position = 0; position < plan->count; position++) {
        anvil_instr_t *instr = plan->instrs[position];
        lv_list *body = &bodies[plan->block_of[position]];
        anvil_value_t *operands[LV_GEP_OPERANDS] = {0};
        anvil_instr_t *made = NULL;
        switch (plan->classes[position]) {
        case LV_SKIP:
        case LV_BOUND:
            continue;
        case LV_INDEX:
        case LV_ADDRESS: {
            /* The same scalar computation on the vector index. */
            for (size_t operand = 0; operand < instr->num_operands; operand++) {
                anvil_value_t *mapped = map_get(&map, instr->operands[operand]);
                operands[operand] = mapped ? mapped : instr->operands[operand];
            }
            made = make(plan, body, instr->op, instr->result->type, operands, instr->num_operands);
            if (!made || !anvil_instr_copy_attributes(made, instr))
                return false;
            if (plan->classes[position] == LV_ADDRESS) {
                anvil_value_t *address = made->result;
                made = make(plan, body, ANVIL_OP_BITCAST, vector_pointer, &address, 1);
            }
            break;
        }
        case LV_WIDE:
        case LV_VECTOR:
            if (lane_identity(plan, position)) {
                anvil_value_t *same = map_get(&map, instr->operands[0]);
                if (!same || !map_put(&map, instr->result, same))
                    return false;
                continue;
            }
            if (plan->saturating[position]) {
                /* The clamped sum's lanes, saturated. */
                const anvil_instr_t *sum = plan->instrs[plan->clamp_source[position]];
                operands[0] = map_get(&map, sum->operands[0]);
                operands[1] = map_get(&map, sum->operands[1]);
                made = operands[0] && operands[1] ? make(plan, body, plan->lane_op[position], plan->vector, operands, 2) : NULL;
                break;
            }
            if (plan->classes[position] == LV_WIDE && instr->op == ANVIL_OP_SELECT) {
                operands[0] = map_get(&map, instr->operands[0]);
                operands[1] = narrow_value(plan, &map, preheader, instr->operands[1]);
                operands[2] = narrow_value(plan, &map, preheader, instr->operands[2]);
                made = operands[0] && operands[1] && operands[2] ? make(plan, body, ANVIL_OP_SELECT, plan->vector, operands, 3) : NULL;
                break;
            }
            if (plan->classes[position] == LV_WIDE) {
                operands[0] = narrow_value(plan, &map, preheader, instr->operands[0]);
                operands[1] = narrow_value(plan, &map, preheader, instr->operands[1]);
                made = operands[0] && operands[1] ? make(plan, body, instr->op, plan->vector, operands, 2) : NULL;
                break;
            }
            if (instr->op == ANVIL_OP_LOAD) {
                operands[0] = map_get(&map, instr->operands[0]);
                made = operands[0] ? make(plan, body, ANVIL_OP_LOAD, plan->vector, operands, 1) : NULL;
                break;
            }
            if (instr->op == ANVIL_OP_SELECT) {
                operands[0] = map_get(&map, instr->operands[0]);
                operands[1] = lane_value(plan, &map, preheader, instr->operands[1]);
                operands[2] = lane_value(plan, &map, preheader, instr->operands[2]);
                made = operands[0] && operands[1] && operands[2] ? make(plan, body, ANVIL_OP_SELECT, plan->vector, operands, 3) : NULL;
                break;
            }
            if (instr->op == ANVIL_OP_FMA || instr->op == ANVIL_OP_FSQRT) {
                bool mapped = true;
                for (size_t operand = 0; operand < instr->num_operands; operand++)
                    mapped = (operands[operand] = lane_value(plan, &map, preheader, instr->operands[operand])) && mapped;
                made = mapped ? make(plan, body, instr->op, plan->vector, operands, instr->num_operands) : NULL;
                break;
            }
            operands[0] = lane_value(plan, &map, preheader, instr->operands[0]);
            /* A uniform shift keeps its scalar count. */
            operands[1] = plan->uniform[position] ? instr->operands[1] : lane_value(plan, &map, preheader, instr->operands[1]);
            made = operands[0] && operands[1] ? make(plan, body, instr->op, plan->vector, operands, 2) : NULL;
            break;
        case LV_ACCUM: {
            const lv_reduction *reduction = reduction_of(plan, instr);
            operands[0] = map_get(&map, instr->operands[1 - reduction->lane_side]);
            operands[1] = map_get(&map, instr->operands[reduction->lane_side]);
            made = operands[0] && operands[1] ? make(plan, body, reduction->accumulate, reduction->vector, operands, 2) : NULL;
            break;
        }
        case LV_MASK: {
            anvil_type_t *mask = anvil_type_vector_mask(plan->ctx, plan->vector);
            made = mask && mask_operands(plan, &map, preheader, position, operands) ? make(plan, body, plan->lane_op[position], mask, operands, 2)
                                                                                     : NULL;
            if (made)
                made->fcmp_pred = instr->fcmp_pred;
            break;
        }
        case LV_STORE:
            operands[0] = lane_value(plan, &map, preheader, instr->operands[0]);
            operands[1] = map_get(&map, instr->operands[1]);
            made = operands[0] && operands[1] ? make(plan, body, ANVIL_OP_STORE, NULL, operands, 2) : NULL;
            break;
        }
        if (!made || (made->result && !map_put(&map, instr->result, made->result)))
            return false;
    }

    for (size_t reduction = 0; reduction < plan->num_reductions; reduction++) {
        if (!(updated[reduction] = map_get(&map, plan->reductions[reduction].update->result)))
            return false;
    }

    anvil_type_t *mask = plan->chain_count > 1 ? anvil_type_vector_mask(plan->ctx, plan->vector) : NULL;
    if (plan->chain_count > 1 && !mask)
        return false;
    for (size_t block = 0; block + 1 < plan->chain_count; block++) {
        lv_list *body = &bodies[block];
        size_t position = body_index(plan, plan->chain[block]->last->operands[0]);
        anvil_instr_t *compare = plan->instrs[position];
        anvil_value_t *leaving = map_get(&map, compare->result);
        if (!plan->exit_on_true[block]) {
            anvil_op_t op;
            anvil_fcmp_pred_t pred;
            anvil_value_t *operands[2];
            anvil_instr_t *inverse = mask_operands(plan, &map, preheader, position, operands) &&
                                             anvil_opt_inverse_compare(plan->lane_op[position], compare->fcmp_pred, &op, &pred)
                                         ? make(plan, body, op, mask, operands, 2)
                                         : NULL;
            if (!inverse)
                return false;
            inverse->fcmp_pred = pred;
            leaving = inverse->result;
        }
        anvil_instr_t *any = leaving ? make(plan, body, ANVIL_OP_REDUCE_OR, mask->data.vector.element, &leaving, 1) : NULL;
        anvil_value_t *test_ops[] = {any ? any->result : NULL, anvil_opt_make_const_int(plan->ctx, mask->data.vector.element, 0)};
        anvil_instr_t *hit = any && test_ops[1] ? make(plan, body, ANVIL_OP_CMP_NE, plan->ctx->type_i1, test_ops, 2) : NULL;
        if (!hit)
            return false;
        hits[block] = hit->result;
    }
    return true;
}

/* min/max start from the start value (any lane may hold it); the others
 * from the identity of their operation, combining the start value at the
 * end. */
static bool reduction_from_identity(const lv_reduction *reduction)
{
    return reduction->combine != ANVIL_OP_SELECT;
}

static anvil_value_t *accumulator_identity(lv_plan *plan, const lv_reduction *reduction)
{
    anvil_type_t *lane = anvil_type_vector_element(accumulator_vector(plan, reduction));
    switch (reduction->combine) {
    case ANVIL_OP_SELECT:
        return reduction->init;
    case ANVIL_OP_AND:
        return anvil_opt_make_const_int(plan->ctx, lane, -1);
    case ANVIL_OP_FADD:
        return anvil_opt_make_const_float(plan->ctx, lane, -0.0);
    default:
        return anvil_opt_make_const_int(plan->ctx, lane, 0);
    }
}

static void insert_block_after(anvil_func_t *func, anvil_block_t *anchor, anvil_block_t *block)
{
    block->next = anchor->next;
    anchor->next = block;
    if (func->last_block == anchor)
        func->last_block = block;
    func->num_blocks++;
}

/* Builds, in front of the scalar loop:
 *     vec.header: i = phi [start, preheader], [i + lanes, last vec.body]
 *                 br i != end, vec.body.0, vec.exit
 *     vec.body.j: chain[j]'s work on lanes i..i+lanes-1; at an early exit
 *                 br any lane leaves, vec.exit, vec.body.j+1
 *     vec.exit:   br header (the scalar loop, from i)
 * The scalar loop runs the remainder, and redoes the vector iteration in
 * which a lane left, taking the original exit. */
static anvil_pass_result_t vectorize_loop(lv_plan *plan)
{
    anvil_ctx_t *ctx = plan->ctx;
    size_t blocks = plan->chain_count;
    lv_list *setup = calloc(1, sizeof(*setup));
    lv_list *bodies = calloc(blocks, sizeof(*bodies));
    anvil_block_t *vector_bodies[LV_CHAIN_LIMIT] = {0};
    anvil_value_t *hits[LV_CHAIN_LIMIT] = {0};
    anvil_instr_t *accumulators[LV_REDUCTION_LIMIT] = {0};
    anvil_value_t *accumulator_values[LV_REDUCTION_LIMIT] = {0}, *identities[LV_REDUCTION_LIMIT] = {0};
    anvil_value_t *updated[LV_REDUCTION_LIMIT] = {0}, *finished[LV_REDUCTION_LIMIT] = {0};
    lv_list *tail = calloc(1, sizeof(*tail));
    anvil_pass_result_t result = ANVIL_PASS_RUN_ERROR;
    if (!setup || !bodies || !tail)
        goto done;

    /* Accumulators: a vector PHI from the identity (min/max: from the start
     * value), reduced and combined with the start value after the loop. */
    for (size_t reduction = 0; reduction < plan->num_reductions; reduction++) {
        const lv_reduction *r = &plan->reductions[reduction];
        anvil_type_t *vector = accumulator_vector(plan, r);
        anvil_value_t *identity = accumulator_identity(plan, r);
        anvil_instr_t *splat = identity ? make(plan, setup, ANVIL_OP_SPLAT, vector, &identity, 1) : NULL;
        if (!splat || !(accumulators[reduction] = make_phi(plan, vector)))
            goto done;
        identities[reduction] = splat->result;
        accumulator_values[reduction] = accumulators[reduction]->result;
    }

    anvil_value_t *end = vector_end(plan, setup);
    anvil_type_t *index_type = plan->phi->result->type;
    anvil_instr_t *index = make_phi(plan, index_type);
    anvil_block_t *vector_header = anvil_block_prepare(plan->func, "vec.header");
    anvil_block_t *vector_exit = anvil_block_prepare(plan->func, "vec.exit");
    if (!end || !index || !vector_header || !vector_exit ||
        !build_body(plan, bodies, setup, index->result, hits, accumulator_values, updated))
        goto done;
    for (size_t reduction = 0; reduction < plan->num_reductions; reduction++) {
        const lv_reduction *r = &plan->reductions[reduction];
        anvil_type_t *scalar = r->phi->result->type, *lane = anvil_type_vector_element(accumulator_vector(plan, r));
        anvil_instr_t *lanes = make(plan, tail, r->reduce, lane, &accumulator_values[reduction], 1);
        /* Lanes wider than the accumulator wrap to the same low bits. */
        if (lanes && !anvil_types_equal(lane, scalar))
            lanes = make(plan, tail, lane->size == scalar->size ? ANVIL_OP_BITCAST : ANVIL_OP_TRUNC, scalar, &lanes->result, 1);
        if (!lanes)
            goto done;
        finished[reduction] = lanes->result;
        if (!reduction_from_identity(r))
            continue;
        anvil_value_t *combine_ops[] = {r->init, lanes->result};
        anvil_instr_t *combined = make(plan, tail, r->combine, scalar, combine_ops, 2);
        if (!combined)
            goto done;
        finished[reduction] = combined->result;
    }
    for (size_t block = 0; block < blocks; block++) {
        if (!(vector_bodies[block] = anvil_block_prepare(plan->func, "vec.body")))
            goto done;
    }

    /* Terminators: early-exit tests, the latch's step and back edge, the
     * header's trip test and the exit's resumption of the scalar loop. */
    anvil_instr_t *exits[LV_CHAIN_LIMIT] = {0};
    for (size_t block = 0; block + 1 < blocks; block++) {
        if (!(exits[block] = make(plan, &bodies[block], ANVIL_OP_BR_COND, NULL, &hits[block], 1)))
            goto done;
        exits[block]->true_block = vector_exit;
        exits[block]->false_block = vector_bodies[block + 1];
    }
    lv_list *last = &bodies[blocks - 1];
    anvil_value_t *step_ops[] = {index->result, index_const(plan, (int64_t)plan->lanes)};
    anvil_instr_t *step = make(plan, last, ANVIL_OP_ADD, index_type, step_ops, 2);
    anvil_instr_t *back = make(plan, last, ANVIL_OP_BR, NULL, NULL, 0);
    anvil_value_t *more_ops[] = {index->result, end};
    anvil_instr_t *more = make(plan, setup, ANVIL_OP_CMP_NE, ctx->type_i1, more_ops, 2);
    anvil_instr_t *test = more ? make(plan, setup, ANVIL_OP_BR_COND, NULL, &more->result, 1) : NULL;
    anvil_instr_t *resume = make(plan, setup, ANVIL_OP_BR, NULL, NULL, 0);
    if (!step || !back || !test || !resume)
        goto done;
    back->true_block = vector_header;
    test->true_block = vector_bodies[0];
    test->false_block = vector_exit;
    resume->true_block = plan->header;

    /* Commit. */
    for (size_t item = 0; item < setup->count; item++) {
        anvil_instr_t *instr = setup->items[item];
        if (instr != more && instr != test && instr != resume)
            anvil_opt_insert_before(plan->preheader->last, instr);
    }
    plan->preheader->last->true_block = vector_header;

    index->operands[0] = plan->start;
    index->phi_blocks[0] = plan->preheader;
    index->operands[1] = step->result;
    index->phi_blocks[1] = vector_bodies[blocks - 1];
    index->num_operands = index->num_phi_incoming = 2;
    anvil_block_append_prepared(vector_header, index);
    for (size_t reduction = 0; reduction < plan->num_reductions; reduction++) {
        anvil_instr_t *phi = accumulators[reduction];
        phi->operands[0] = identities[reduction];
        phi->phi_blocks[0] = plan->preheader;
        phi->operands[1] = updated[reduction];
        phi->phi_blocks[1] = vector_bodies[blocks - 1];
        phi->num_operands = phi->num_phi_incoming = 2;
        anvil_block_append_prepared(vector_header, phi);
    }
    anvil_block_append_prepared(vector_header, more);
    anvil_block_append_prepared(vector_header, test);
    for (size_t block = 0; block < blocks; block++) {
        for (size_t item = 0; item < bodies[block].count; item++)
            anvil_block_append_prepared(vector_bodies[block], bodies[block].items[item]);
    }
    for (size_t item = 0; item < tail->count; item++)
        anvil_block_append_prepared(vector_exit, tail->items[item]);
    anvil_block_append_prepared(vector_exit, resume);

    for (size_t item = 0; item < 2; item++) {
        if (plan->phi->phi_blocks[item] == plan->preheader) {
            plan->phi->phi_blocks[item] = vector_exit;
            plan->phi->operands[item] = index->result;
        }
    }
    for (size_t reduction = 0; reduction < plan->num_reductions; reduction++) {
        anvil_instr_t *phi = plan->reductions[reduction].phi;
        for (size_t item = 0; item < 2; item++) {
            if (phi->phi_blocks[item] == plan->preheader) {
                phi->phi_blocks[item] = vector_exit;
                phi->operands[item] = finished[reduction];
            }
        }
    }

    anvil_block_t *anchor = plan->preheader;
    insert_block_after(plan->func, anchor, vector_header);
    anchor = vector_header;
    for (size_t block = 0; block < blocks; block++) {
        insert_block_after(plan->func, anchor, vector_bodies[block]);
        anchor = vector_bodies[block];
    }
    insert_block_after(plan->func, anchor, vector_exit);
    plan->header->vectorized = true;
    anvil_func_invalidate_cfg(plan->func);
    result = ANVIL_PASS_RUN_CHANGED;

done:
    free(setup);
    free(bodies);
    free(tail);
    return result;
}

anvil_pass_result_t anvil_pass_loop_vectorize(anvil_func_t *func)
{
    if (!func || !func->parent || !func->owner_ctx)
        return ANVIL_PASS_RUN_ERROR;
    const anvil_backend_t *backend = func->owner_ctx->backend;
    if (!backend || !backend->ops || !backend->ops->vector_operation_cost)
        return ANVIL_PASS_RUN_UNCHANGED;

    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (bool changed = true; changed;) {
        changed = false;
        anvil_opt_cfg_t cfg;
        anvil_loop_analysis_t loops;
        if (!anvil_opt_cfg_build(func, &cfg))
            return ANVIL_PASS_RUN_ERROR;
        if (!anvil_loop_analysis_build(&cfg, &loops)) {
            anvil_opt_cfg_destroy(&cfg);
            return ANVIL_PASS_RUN_ERROR;
        }
        for (size_t loop = 0; loop < loops.count && !changed; loop++) {
            lv_plan *plan = calloc(1, sizeof(*plan));
            if (!plan) {
                result = ANVIL_PASS_RUN_ERROR;
                break;
            }
            plan->func = func;
            plan->ctx = func->owner_ctx;
            anvil_pass_result_t status = ANVIL_PASS_RUN_UNCHANGED;
            if (match_loop(plan, &cfg, &loops, loop) && classify_body(plan) && match_reductions(plan) && plan->num_streams)
                status = vector_costs(plan);
            if (status == ANVIL_PASS_RUN_CHANGED)
                status = profitable(plan) ? vectorize_loop(plan) : ANVIL_PASS_RUN_UNCHANGED;
            if (status != ANVIL_PASS_RUN_UNCHANGED)
                result = status;
            changed = status == ANVIL_PASS_RUN_CHANGED;
            free(plan);
            if (result == ANVIL_PASS_RUN_ERROR)
                break;
        }
        anvil_loop_analysis_destroy(&loops);
        anvil_opt_cfg_destroy(&cfg);
        if (result == ANVIL_PASS_RUN_ERROR)
            return result;
    }
    return result;
}
