#ifndef ANVIL_SMALLTALK_VALUE_H
#define ANVIL_SMALLTALK_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/*
 * Portable 64-bit value representation.
 *
 * Heap objects are at least eight-byte aligned, leaving the low three bits
 * available as an immediate tag.  Zero is deliberately not an object: it is
 * the invalid/uninitialized value, while Smalltalk nil is an explicit special
 * immediate.  The representation is part of the Smalltalk runtime ABI.
 */
typedef uint64_t st_value_t;

enum {
    ST_VALUE_TAG_BITS = 3,
    ST_VALUE_TAG_MASK = 0x7u,
    ST_VALUE_TAG_OBJECT = 0x0u,
    ST_VALUE_TAG_SMALL_INTEGER = 0x1u,
    ST_VALUE_TAG_CHARACTER = 0x2u,
    ST_VALUE_TAG_SPECIAL = 0x3u
};

#define ST_SMALL_INTEGER_MIN (-(INT64_C(1) << 60))
#define ST_SMALL_INTEGER_MAX ((INT64_C(1) << 60) - INT64_C(1))

typedef enum {
    ST_VALUE_INVALID,
    ST_VALUE_OBJECT,
    ST_VALUE_SMALL_INTEGER,
    ST_VALUE_CHARACTER,
    ST_VALUE_NIL,
    ST_VALUE_FALSE,
    ST_VALUE_TRUE
} st_value_kind_t;

/* This validates only the tagged-word encoding. For object values it does not
 * prove heap membership, allocation liveness, or safe dereference; the heap
 * validator owns those checks. */
st_value_kind_t st_value_kind(st_value_t value);
bool st_value_has_valid_encoding(st_value_t value);

bool st_value_from_object(void *object, st_value_t *value_out);
/* Decodes the pointer bits after checking only the tag/nonzero encoding. The
 * caller must already have proved heap membership and allocation liveness. */
bool st_value_to_object_unchecked(st_value_t value, void **object_out);

bool st_value_from_small_integer(int64_t integer, st_value_t *value_out);
bool st_value_to_small_integer(st_value_t value, int64_t *integer_out);
bool st_small_integer_add(st_value_t lhs, st_value_t rhs,
                          st_value_t *result_out);
bool st_small_integer_subtract(st_value_t lhs, st_value_t rhs,
                               st_value_t *result_out);
bool st_small_integer_multiply(st_value_t lhs, st_value_t rhs,
                               st_value_t *result_out);
bool st_small_integer_negate(st_value_t operand, st_value_t *result_out);

bool st_value_from_character(uint32_t code_point, st_value_t *value_out);
bool st_value_to_character(st_value_t value, uint32_t *code_point_out);

st_value_t st_value_nil(void);
st_value_t st_value_false(void);
st_value_t st_value_true(void);
bool st_value_to_boolean(st_value_t value, bool *boolean_out);

/*
 * Every object begins with one atomic 64-bit header word.  IDs use dense
 * integer domains so equality and cache probes are integer operations.  GC
 * state and common object properties occupy the high bits and can be updated
 * with masks without disturbing class or shape identity. Shape transitions
 * are intentionally implemented at the descriptor/heap layer, where layout
 * compatibility and allocation extent can be proved.
 *
 *   63       56 55 54 53 52 51       48 47                24 23         0
 *  +-----------+-----+-----+-----------+---------------------+------------+
 *  |   flags   | gen |color|    age    |      shape_id       | class_id   |
 *  +-----------+-----+-----+-----------+---------------------+------------+
 */
enum {
    ST_HEADER_CLASS_BITS = 24,
    ST_HEADER_SHAPE_BITS = 24,
    ST_HEADER_AGE_BITS = 4,
    ST_HEADER_COLOR_BITS = 2,
    ST_HEADER_GENERATION_BITS = 2,
    ST_HEADER_CLASS_SHIFT = 0,
    ST_HEADER_SHAPE_SHIFT = 24,
    ST_HEADER_AGE_SHIFT = 48,
    ST_HEADER_COLOR_SHIFT = 52,
    ST_HEADER_GENERATION_SHIFT = 54,
    ST_HEADER_FLAGS_SHIFT = 56
};

