/* Monotone bit facts and unsigned bounds, propagated through sparse SSA uses. */
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

#include <stdlib.h>

typedef struct {
    uint64_t zero;
    uint64_t one;
} known_bits_t;

static uint64_t width_mask(unsigned width)
{
    return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
}

static known_bits_t constant_bits(uint64_t value, unsigned width)
{
    uint64_t mask = width_mask(width);
    return (known_bits_t){ .zero = ~value & mask, .one = value & mask };
}

static known_bits_t operand_bits(const anvil_def_use_t *uses, const known_bits_t *facts, const anvil_value_t *value)
{
    if (!anvil_type_is_integer(value->type))
        return (known_bits_t){ 0 };
    if (value->kind == ANVIL_VAL_CONST_INT)
        return constant_bits(value->data.u, anvil_type_bit_width(value->type));

    size_t definition = anvil_def_use_definition(uses, value);
    return definition == SIZE_MAX ? (known_bits_t){ 0 } : facts[definition];
}

static unsigned possible_bit(known_bits_t bits, uint64_t bit)
{
    if (bits.zero & bit)
        return 1;
    if (bits.one & bit)
        return 2;

    return 3;
}

static known_bits_t add_bits(known_bits_t left, known_bits_t right, unsigned width, bool subtract)
{
    known_bits_t result = { 0 };
    unsigned carry = subtract ? 2 : 1;
    if (subtract)
        right = (known_bits_t){ .zero = right.one, .one = right.zero };

    for (unsigned position = 0; position < width; position++)
    {
        uint64_t bit = UINT64_C(1) << position;
        unsigned left_set = possible_bit(left, bit);
        unsigned right_set = possible_bit(right, bit);
        unsigned sums = 0;
        unsigned next_carry = 0;
        for (unsigned a = 0; a < 2; a++)
        {
            for (unsigned b = 0; b < 2; b++)
            {
                for (unsigned c = 0; c < 2; c++)
                {
                    if (!(left_set & (1u << a)) || !(right_set & (1u << b)) || !(carry & (1u << c)))
                        continue;

                    unsigned sum = a + b + c;
                    sums |= 1u << (sum & 1);
                    next_carry |= 1u << (sum >> 1);
                }
            }
        }

        if (sums == 1)
            result.zero |= bit;
        else if (sums == 2)
            result.one |= bit;

        carry = next_carry;
    }

    return result;
}

static known_bits_t compare_bits(anvil_op_t op, known_bits_t left, known_bits_t right, unsigned width, bool identical)
{
    if (op == ANVIL_OP_CMP_LT || op == ANVIL_OP_CMP_LE || op == ANVIL_OP_CMP_GT || op == ANVIL_OP_CMP_GE)
    {
        /* Flipping the sign bit maps signed order to unsigned order, including
         * ranges that contain both signs, without host signed overflow. */
        uint64_t sign = UINT64_C(1) << (width - 1);
        left = (known_bits_t){ .zero = (left.zero & ~sign) | (left.one & sign), .one = (left.one & ~sign) | (left.zero & sign) };
        right = (known_bits_t){ .zero = (right.zero & ~sign) | (right.one & sign), .one = (right.one & ~sign) | (right.zero & sign) };
        switch (op)
        {
        case ANVIL_OP_CMP_LT:
            op = ANVIL_OP_CMP_ULT;
            break;
        case ANVIL_OP_CMP_LE:
            op = ANVIL_OP_CMP_ULE;
            break;
        case ANVIL_OP_CMP_GT:
            op = ANVIL_OP_CMP_UGT;
            break;
        case ANVIL_OP_CMP_GE:
            op = ANVIL_OP_CMP_UGE;
            break;
        default:
            break;
        }
    }

    uint64_t mask = width_mask(width);
    uint64_t minimum_left = left.one;
    uint64_t maximum_left = ~left.zero & mask;
    uint64_t minimum_right = right.one;
    uint64_t maximum_right = ~right.zero & mask;
    if (op == ANVIL_OP_CMP_EQ || op == ANVIL_OP_CMP_NE)
    {
        if ((left.one & right.zero) || (left.zero & right.one))
            return constant_bits(op == ANVIL_OP_CMP_NE, 1);
        if (identical || (minimum_left == maximum_left && minimum_right == maximum_right))
            return constant_bits((minimum_left == minimum_right) == (op == ANVIL_OP_CMP_EQ), 1);
    }

    if (op == ANVIL_OP_CMP_UGT || op == ANVIL_OP_CMP_UGE)
    {
        known_bits_t swap = left;
        left = right;
        right = swap;
        return compare_bits(op == ANVIL_OP_CMP_UGT ? ANVIL_OP_CMP_ULT : ANVIL_OP_CMP_ULE, left, right, width, identical);
    }

    if (op == ANVIL_OP_CMP_ULT)
    {
        if (maximum_left < minimum_right)
            return constant_bits(true, 1);
        if (identical || minimum_left >= maximum_right)
            return constant_bits(false, 1);
    }
    else if (op == ANVIL_OP_CMP_ULE)
    {
        if (identical || maximum_left <= minimum_right)
            return constant_bits(true, 1);
        if (minimum_left > maximum_right)
            return constant_bits(false, 1);
    }

    return (known_bits_t){ 0 };
}

