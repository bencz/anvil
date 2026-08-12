#include "st_value.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#define ST_VALUE_PAYLOAD_MASK ((UINT64_C(1) << 61) - UINT64_C(1))
#define ST_VALUE_PAYLOAD_SIGN (UINT64_C(1) << 60)
#define ST_SPECIAL_NIL UINT64_C(0)
#define ST_SPECIAL_FALSE UINT64_C(1)
#define ST_SPECIAL_TRUE UINT64_C(2)

_Static_assert(sizeof(st_value_t) == 8, "StValue ABI requires 64 bits");
_Static_assert(sizeof(uintptr_t) == 8,
               "the current Smalltalk object ABI is 64-bit only");
_Static_assert(sizeof(st_object_header_t) == 8,
               "the object header must remain one word");
_Static_assert(_Alignof(st_object_header_t) >= 8,
               "the atomic object header must be eight-byte aligned");
_Static_assert((ST_HEADER_CLASS_MASK | ST_HEADER_SHAPE_MASK |
                ST_HEADER_AGE_MASK | ST_HEADER_COLOR_MASK |
                ST_HEADER_GENERATION_MASK | ST_HEADER_FLAGS_MASK) ==
                   UINT64_MAX,
               "header fields must cover exactly one word");
_Static_assert((ST_HEADER_CLASS_MASK &
                (ST_HEADER_SHAPE_MASK | ST_HEADER_AGE_MASK |
                 ST_HEADER_COLOR_MASK | ST_HEADER_GENERATION_MASK |
                 ST_HEADER_FLAGS_MASK)) == 0 &&
               (ST_HEADER_SHAPE_MASK &
                (ST_HEADER_AGE_MASK | ST_HEADER_COLOR_MASK |
                 ST_HEADER_GENERATION_MASK | ST_HEADER_FLAGS_MASK)) == 0 &&
               (ST_HEADER_AGE_MASK &
                (ST_HEADER_COLOR_MASK | ST_HEADER_GENERATION_MASK |
                 ST_HEADER_FLAGS_MASK)) == 0 &&
               (ST_HEADER_COLOR_MASK &
                (ST_HEADER_GENERATION_MASK | ST_HEADER_FLAGS_MASK)) == 0 &&
               (ST_HEADER_GENERATION_MASK & ST_HEADER_FLAGS_MASK) == 0,
               "header fields must be disjoint");
_Static_assert((ST_HEADER_CLASS_MASK >> ST_HEADER_CLASS_SHIFT) ==
                   ST_HEADER_CLASS_MAX &&
               (ST_HEADER_SHAPE_MASK >> ST_HEADER_SHAPE_SHIFT) ==
                   ST_HEADER_SHAPE_MAX &&
               (ST_HEADER_AGE_MASK >> ST_HEADER_AGE_SHIFT) ==
                   ST_HEADER_AGE_MAX,
               "public header maxima must match their masks");

static st_value_t special_value(uint64_t payload)
{
    return (payload << ST_VALUE_TAG_BITS) | ST_VALUE_TAG_SPECIAL;
}

static bool unicode_scalar_is_valid(uint32_t code_point)
{
    return code_point <= UINT32_C(0x10ffff) &&
           !(code_point >= UINT32_C(0xd800) &&
             code_point <= UINT32_C(0xdfff));
}

st_value_kind_t st_value_kind(st_value_t value)
{
    uint64_t tag = value & ST_VALUE_TAG_MASK;
    if (tag == ST_VALUE_TAG_OBJECT)
        return value == 0 ? ST_VALUE_INVALID : ST_VALUE_OBJECT;
    if (tag == ST_VALUE_TAG_SMALL_INTEGER)
        return ST_VALUE_SMALL_INTEGER;
    if (tag == ST_VALUE_TAG_CHARACTER) {
        uint64_t payload = value >> ST_VALUE_TAG_BITS;
        return payload <= UINT32_MAX && unicode_scalar_is_valid((uint32_t)payload)
            ? ST_VALUE_CHARACTER : ST_VALUE_INVALID;
    }
    if (tag == ST_VALUE_TAG_SPECIAL) {
        uint64_t payload = value >> ST_VALUE_TAG_BITS;
        if (payload == ST_SPECIAL_NIL) return ST_VALUE_NIL;
        if (payload == ST_SPECIAL_FALSE) return ST_VALUE_FALSE;
        if (payload == ST_SPECIAL_TRUE) return ST_VALUE_TRUE;
    }
    return ST_VALUE_INVALID;
}