#define ST_HEADER_CLASS_MASK \
    (UINT64_C(0x00ffffff) << ST_HEADER_CLASS_SHIFT)
#define ST_HEADER_SHAPE_MASK \
    (UINT64_C(0x00ffffff) << ST_HEADER_SHAPE_SHIFT)
#define ST_HEADER_AGE_MASK \
    (UINT64_C(0x0f) << ST_HEADER_AGE_SHIFT)
#define ST_HEADER_COLOR_MASK \
    (UINT64_C(0x03) << ST_HEADER_COLOR_SHIFT)
#define ST_HEADER_GENERATION_MASK \
    (UINT64_C(0x03) << ST_HEADER_GENERATION_SHIFT)
#define ST_HEADER_FLAGS_MASK \
    (UINT64_C(0xff) << ST_HEADER_FLAGS_SHIFT)

#define ST_HEADER_CLASS_MAX UINT32_C(0x00ffffff)
#define ST_HEADER_SHAPE_MAX UINT32_C(0x00ffffff)
#define ST_HEADER_AGE_MAX UINT8_C(0x0f)

typedef enum {
    ST_GC_WHITE = 0,
    ST_GC_GRAY = 1,
    ST_GC_BLACK = 2,
    ST_GC_COLOR_INVALID = 3
} st_gc_color_t;

typedef enum {
    ST_GC_NURSERY = 0,
    ST_GC_SURVIVOR = 1,
    ST_GC_OLD = 2,
    ST_GC_PERMANENT = 3
} st_gc_generation_t;

typedef uint8_t st_header_flags_t;
enum {
    ST_HEADER_REMEMBERED = UINT8_C(1) << 0,
    ST_HEADER_IMMUTABLE = UINT8_C(1) << 1,
    ST_HEADER_PINNED = UINT8_C(1) << 2,
    ST_HEADER_FINALIZABLE = UINT8_C(1) << 3,
    ST_HEADER_WEAK = UINT8_C(1) << 4,
    ST_HEADER_RESERVED_MASK = UINT8_C(0xe0),
    ST_HEADER_PUBLIC_FLAGS = UINT8_C(0x1f)
};

typedef struct {
    _Atomic uint64_t bits;
} st_object_header_t;

bool st_object_header_pack(uint32_t class_id, uint32_t shape_id, uint8_t age,
                           st_gc_color_t color,
                           st_gc_generation_t generation,
                           st_header_flags_t flags, uint64_t *word_out);
bool st_object_header_word_is_valid(uint64_t word);
/* The current runtime requires native always-lock-free 64-bit atomics. */
bool st_object_header_is_supported(void);
bool st_object_header_init(st_object_header_t *header, uint32_t class_id,
                           uint32_t shape_id, uint8_t age,
                           st_gc_color_t color,
                           st_gc_generation_t generation,
                           st_header_flags_t flags);

/* Acquire-load a complete header snapshot suitable for cache/GC decisions. */
uint64_t st_object_header_load(const st_object_header_t *header);
uint32_t st_object_header_class_id(uint64_t word);
uint32_t st_object_header_shape_id(uint64_t word);
uint8_t st_object_header_age(uint64_t word);
st_gc_color_t st_object_header_color(uint64_t word);
st_gc_generation_t st_object_header_generation(uint64_t word);
st_header_flags_t st_object_header_flags(uint64_t word);

/* Invariant-preserving concurrent transitions. Generic masked mutation is not
 * public: marking is monotonic within a collection cycle, survivor age and
 * generation change together, and remembered bits are never cleared by
 * mutators. */
bool st_object_header_try_mark_gray(st_object_header_t *header);
bool st_object_header_try_mark_black(st_object_header_t *header);
bool st_object_header_survive(st_object_header_t *header,
                              uint8_t promotion_age,
                              st_gc_generation_t *generation_out,
                              uint8_t *age_out);
bool st_object_header_remember(st_object_header_t *header);
bool st_object_header_is_lock_free(const st_object_header_t *header);

#endif