static known_bits_t evaluate_bits(const anvil_def_use_t *uses, const known_bits_t *facts, const anvil_instr_t *instr)
{
    known_bits_t unknown = { 0 };
    if (!instr->result || !anvil_type_is_integer(instr->result->type))
        return unknown;

    unsigned width = anvil_type_bit_width(instr->result->type);
    uint64_t mask = width_mask(width);
    if (instr->op == ANVIL_OP_PHI || instr->op == ANVIL_OP_SELECT)
    {
        known_bits_t result = { .zero = mask, .one = mask };
        size_t first = instr->op == ANVIL_OP_SELECT ? 1 : 0;
        if (instr->num_operands <= first)
            return unknown;

        for (size_t operand = first; operand < instr->num_operands; operand++)
        {
            known_bits_t incoming = operand_bits(uses, facts, instr->operands[operand]);
            result.zero &= incoming.zero;
            result.one &= incoming.one;
        }

        return result;
    }

    if (!instr->num_operands)
        return unknown;

    known_bits_t left = operand_bits(uses, facts, instr->operands[0]);
    known_bits_t right = instr->num_operands > 1 ? operand_bits(uses, facts, instr->operands[1]) : unknown;
    switch (instr->op)
    {
        case ANVIL_OP_AND:
            return (known_bits_t){ .zero = left.zero | right.zero, .one = left.one & right.one };
        case ANVIL_OP_OR:
            return (known_bits_t){ .zero = left.zero & right.zero, .one = left.one | right.one };
        case ANVIL_OP_XOR:
            return (known_bits_t){ .zero = (left.zero & right.zero) | (left.one & right.one),
                                  .one = (left.zero & right.one) | (left.one & right.zero) };
        case ANVIL_OP_NOT:
            return (known_bits_t){ .zero = left.one, .one = left.zero };
        case ANVIL_OP_ADD:
        case ANVIL_OP_SUB:
            return add_bits(left, right, width, instr->op == ANVIL_OP_SUB);
        case ANVIL_OP_TRUNC:
            return (known_bits_t){ .zero = left.zero & mask, .one = left.one & mask };
        case ANVIL_OP_ZEXT:
        case ANVIL_OP_SEXT:
        {
            unsigned source_width = anvil_type_bit_width(instr->operands[0]->type);
            uint64_t upper = mask & ~width_mask(source_width);
            uint64_t sign = UINT64_C(1) << (source_width - 1);
            if (instr->op == ANVIL_OP_ZEXT || (left.zero & sign))
                left.zero |= upper;
            else if (left.one & sign)
                left.one |= upper;

            return left;
        }
        case ANVIL_OP_SHL:
        case ANVIL_OP_SHR:
        case ANVIL_OP_SAR:
        {
            unsigned shift_width = anvil_type_bit_width(instr->operands[1]->type);
            if ((right.zero | right.one) != width_mask(shift_width) || right.one >= width)
                return unknown;

            unsigned shift = (unsigned)right.one;
            if (!shift)
                return left;
            if (instr->op == ANVIL_OP_SHL)
                return (known_bits_t){ .zero = ((left.zero << shift) | width_mask(shift)) & mask, .one = (left.one << shift) & mask };

            uint64_t upper = mask & ~width_mask(width - shift);
            known_bits_t result = { .zero = left.zero >> shift, .one = left.one >> shift };
            if (instr->op == ANVIL_OP_SHR || (left.zero & (UINT64_C(1) << (width - 1))))
                result.zero |= upper;
            else if (left.one & (UINT64_C(1) << (width - 1)))
                result.one |= upper;

            return result;
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
            return compare_bits(instr->op, left, right, anvil_type_bit_width(instr->operands[0]->type), instr->operands[0] == instr->operands[1]);
        default:
            return unknown;
    }
}

anvil_pass_result_t anvil_pass_known_bits(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;

    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (func->is_declaration || !func->blocks)
        return ANVIL_PASS_RUN_UNCHANGED;

    anvil_def_use_t uses;
    if (!anvil_def_use_build(func, &uses))
        return ANVIL_PASS_RUN_ERROR;

    known_bits_t *facts = anvil_ctx_calloc(ctx, uses.count, sizeof(*facts));
    size_t *worklist = anvil_ctx_calloc(ctx, uses.count, sizeof(*worklist));
    bool *queued = anvil_ctx_calloc(ctx, uses.count, sizeof(*queued));
    if (!facts || !worklist || !queued)
    {
        free(facts);
        free(worklist);
        free(queued);
        anvil_def_use_destroy(&uses);
        return ANVIL_PASS_RUN_ERROR;
    }

    for (size_t index = 0; index < uses.count; index++)
    {
        worklist[index] = index;
        queued[index] = true;
    }

    size_t head = 0;
    size_t tail = 0;
    size_t pending = uses.count;
    while (pending)
    {
        size_t index = worklist[head];
        head = (head + 1) % uses.count;
        pending--;
        queued[index] = false;
        known_bits_t next = evaluate_bits(&uses, facts, uses.instructions[index]);
        if (next.zero == facts[index].zero && next.one == facts[index].one)
            continue;

        facts[index] = next;
        for (size_t use = uses.use_offsets[index]; use < uses.use_offsets[index + 1]; use++)
        {
            size_t user = uses.users[use];
            if (!queued[user])
            {
                queued[user] = true;
                worklist[tail] = user;
                tail = (tail + 1) % uses.count;
                pending++;
            }
        }
    }

    bool changed = false;
    for (size_t index = 0; index < uses.count; index++)
    {
        anvil_instr_t *instr = uses.instructions[index];
        if (!instr->result || !anvil_type_is_integer(instr->result->type))
            continue;

        unsigned width = anvil_type_bit_width(instr->result->type);
        if ((facts[index].zero | facts[index].one) != width_mask(width))
            continue;

        anvil_value_t *constant = width == 1 ? anvil_const_i1(ctx, facts[index].one != 0) :
                                  anvil_opt_make_const_int(ctx, instr->result->type, (int64_t)facts[index].one);
        if (!constant)
            break;

        for (size_t use = uses.use_offsets[index]; use < uses.use_offsets[index + 1]; use++)
        {
            anvil_instr_t *user = uses.instructions[uses.users[use]];
            for (size_t operand = 0; operand < user->num_operands; operand++)
            {
                if (user->operands[operand] == instr->result)
                    user->operands[operand] = constant;
            }
        }

        anvil_opt_erase_instr(instr);
        changed = true;
    }

    free(facts);
    free(worklist);
    free(queued);
    anvil_def_use_destroy(&uses);
    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;

    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
