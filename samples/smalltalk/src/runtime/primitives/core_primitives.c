#include "st_core_primitives.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPEC(name_, arity_, receiver_, failure_, id_)                       \
    {                                                                       \
        (name_), sizeof(name_) - 1u, (arity_), (receiver_), (failure_),     \
        ST_PRIMITIVE_INTRINSIC, (id_), NULL, 0u                             \
    }

static const st_primitive_spec_t core_specs[] = {
    SPEC("IdentityPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_CANNOT_FAIL, ST_INTRINSIC_IDENTITY),
    SPEC("IntEqualsPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_EQUALS),
    SPEC("IntNotEqualsPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_NOT_EQUALS),
    SPEC("IntLessThanPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_LESS_THAN),
    SPEC("IntLessEqualsPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_LESS_EQUALS),
    SPEC("IntGreaterThanPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_GREATER_THAN),
    SPEC("IntGreaterEqualsPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_GREATER_EQUALS),
    SPEC("IntAddPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_ADD),
    SPEC("IntSubPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_SUBTRACT),
    SPEC("IntMulPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_MULTIPLY),
    SPEC("IntFloorDivPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_FLOOR_DIVIDE),
    SPEC("IntModPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_MODULO),
    SPEC("IntNegPrimitive", 0u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_NEGATE),
    SPEC("IntAndPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_BIT_AND),
    SPEC("IntOrPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_BIT_OR),
    SPEC("IntXorPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_BIT_XOR),
    SPEC("IntShiftPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_INT_SHIFT),
    SPEC("CharacterNewPrimitive", 1u, ST_PRIMITIVE_CLASS_ONLY,
         ST_PRIMITIVE_FALL_THROUGH, ST_INTRINSIC_CHARACTER_NEW),
    SPEC("CharacterCodePrimitive", 0u, ST_PRIMITIVE_INSTANCE_ONLY,
         ST_PRIMITIVE_CANNOT_FAIL, ST_INTRINSIC_CHARACTER_CODE)
};

#undef SPEC

typedef st_core_primitive_status_t (*primitive_handler_t)(
    st_value_t receiver, const st_value_t *arguments,
    st_value_t *result_out);

typedef struct {
    uint32_t id;
    uint32_t arity;
    primitive_handler_t handler;
} primitive_definition_t;

static st_core_primitive_status_t decode_integer(st_value_t value,
                                                  int64_t *integer_out)
{
    if (!st_value_has_valid_encoding(value))
        return ST_CORE_PRIMITIVE_ERR_INVALID_VALUE;
    if (!st_value_to_small_integer(value, integer_out))
        return ST_CORE_PRIMITIVE_ERR_TYPE_MISMATCH;
    return ST_CORE_PRIMITIVE_OK;
}

static st_core_primitive_status_t decode_integer_pair(
    st_value_t receiver, const st_value_t *arguments,
    int64_t *left_out, int64_t *right_out)
{
    st_core_primitive_status_t status = decode_integer(receiver, left_out);
    if (status != ST_CORE_PRIMITIVE_OK) return status;
    return decode_integer(arguments[0], right_out);
}

static st_core_primitive_status_t encode_integer(int64_t integer,
                                                  st_value_t *result_out)
{
    if (!st_value_from_small_integer(integer, result_out))
        return ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED;
    return ST_CORE_PRIMITIVE_OK;
}

static st_core_primitive_status_t encode_boolean(bool value,
                                                  st_value_t *result_out)
{
    *result_out = value ? st_value_true() : st_value_false();
    return ST_CORE_PRIMITIVE_OK;
}

static st_core_primitive_status_t primitive_identity(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out)
{
    if (!st_value_has_valid_encoding(receiver) ||
        !st_value_has_valid_encoding(arguments[0]))
        return ST_CORE_PRIMITIVE_ERR_INVALID_VALUE;
    return encode_boolean(receiver == arguments[0], result_out);
}

typedef enum {
    COMPARE_EQUAL,
    COMPARE_NOT_EQUAL,
    COMPARE_LESS,
    COMPARE_LESS_EQUAL,
    COMPARE_GREATER,
    COMPARE_GREATER_EQUAL
} compare_kind_t;

static st_core_primitive_status_t compare_integers(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out,
    compare_kind_t kind)
{
    int64_t left;
    int64_t right;
    bool result;
    st_core_primitive_status_t status = decode_integer_pair(
        receiver, arguments, &left, &right);
    if (status != ST_CORE_PRIMITIVE_OK) return status;
    switch (kind) {
    case COMPARE_EQUAL: result = left == right; break;
    case COMPARE_NOT_EQUAL: result = left != right; break;
    case COMPARE_LESS: result = left < right; break;
    case COMPARE_LESS_EQUAL: result = left <= right; break;
    case COMPARE_GREATER: result = left > right; break;
    case COMPARE_GREATER_EQUAL: result = left >= right; break;
    default: return ST_CORE_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    return encode_boolean(result, result_out);
}

#define DEFINE_COMPARISON(function_, kind_)                                 \
    static st_core_primitive_status_t function_(                            \
        st_value_t receiver, const st_value_t *arguments,                   \
        st_value_t *result_out)                                             \
    {                                                                       \
        return compare_integers(receiver, arguments, result_out, (kind_));  \
    }

DEFINE_COMPARISON(primitive_int_equals, COMPARE_EQUAL)
DEFINE_COMPARISON(primitive_int_not_equals, COMPARE_NOT_EQUAL)
DEFINE_COMPARISON(primitive_int_less, COMPARE_LESS)
DEFINE_COMPARISON(primitive_int_less_equal, COMPARE_LESS_EQUAL)
DEFINE_COMPARISON(primitive_int_greater, COMPARE_GREATER)
DEFINE_COMPARISON(primitive_int_greater_equal, COMPARE_GREATER_EQUAL)

#undef DEFINE_COMPARISON

static st_core_primitive_status_t primitive_int_add(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out)
{
    int64_t left;
    int64_t right;
    st_core_primitive_status_t status = decode_integer_pair(
        receiver, arguments, &left, &right);
    if (status != ST_CORE_PRIMITIVE_OK) return status;
    if ((right > 0 && left > ST_SMALL_INTEGER_MAX - right) ||
        (right < 0 && left < ST_SMALL_INTEGER_MIN - right))
        return ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED;
    return encode_integer(left + right, result_out);
}

static st_core_primitive_status_t primitive_int_subtract(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out)
{
    int64_t left;
    int64_t right;
    st_core_primitive_status_t status = decode_integer_pair(
        receiver, arguments, &left, &right);
    if (status != ST_CORE_PRIMITIVE_OK) return status;
    if ((right > 0 && left < ST_SMALL_INTEGER_MIN + right) ||
        (right < 0 && left > ST_SMALL_INTEGER_MAX + right))
        return ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED;
    return encode_integer(left - right, result_out);
}

static st_core_primitive_status_t primitive_int_multiply(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out)
{
    int64_t left;
    int64_t right;
    st_core_primitive_status_t status = decode_integer_pair(
        receiver, arguments, &left, &right);
    if (status != ST_CORE_PRIMITIVE_OK) return status;
    if (left != 0 && right != 0) {
        if (left > 0) {
            if ((right > 0 && left > ST_SMALL_INTEGER_MAX / right) ||
                (right < 0 && right < ST_SMALL_INTEGER_MIN / left))
                return ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED;
        } else if ((right > 0 && left < ST_SMALL_INTEGER_MIN / right) ||
                   (right < 0 && left < ST_SMALL_INTEGER_MAX / right)) {
            return ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED;
        }
    }
    return encode_integer(left * right, result_out);
}

static st_core_primitive_status_t primitive_int_floor_divide(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out)
{
    int64_t left;
    int64_t right;
    int64_t quotient;
    int64_t remainder;
    st_core_primitive_status_t status = decode_integer_pair(
        receiver, arguments, &left, &right);
    if (status != ST_CORE_PRIMITIVE_OK) return status;
    if (right == 0) return ST_CORE_PRIMITIVE_ERR_DIVISION_BY_ZERO;
    quotient = left / right;
    remainder = left % right;
    if (remainder != 0 && ((remainder < 0) != (right < 0))) --quotient;
    return encode_integer(quotient, result_out);
}

static st_core_primitive_status_t primitive_int_modulo(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out)
{
    int64_t left;
    int64_t right;
    int64_t remainder;
    st_core_primitive_status_t status = decode_integer_pair(
        receiver, arguments, &left, &right);
    if (status != ST_CORE_PRIMITIVE_OK) return status;
    if (right == 0) return ST_CORE_PRIMITIVE_ERR_DIVISION_BY_ZERO;
    remainder = left % right;
    if (remainder != 0 && ((remainder < 0) != (right < 0)))
        remainder += right;
    return encode_integer(remainder, result_out);
}

static st_core_primitive_status_t primitive_int_negate(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out)
{
    int64_t integer;
    st_core_primitive_status_t status;
    (void)arguments;
    status = decode_integer(receiver, &integer);
    if (status != ST_CORE_PRIMITIVE_OK) return status;
    if (integer == ST_SMALL_INTEGER_MIN)
        return ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED;
    return encode_integer(-integer, result_out);
}

static int64_t signed_from_twos_complement(uint64_t bits)
{
    if (bits <= (uint64_t)INT64_MAX) return (int64_t)bits;
    return -(int64_t)((~bits) + UINT64_C(1));
}

typedef enum {
    BIT_AND,
    BIT_OR,
    BIT_XOR
} bit_operation_t;

static st_core_primitive_status_t bit_operation(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out,
    bit_operation_t operation)
{
    int64_t left;
    int64_t right;
    uint64_t bits;
    st_core_primitive_status_t status = decode_integer_pair(
        receiver, arguments, &left, &right);
    if (status != ST_CORE_PRIMITIVE_OK) return status;
    if (operation == BIT_AND)
        bits = (uint64_t)left & (uint64_t)right;
    else if (operation == BIT_OR)
        bits = (uint64_t)left | (uint64_t)right;
    else
        bits = (uint64_t)left ^ (uint64_t)right;
    return encode_integer(signed_from_twos_complement(bits), result_out);
}

#define DEFINE_BIT_OPERATION(function_, operation_)                         \
    static st_core_primitive_status_t function_(                            \
        st_value_t receiver, const st_value_t *arguments,                   \
        st_value_t *result_out)                                             \
    {                                                                       \
        return bit_operation(receiver, arguments, result_out, (operation_));\
    }

DEFINE_BIT_OPERATION(primitive_int_and, BIT_AND)
DEFINE_BIT_OPERATION(primitive_int_or, BIT_OR)
DEFINE_BIT_OPERATION(primitive_int_xor, BIT_XOR)

#undef DEFINE_BIT_OPERATION

static st_core_primitive_status_t primitive_int_shift(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out)
{
    int64_t integer;
    int64_t signed_count;
    st_core_primitive_status_t status = decode_integer_pair(
        receiver, arguments, &integer, &signed_count);
    if (status != ST_CORE_PRIMITIVE_OK) return status;
    if (signed_count >= 0) {
        uint64_t count = (uint64_t)signed_count;
        int64_t factor;
        if (integer == 0) return encode_integer(0, result_out);
        if (count >= 61u)
            return ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED;
        factor = (int64_t)(UINT64_C(1) << (unsigned)count);
        if (integer > ST_SMALL_INTEGER_MAX / factor ||
            integer < ST_SMALL_INTEGER_MIN / factor)
            return ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED;
        return encode_integer(integer * factor, result_out);
    } else {
        uint64_t count = (uint64_t)(-signed_count);
        int64_t shifted;
        if (count >= 61u)
            shifted = integer < 0 ? -1 : 0;
        else if (integer >= 0)
            shifted = (int64_t)((uint64_t)integer >> (unsigned)count);
        else {
            uint64_t magnitude = (uint64_t)(-integer);
            uint64_t mask = (UINT64_C(1) << (unsigned)count) - UINT64_C(1);
            shifted = -(int64_t)((magnitude + mask) >> (unsigned)count);
        }
        return encode_integer(shifted, result_out);
    }
}

static st_core_primitive_status_t primitive_character_new(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out)
{
    int64_t code_point;
    st_core_primitive_status_t status;
    if (!st_value_has_valid_encoding(receiver))
        return ST_CORE_PRIMITIVE_ERR_INVALID_VALUE;
    status = decode_integer(arguments[0], &code_point);
    if (status != ST_CORE_PRIMITIVE_OK) return status;
    if (code_point < 0 || code_point > UINT32_C(0x10ffff) ||
        (code_point >= UINT32_C(0xd800) && code_point <= UINT32_C(0xdfff)))
        return ST_CORE_PRIMITIVE_ERR_INVALID_CODE_POINT;
    if (!st_value_from_character((uint32_t)code_point, result_out))
        return ST_CORE_PRIMITIVE_ERR_INVALID_CODE_POINT;
    return ST_CORE_PRIMITIVE_OK;
}

static st_core_primitive_status_t primitive_character_code(
    st_value_t receiver, const st_value_t *arguments, st_value_t *result_out)
{
    uint32_t code_point;
    (void)arguments;
    if (!st_value_has_valid_encoding(receiver))
        return ST_CORE_PRIMITIVE_ERR_INVALID_VALUE;
    if (!st_value_to_character(receiver, &code_point))
        return ST_CORE_PRIMITIVE_ERR_TYPE_MISMATCH;
    return encode_integer((int64_t)code_point, result_out);
}

static const primitive_definition_t definitions[] = {
    { ST_INTRINSIC_IDENTITY, 1u, primitive_identity },
    { ST_INTRINSIC_INT_EQUALS, 1u, primitive_int_equals },
    { ST_INTRINSIC_INT_NOT_EQUALS, 1u, primitive_int_not_equals },
    { ST_INTRINSIC_INT_LESS_THAN, 1u, primitive_int_less },
    { ST_INTRINSIC_INT_LESS_EQUALS, 1u, primitive_int_less_equal },
    { ST_INTRINSIC_INT_GREATER_THAN, 1u, primitive_int_greater },
    { ST_INTRINSIC_INT_GREATER_EQUALS, 1u, primitive_int_greater_equal },
    { ST_INTRINSIC_INT_ADD, 1u, primitive_int_add },
    { ST_INTRINSIC_INT_SUBTRACT, 1u, primitive_int_subtract },
    { ST_INTRINSIC_INT_MULTIPLY, 1u, primitive_int_multiply },
    { ST_INTRINSIC_INT_FLOOR_DIVIDE, 1u, primitive_int_floor_divide },
    { ST_INTRINSIC_INT_MODULO, 1u, primitive_int_modulo },
    { ST_INTRINSIC_INT_NEGATE, 0u, primitive_int_negate },
    { ST_INTRINSIC_INT_BIT_AND, 1u, primitive_int_and },
    { ST_INTRINSIC_INT_BIT_OR, 1u, primitive_int_or },
    { ST_INTRINSIC_INT_BIT_XOR, 1u, primitive_int_xor },
    { ST_INTRINSIC_INT_SHIFT, 1u, primitive_int_shift },
    { ST_INTRINSIC_CHARACTER_NEW, 1u, primitive_character_new },
    { ST_INTRINSIC_CHARACTER_CODE, 0u, primitive_character_code }
};

_Static_assert(sizeof(core_specs) / sizeof(core_specs[0]) ==
                   sizeof(definitions) / sizeof(definitions[0]),
               "every core primitive spec must have one handler");

st_core_primitive_status_t st_core_primitive_execute(
    uint32_t intrinsic_id, st_value_t receiver,
    const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out)
{
    size_t index;
    if (!result_out) return ST_CORE_PRIMITIVE_ERR_INVALID_ARGUMENT;
    *result_out = 0;
    for (index = 0u; index < sizeof(definitions) / sizeof(definitions[0]);
         ++index) {
        const primitive_definition_t *definition = &definitions[index];
        if (definition->id != intrinsic_id) continue;
        if (argument_count != definition->arity)
            return ST_CORE_PRIMITIVE_ERR_WRONG_ARITY;
        if (argument_count != 0u && !arguments)
            return ST_CORE_PRIMITIVE_ERR_INVALID_ARGUMENT;
        return definition->handler(receiver, arguments, result_out);
    }
    return ST_CORE_PRIMITIVE_ERR_UNKNOWN_INTRINSIC;
}

const st_primitive_spec_t *st_core_primitive_specs(size_t *count_out)
{
    if (count_out)
        *count_out = sizeof(core_specs) / sizeof(core_specs[0]);
    return core_specs;
}

const char *st_core_primitive_status_string(st_core_primitive_status_t status)
{
    switch (status) {
    case ST_CORE_PRIMITIVE_OK: return "ok";
    case ST_CORE_PRIMITIVE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_CORE_PRIMITIVE_ERR_UNKNOWN_INTRINSIC: return "unknown intrinsic";
    case ST_CORE_PRIMITIVE_ERR_WRONG_ARITY: return "wrong arity";
    case ST_CORE_PRIMITIVE_ERR_INVALID_VALUE: return "invalid StValue";
    case ST_CORE_PRIMITIVE_ERR_TYPE_MISMATCH: return "type mismatch";
    case ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED:
        return "integer promotion required";
    case ST_CORE_PRIMITIVE_ERR_DIVISION_BY_ZERO: return "division by zero";
    case ST_CORE_PRIMITIVE_ERR_INVALID_CODE_POINT:
        return "invalid Unicode code point";
    default: return "unknown core primitive status";
    }
}