bool st_value_has_valid_encoding(st_value_t value)
{
    return st_value_kind(value) != ST_VALUE_INVALID;
}

bool st_value_from_object(void *object, st_value_t *value_out)
{
    uintptr_t pointer;
    if (!value_out) return false;
    *value_out = 0;
    if (!object) return false;
    pointer = (uintptr_t)object;
    if ((pointer & ST_VALUE_TAG_MASK) != 0) return false;
    *value_out = (st_value_t)pointer;
    return true;
}

bool st_value_to_object_unchecked(st_value_t value, void **object_out)
{
    if (!object_out) return false;
    *object_out = NULL;
    if (st_value_kind(value) != ST_VALUE_OBJECT) return false;
    *object_out = (void *)(uintptr_t)value;
    return true;
}

bool st_value_from_small_integer(int64_t integer, st_value_t *value_out)
{
    if (!value_out) return false;
    *value_out = 0;
    if (integer < ST_SMALL_INTEGER_MIN || integer > ST_SMALL_INTEGER_MAX)
        return false;
    *value_out = ((uint64_t)integer << ST_VALUE_TAG_BITS) |
                 ST_VALUE_TAG_SMALL_INTEGER;
    return true;
}

bool st_value_to_small_integer(st_value_t value, int64_t *integer_out)
{
    uint64_t payload;
    uint64_t magnitude;
    if (!integer_out) return false;
    *integer_out = 0;
    if (st_value_kind(value) != ST_VALUE_SMALL_INTEGER) return false;
    payload = value >> ST_VALUE_TAG_BITS;
    if ((payload & ST_VALUE_PAYLOAD_SIGN) == 0) {
        *integer_out = (int64_t)payload;
        return true;
    }
    magnitude = ((~payload) + UINT64_C(1)) & ST_VALUE_PAYLOAD_MASK;
    *integer_out = -(int64_t)magnitude;
    return true;
}

static bool decode_pair(st_value_t lhs, st_value_t rhs,
                        int64_t *left_out, int64_t *right_out)
{
    return st_value_to_small_integer(lhs, left_out) &&
           st_value_to_small_integer(rhs, right_out);
}

bool st_small_integer_add(st_value_t lhs, st_value_t rhs,
                          st_value_t *result_out)
{
    int64_t left;
    int64_t right;
    if (!result_out) return false;
    *result_out = 0;
    if (!decode_pair(lhs, rhs, &left, &right)) return false;
    if ((right > 0 && left > ST_SMALL_INTEGER_MAX - right) ||
        (right < 0 && left < ST_SMALL_INTEGER_MIN - right)) return false;
    return st_value_from_small_integer(left + right, result_out);
}

bool st_small_integer_subtract(st_value_t lhs, st_value_t rhs,
                               st_value_t *result_out)
{
    int64_t left;
    int64_t right;
    if (!result_out) return false;
    *result_out = 0;
    if (!decode_pair(lhs, rhs, &left, &right)) return false;
    if ((right > 0 && left < ST_SMALL_INTEGER_MIN + right) ||
        (right < 0 && left > ST_SMALL_INTEGER_MAX + right)) return false;
    return st_value_from_small_integer(left - right, result_out);
}

bool st_small_integer_multiply(st_value_t lhs, st_value_t rhs,
                               st_value_t *result_out)
{
    int64_t left;
    int64_t right;
    if (!result_out) return false;
    *result_out = 0;
    if (!decode_pair(lhs, rhs, &left, &right)) return false;
    if (left != 0 && right != 0) {
        if (left > 0) {
            if ((right > 0 && left > ST_SMALL_INTEGER_MAX / right) ||
                (right < 0 && right < ST_SMALL_INTEGER_MIN / left))
                return false;
        } else {
            if ((right > 0 && left < ST_SMALL_INTEGER_MIN / right) ||
                (right < 0 && left < ST_SMALL_INTEGER_MAX / right))
                return false;
        }
    }
    return st_value_from_small_integer(left * right, result_out);
}

