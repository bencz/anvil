#include "st_value.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                         \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

static st_value_t integer(int64_t number)
{
    st_value_t value = 0;
    CHECK(st_value_from_small_integer(number, &value));
    return value;
}

static void expect_integer(st_value_t value, int64_t expected)
{
    int64_t actual = 0;
    CHECK(st_value_to_small_integer(value, &actual));
    CHECK(actual == expected);
}

static void test_small_integers(void)
{
    static const int64_t cases[] = {
        ST_SMALL_INTEGER_MIN, ST_SMALL_INTEGER_MIN + 1,
        -INT64_C(1048576), -2, -1, 0, 1, 2, INT64_C(1048576),
        ST_SMALL_INTEGER_MAX - 1, ST_SMALL_INTEGER_MAX
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        st_value_t value = integer(cases[i]);
        CHECK((value & ST_VALUE_TAG_MASK) == ST_VALUE_TAG_SMALL_INTEGER);
        CHECK(st_value_kind(value) == ST_VALUE_SMALL_INTEGER);
        CHECK(st_value_has_valid_encoding(value));
        expect_integer(value, cases[i]);
    }

    st_value_t out = UINT64_MAX;
    CHECK(!st_value_from_small_integer(ST_SMALL_INTEGER_MIN - 1, &out));
    CHECK(out == 0);
    out = UINT64_MAX;
    CHECK(!st_value_from_small_integer(ST_SMALL_INTEGER_MAX + 1, &out));
    CHECK(out == 0);
    CHECK(!st_value_from_small_integer(0, NULL));
}

static void test_checked_small_integer_arithmetic(void)
{
    st_value_t result = 0;
    CHECK(st_small_integer_add(integer(20), integer(22), &result));
    expect_integer(result, 42);
    CHECK(st_small_integer_subtract(integer(20), integer(22), &result));
    expect_integer(result, -2);
    CHECK(st_small_integer_multiply(integer(-7), integer(6), &result));
    expect_integer(result, -42);
    CHECK(st_small_integer_negate(integer(-42), &result));
    expect_integer(result, 42);

    result = UINT64_MAX;
    CHECK(!st_small_integer_add(integer(ST_SMALL_INTEGER_MAX), integer(1),
                                &result));
    CHECK(result == 0);
    CHECK(!st_small_integer_add(integer(ST_SMALL_INTEGER_MIN), integer(-1),
                                &result));
    CHECK(!st_small_integer_subtract(integer(ST_SMALL_INTEGER_MIN), integer(1),
                                     &result));
    CHECK(!st_small_integer_subtract(integer(ST_SMALL_INTEGER_MAX), integer(-1),
                                     &result));
    CHECK(!st_small_integer_multiply(integer(ST_SMALL_INTEGER_MAX), integer(2),
                                     &result));
    CHECK(!st_small_integer_multiply(integer(ST_SMALL_INTEGER_MIN), integer(-1),
                                     &result));
    CHECK(!st_small_integer_negate(integer(ST_SMALL_INTEGER_MIN), &result));
    CHECK(!st_small_integer_add(st_value_nil(), integer(1), &result));
    CHECK(!st_small_integer_add(integer(1), integer(1), NULL));
}

#if defined(__SIZEOF_INT128__)
__extension__ typedef __int128 st_test_wide_t;