bool st_small_integer_negate(st_value_t operand, st_value_t *result_out)
{
    int64_t integer;
    if (!result_out) return false;
    *result_out = 0;
    if (!st_value_to_small_integer(operand, &integer) ||
        integer == ST_SMALL_INTEGER_MIN) return false;
    return st_value_from_small_integer(-integer, result_out);
}

bool st_value_from_character(uint32_t code_point, st_value_t *value_out)
{
    if (!value_out) return false;
    *value_out = 0;
    if (!unicode_scalar_is_valid(code_point)) return false;
    *value_out = ((uint64_t)code_point << ST_VALUE_TAG_BITS) |
                 ST_VALUE_TAG_CHARACTER;
    return true;
}

bool st_value_to_character(st_value_t value, uint32_t *code_point_out)
{
    if (!code_point_out) return false;
    *code_point_out = 0;
    if (st_value_kind(value) != ST_VALUE_CHARACTER) return false;
    *code_point_out = (uint32_t)(value >> ST_VALUE_TAG_BITS);
    return true;
}

st_value_t st_value_nil(void)
{
    return special_value(ST_SPECIAL_NIL);
}

st_value_t st_value_false(void)
{
    return special_value(ST_SPECIAL_FALSE);
}

st_value_t st_value_true(void)
{
    return special_value(ST_SPECIAL_TRUE);
}

bool st_value_to_boolean(st_value_t value, bool *boolean_out)
{
    if (!boolean_out) return false;
    *boolean_out = false;
    if (value == st_value_false()) return true;
    if (value == st_value_true()) {
        *boolean_out = true;
        return true;
    }
    return false;
}

bool st_object_header_pack(uint32_t class_id, uint32_t shape_id, uint8_t age,
                           st_gc_color_t color,
                           st_gc_generation_t generation,
                           st_header_flags_t flags, uint64_t *word_out)
{
    if (!word_out) return false;
    *word_out = 0;
    if (class_id == 0 || class_id > ST_HEADER_CLASS_MAX ||
        shape_id > ST_HEADER_SHAPE_MAX || age > ST_HEADER_AGE_MAX ||
        (int)color < 0 || color >= ST_GC_COLOR_INVALID ||
        (int)generation < 0 || generation > ST_GC_PERMANENT ||
        (flags & (st_header_flags_t)~ST_HEADER_PUBLIC_FLAGS) != 0)
        return false;
    *word_out = (uint64_t)class_id |
                ((uint64_t)shape_id << ST_HEADER_SHAPE_SHIFT) |
                ((uint64_t)age << ST_HEADER_AGE_SHIFT) |
                ((uint64_t)color << ST_HEADER_COLOR_SHIFT) |
                ((uint64_t)generation << ST_HEADER_GENERATION_SHIFT) |
                ((uint64_t)flags << ST_HEADER_FLAGS_SHIFT);
    return true;
}

bool st_object_header_word_is_valid(uint64_t word)
{
    return st_object_header_class_id(word) != 0 &&
           st_object_header_color(word) < ST_GC_COLOR_INVALID &&
           st_object_header_generation(word) <= ST_GC_PERMANENT &&
           (st_object_header_flags(word) & ST_HEADER_RESERVED_MASK) == 0;
}

bool st_object_header_is_supported(void)
{
    st_object_header_t probe;
    atomic_init(&probe.bits, 0);
    return atomic_is_lock_free(&probe.bits);
}

bool st_object_header_init(st_object_header_t *header, uint32_t class_id,
                           uint32_t shape_id, uint8_t age,
                           st_gc_color_t color,
                           st_gc_generation_t generation,
                           st_header_flags_t flags)
{
    uint64_t word;
    if (!header || !st_object_header_is_supported() ||
        !st_object_header_pack(class_id, shape_id, age, color,
                                           generation, flags, &word))
        return false;
    atomic_init(&header->bits, word);
    return true;
}

uint64_t st_object_header_load(const st_object_header_t *header)
{
    return header ? atomic_load_explicit(&header->bits, memory_order_acquire)
                  : 0;
}

uint32_t st_object_header_class_id(uint64_t word)
{
    return (uint32_t)((word & ST_HEADER_CLASS_MASK) >> ST_HEADER_CLASS_SHIFT);
}

uint32_t st_object_header_shape_id(uint64_t word)
{
    return (uint32_t)((word & ST_HEADER_SHAPE_MASK) >> ST_HEADER_SHAPE_SHIFT);
}

uint8_t st_object_header_age(uint64_t word)
{
    return (uint8_t)((word & ST_HEADER_AGE_MASK) >> ST_HEADER_AGE_SHIFT);
}

st_gc_color_t st_object_header_color(uint64_t word)
{
    return (st_gc_color_t)((word & ST_HEADER_COLOR_MASK) >>
                           ST_HEADER_COLOR_SHIFT);
}

st_gc_generation_t st_object_header_generation(uint64_t word)
{
    return (st_gc_generation_t)((word & ST_HEADER_GENERATION_MASK) >>
                                ST_HEADER_GENERATION_SHIFT);
}

st_header_flags_t st_object_header_flags(uint64_t word)
{
    return (st_header_flags_t)((word & ST_HEADER_FLAGS_MASK) >>
                               ST_HEADER_FLAGS_SHIFT);
}

static bool transition_color(st_object_header_t *header,
                             st_gc_color_t expected_color,
                             st_gc_color_t new_color)
{
    uint64_t current;
    uint64_t desired;
    if (!header) return false;
    current = atomic_load_explicit(&header->bits, memory_order_acquire);
    for (;;) {
        if (!st_object_header_word_is_valid(current) ||
            st_object_header_color(current) != expected_color) return false;
        desired = (current & ~ST_HEADER_COLOR_MASK) |
                  ((uint64_t)new_color << ST_HEADER_COLOR_SHIFT);
        if (atomic_compare_exchange_weak_explicit(
                &header->bits, &current, desired,
                memory_order_acq_rel, memory_order_acquire)) return true;
    }
}

bool st_object_header_try_mark_gray(st_object_header_t *header)
{
    return transition_color(header, ST_GC_WHITE, ST_GC_GRAY);
}

bool st_object_header_try_mark_black(st_object_header_t *header)
{
    return transition_color(header, ST_GC_GRAY, ST_GC_BLACK);
}

bool st_object_header_survive(st_object_header_t *header,
                              uint8_t promotion_age,
                              st_gc_generation_t *generation_out,
                              uint8_t *age_out)
{
    uint64_t current;
    uint64_t desired;
    uint8_t age;
    st_gc_generation_t generation;
    if (generation_out) *generation_out = ST_GC_NURSERY;
    if (age_out) *age_out = 0;
    if (!header || !generation_out || !age_out || promotion_age == 0 ||
        promotion_age > ST_HEADER_AGE_MAX) return false;
    current = atomic_load_explicit(&header->bits, memory_order_acquire);
    for (;;) {
        if (!st_object_header_word_is_valid(current)) return false;
        age = st_object_header_age(current);
        generation = st_object_header_generation(current);
        if (generation >= ST_GC_OLD) {
            *generation_out = generation;
            *age_out = age;
            return true;
        }
        age = age == ST_HEADER_AGE_MAX ? age : (uint8_t)(age + 1u);
        if (age >= promotion_age) {
            generation = (st_gc_generation_t)(generation + 1);
            age = 0;
        }
        desired = (current & ~(ST_HEADER_AGE_MASK |
                               ST_HEADER_GENERATION_MASK)) |
                  ((uint64_t)age << ST_HEADER_AGE_SHIFT) |
                  ((uint64_t)generation << ST_HEADER_GENERATION_SHIFT);
        if (atomic_compare_exchange_weak_explicit(
                &header->bits, &current, desired,
                memory_order_acq_rel, memory_order_acquire)) {
            *generation_out = generation;
            *age_out = age;
            return true;
        }
    }
}

bool st_object_header_remember(st_object_header_t *header)
{
    uint64_t current;
    if (!header) return false;
    current = atomic_load_explicit(&header->bits, memory_order_acquire);
    if (!st_object_header_word_is_valid(current)) return false;
    (void)atomic_fetch_or_explicit(
        &header->bits,
        (uint64_t)ST_HEADER_REMEMBERED << ST_HEADER_FLAGS_SHIFT,
        memory_order_release);
    return true;
}

bool st_object_header_is_lock_free(const st_object_header_t *header)
{
    return header && atomic_is_lock_free(&header->bits);
}