static uint64_t random_bits(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static void check_wide_result(bool succeeded, st_value_t result,
                              st_test_wide_t expected)
{
    bool fits = expected >= (st_test_wide_t)ST_SMALL_INTEGER_MIN &&
                expected <= (st_test_wide_t)ST_SMALL_INTEGER_MAX;
    CHECK(succeeded == fits);
    if (fits) expect_integer(result, (int64_t)expected);
    else CHECK(result == 0);
}

static void test_small_integer_properties(void)
{
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    size_t iteration;
    for (iteration = 0; iteration < 100000; iteration++) {
        uint64_t left_payload = random_bits(&state) &
            ((UINT64_C(1) << 61) - 1);
        uint64_t right_payload = random_bits(&state) &
            ((UINT64_C(1) << 61) - 1);
        st_value_t left_value = (left_payload << ST_VALUE_TAG_BITS) |
                                ST_VALUE_TAG_SMALL_INTEGER;
        st_value_t right_value = (right_payload << ST_VALUE_TAG_BITS) |
                                 ST_VALUE_TAG_SMALL_INTEGER;
        int64_t left = 0;
        int64_t right = 0;
        st_value_t result = UINT64_MAX;
        CHECK(st_value_to_small_integer(left_value, &left));
        CHECK(st_value_to_small_integer(right_value, &right));
        bool succeeded = st_small_integer_add(left_value, right_value,
                                               &result);
        check_wide_result(succeeded, result,
                          (st_test_wide_t)left + (st_test_wide_t)right);
        result = UINT64_MAX;
        succeeded = st_small_integer_subtract(left_value, right_value,
                                               &result);
        check_wide_result(succeeded, result,
                          (st_test_wide_t)left - (st_test_wide_t)right);
        result = UINT64_MAX;
        succeeded = st_small_integer_multiply(left_value, right_value,
                                               &result);
        check_wide_result(succeeded, result,
                          (st_test_wide_t)left * (st_test_wide_t)right);
    }
}
#else
static void test_small_integer_properties(void)
{
    /* Boundary coverage above remains mandatory on compilers without a wider
     * integer oracle. */
}
#endif

static void test_characters_and_specials(void)
{
    static const uint32_t scalars[] = {
        0, 1, 'A', UINT32_C(0xd7ff), UINT32_C(0xe000), UINT32_C(0x10ffff)
    };
    size_t i;
    for (i = 0; i < sizeof(scalars) / sizeof(scalars[0]); i++) {
        st_value_t value = 0;
        uint32_t decoded = UINT32_MAX;
        CHECK(st_value_from_character(scalars[i], &value));
        CHECK(st_value_kind(value) == ST_VALUE_CHARACTER);
        CHECK(st_value_to_character(value, &decoded));
        CHECK(decoded == scalars[i]);
    }
    st_value_t value = UINT64_MAX;
    CHECK(!st_value_from_character(UINT32_C(0xd800), &value));
    CHECK(value == 0);
    CHECK(!st_value_from_character(UINT32_C(0xdfff), &value));
    CHECK(!st_value_from_character(UINT32_C(0x110000), &value));

    CHECK(st_value_kind(st_value_nil()) == ST_VALUE_NIL);
    CHECK(st_value_kind(st_value_false()) == ST_VALUE_FALSE);
    CHECK(st_value_kind(st_value_true()) == ST_VALUE_TRUE);
    bool boolean = true;
    CHECK(st_value_to_boolean(st_value_false(), &boolean) && !boolean);
    CHECK(st_value_to_boolean(st_value_true(), &boolean) && boolean);
    CHECK(!st_value_to_boolean(st_value_nil(), &boolean));
    CHECK(st_value_kind(UINT64_C(3) | (UINT64_C(99) << 3)) ==
          ST_VALUE_INVALID);
    CHECK(st_value_kind(UINT64_C(0xffffffffffffffff)) == ST_VALUE_INVALID);
    CHECK(!st_value_has_valid_encoding(UINT64_C(0xffffffffffffffff)));
}

static void test_object_values(void)
{
    alignas(8) unsigned char object[16] = { 0 };
    st_value_t value = 0;
    void *decoded = NULL;
    CHECK(st_value_from_object(object, &value));
    CHECK(st_value_kind(value) == ST_VALUE_OBJECT);
    CHECK(st_value_has_valid_encoding(value));
    CHECK(st_value_to_object_unchecked(value, &decoded));
    CHECK(decoded == object);
    CHECK(!st_value_from_object(NULL, &value));
    CHECK(!st_value_from_object(object + 1, &value));
    CHECK(!st_value_to_object_unchecked(st_value_nil(), &decoded));
    CHECK(st_value_kind(0) == ST_VALUE_INVALID);
    /* Encoding validation is intentionally not a heap-membership oracle. */
    CHECK(st_value_has_valid_encoding(UINT64_C(8)));
}

static void check_header_fields(uint64_t word, uint32_t class_id,
                                uint32_t shape_id, uint8_t age,
                                st_gc_color_t color,
                                st_gc_generation_t generation,
                                st_header_flags_t flags)
{
    CHECK(st_object_header_word_is_valid(word));
    CHECK(st_object_header_class_id(word) == class_id);
    CHECK(st_object_header_shape_id(word) == shape_id);
    CHECK(st_object_header_age(word) == age);
    CHECK(st_object_header_color(word) == color);
    CHECK(st_object_header_generation(word) == generation);
    CHECK(st_object_header_flags(word) == flags);
}

static void test_object_header(void)
{
    st_object_header_t header;
    uint64_t word = 0;
    st_header_flags_t flags = ST_HEADER_PINNED;
    CHECK(st_object_header_is_supported());
    CHECK(st_object_header_pack(UINT32_C(0x123456), UINT32_C(0xabcdef), 9,
                                ST_GC_GRAY, ST_GC_OLD, flags, &word));
    CHECK(word == UINT64_C(0x0499abcdef123456));
    check_header_fields(word, UINT32_C(0x123456), UINT32_C(0xabcdef), 9,
                        ST_GC_GRAY, ST_GC_OLD, flags);
    CHECK(st_object_header_init(&header, UINT32_C(0x123456),
                                UINT32_C(0xabcdef), 9, ST_GC_GRAY, ST_GC_OLD,
                                flags));
    CHECK(st_object_header_load(&header) == word);

    CHECK(st_object_header_try_mark_black(&header));
    CHECK(!st_object_header_try_mark_gray(&header));
    /* Reinitialize the mark-cycle state to exercise the complete monotonic
     * white -> gray -> black protocol. */
    CHECK(st_object_header_init(&header, UINT32_C(0x123456), 7, 0,
                                ST_GC_WHITE, ST_GC_NURSERY, flags));
    CHECK(st_object_header_try_mark_gray(&header));
    CHECK(!st_object_header_try_mark_gray(&header));
    CHECK(st_object_header_try_mark_black(&header));
    CHECK(!st_object_header_try_mark_black(&header));
    uint8_t new_age = 0;
    st_gc_generation_t new_generation = ST_GC_PERMANENT;
    CHECK(st_object_header_survive(&header, 2, &new_generation, &new_age));
    CHECK(new_generation == ST_GC_NURSERY && new_age == 1);
    CHECK(st_object_header_survive(&header, 2, &new_generation, &new_age));
    CHECK(new_generation == ST_GC_SURVIVOR && new_age == 0);
    CHECK(st_object_header_survive(&header, 2, &new_generation, &new_age));
    CHECK(new_generation == ST_GC_SURVIVOR && new_age == 1);
    CHECK(st_object_header_survive(&header, 2, &new_generation, &new_age));
    CHECK(new_generation == ST_GC_OLD && new_age == 0);
    CHECK(st_object_header_survive(&header, 2, &new_generation, &new_age));
    CHECK(new_generation == ST_GC_OLD && new_age == 0);
    CHECK(st_object_header_remember(&header));
    word = st_object_header_load(&header);
    check_header_fields(word, UINT32_C(0x123456), 7, 0,
                        ST_GC_BLACK, ST_GC_OLD,
                        flags | ST_HEADER_REMEMBERED);

    CHECK(!st_object_header_pack(0, 0, 0, ST_GC_WHITE, ST_GC_NURSERY, 0,
                                 &word));
    CHECK(!st_object_header_pack(ST_HEADER_CLASS_MAX + 1u, 0, 0,
                                 ST_GC_WHITE, ST_GC_NURSERY, 0, &word));
    CHECK(!st_object_header_pack(1, ST_HEADER_SHAPE_MAX + 1u, 0,
                                 ST_GC_WHITE, ST_GC_NURSERY, 0, &word));
    CHECK(!st_object_header_pack(1, 0, ST_HEADER_AGE_MAX + 1u,
                                 ST_GC_WHITE, ST_GC_NURSERY, 0, &word));
    CHECK(!st_object_header_pack(1, 0, 0, (st_gc_color_t)-1,
                                 ST_GC_NURSERY, 0, &word));
    CHECK(!st_object_header_pack(1, 0, 0, ST_GC_COLOR_INVALID,
                                 ST_GC_NURSERY, 0, &word));
    CHECK(!st_object_header_pack(1, 0, 0, ST_GC_WHITE,
                                 (st_gc_generation_t)-1, 0, &word));
    CHECK(!st_object_header_pack(1, 0, 0, ST_GC_WHITE, ST_GC_NURSERY,
                                 ST_HEADER_RESERVED_MASK, &word));
    CHECK(!st_object_header_survive(&header, 0, &new_generation, &new_age));
    CHECK(!st_object_header_survive(&header, ST_HEADER_AGE_MAX + 1u,
                                    &new_generation, &new_age));
    CHECK(!st_object_header_survive(&header, 2, NULL, &new_age));
    (void)st_object_header_is_lock_free(&header);
}

typedef struct {
    st_object_header_t *header;
    _Atomic unsigned *gray_winners;
    _Atomic unsigned *black_winners;
    _Atomic unsigned *errors;
} header_worker_t;

static void *header_worker(void *argument)
{
    header_worker_t *worker = argument;
    st_gc_generation_t generation;
    uint8_t age;
    if (st_object_header_try_mark_gray(worker->header))
        (void)atomic_fetch_add_explicit(worker->gray_winners, 1,
                                        memory_order_relaxed);
    if (st_object_header_try_mark_black(worker->header))
        (void)atomic_fetch_add_explicit(worker->black_winners, 1,
                                        memory_order_relaxed);
    if (!st_object_header_remember(worker->header) ||
        !st_object_header_survive(worker->header, 2, &generation, &age))
        (void)atomic_fetch_add_explicit(worker->errors, 1,
                                        memory_order_relaxed);
    return NULL;
}

static void test_concurrent_header_transitions(void)
{
    enum { WORKERS = 8 };
    st_object_header_t header;
    pthread_t threads[WORKERS];
    _Atomic unsigned gray_winners;
    _Atomic unsigned black_winners;
    _Atomic unsigned worker_errors;
    header_worker_t worker;
    unsigned index;
    unsigned created = 0;
    atomic_init(&gray_winners, 0);
    atomic_init(&black_winners, 0);
    atomic_init(&worker_errors, 0);
    CHECK(st_object_header_init(&header, 1, 1, 0, ST_GC_WHITE,
                                ST_GC_NURSERY, 0));
    worker.header = &header;
    worker.gray_winners = &gray_winners;
    worker.black_winners = &black_winners;
    worker.errors = &worker_errors;
    for (index = 0; index < WORKERS; index++) {
        int error = pthread_create(&threads[index], NULL, header_worker, &worker);
        CHECK(error == 0);
        if (error != 0) break;
        created++;
    }
    for (index = 0; index < created; index++)
        CHECK(pthread_join(threads[index], NULL) == 0);
    CHECK(atomic_load_explicit(&worker_errors, memory_order_relaxed) == 0);
    if (created == WORKERS) {
        CHECK(atomic_load_explicit(&gray_winners, memory_order_relaxed) == 1);
        CHECK(atomic_load_explicit(&black_winners, memory_order_relaxed) == 1);
        uint64_t word = st_object_header_load(&header);
        CHECK(st_object_header_color(word) == ST_GC_BLACK);
        CHECK(st_object_header_generation(word) == ST_GC_OLD);
        CHECK(st_object_header_age(word) == 0);
        CHECK((st_object_header_flags(word) & ST_HEADER_REMEMBERED) != 0);
    }
}

int main(void)
{
    test_small_integers();
    test_checked_small_integer_arithmetic();
    test_small_integer_properties();
    test_characters_and_specials();
    test_object_values();
    test_object_header();
    test_concurrent_header_transitions();
    if (failures != 0) {
        fprintf(stderr, "smalltalk value/header: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk value/header: PASS");
    return EXIT_SUCCESS;
}
