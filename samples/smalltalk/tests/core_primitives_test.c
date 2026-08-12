#include "st_core_primitives.h"
#include "st_source_bundle.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__extension__ typedef __int128 wide_int_t;

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,          \
                    __LINE__, #condition);                                   \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static st_value_t integer_value(int64_t integer)
{
    st_value_t value = 0;
    CHECK(st_value_from_small_integer(integer, &value));
    return value;
}

static int64_t result_integer(st_value_t value)
{
    int64_t integer = 0;
    CHECK(st_value_to_small_integer(value, &integer));
    return integer;
}

static st_core_primitive_status_t unary(uint32_t intrinsic, int64_t operand,
                                         st_value_t *result)
{
    return st_core_primitive_execute(intrinsic, integer_value(operand), NULL,
                                     0u, result);
}

static st_core_primitive_status_t binary(uint32_t intrinsic, int64_t left,
                                          int64_t right, st_value_t *result)
{
    st_value_t argument = integer_value(right);
    return st_core_primitive_execute(intrinsic, integer_value(left),
                                     &argument, 1u, result);
}

static bool in_small_integer_range(wide_int_t value)
{
    return value >= (wide_int_t)ST_SMALL_INTEGER_MIN &&
           value <= (wide_int_t)ST_SMALL_INTEGER_MAX;
}

static uint64_t random_state = UINT64_C(0xd1b54a32d192ed03);

static uint64_t random_u64(void)
{
    uint64_t value = random_state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    random_state = value;
    return value * UINT64_C(0x2545f4914f6cdd1d);
}

static int64_t random_small_integer(void)
{
    uint64_t payload = random_u64() & ((UINT64_C(1) << 61) - 1u);
    if ((payload & (UINT64_C(1) << 60)) != 0u)
        return -(int64_t)(((~payload) + 1u) &
                          ((UINT64_C(1) << 61) - 1u));
    return (int64_t)payload;
}

static void test_api_contract(void)
{
    st_value_t result = UINT64_MAX;
    st_value_t argument = integer_value(1);
    CHECK(st_core_primitive_execute(ST_INTRINSIC_INT_ADD,
          integer_value(1), &argument, 1u, NULL) ==
          ST_CORE_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(st_core_primitive_execute(UINT32_MAX, integer_value(1), NULL, 0u,
          &result) == ST_CORE_PRIMITIVE_ERR_UNKNOWN_INTRINSIC);
    CHECK(result == 0u);
    result = UINT64_MAX;
    CHECK(st_core_primitive_execute(ST_INTRINSIC_INT_ADD, integer_value(1),
          NULL, 0u, &result) == ST_CORE_PRIMITIVE_ERR_WRONG_ARITY);
    CHECK(result == 0u);
    CHECK(st_core_primitive_execute(ST_INTRINSIC_INT_ADD, integer_value(1),
          NULL, 1u, &result) == ST_CORE_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(st_core_primitive_execute(ST_INTRINSIC_INT_ADD, 0u, &argument, 1u,
          &result) == ST_CORE_PRIMITIVE_ERR_INVALID_VALUE);
    argument = st_value_true();
    CHECK(st_core_primitive_execute(ST_INTRINSIC_INT_ADD, integer_value(1),
          &argument, 1u, &result) == ST_CORE_PRIMITIVE_ERR_TYPE_MISMATCH);
    CHECK(strcmp(st_core_primitive_status_string(
          ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED),
          "integer promotion required") == 0);
}

static void test_specs_and_catalog(void)
{
    st_primitive_catalog_t catalog = {0};
    size_t count = 0u;
    size_t index;
    const st_primitive_spec_t *specs = st_core_primitive_specs(&count);
    CHECK(specs != NULL && count == 19u);
    CHECK(st_primitive_catalog_init(&catalog,
          (st_primitive_allocator_t){0}));
    for (index = 0u; index < count; ++index) {
        size_t previous;
        st_value_t result = 0;
        const st_primitive_spec_t *spec = &specs[index];
        CHECK(spec->implementation_kind == ST_PRIMITIVE_INTRINSIC);
        CHECK(spec->intrinsic_id != ST_PRIMITIVE_INVALID_INTRINSIC_ID);
        CHECK(spec->runtime_symbol == NULL && spec->runtime_symbol_length == 0u);
        CHECK(st_primitive_catalog_register(&catalog, spec, NULL) ==
              ST_PRIMITIVE_OK);
        /* Every advertised ID is executable; operand errors are acceptable,
         * an absent handler is not. */
        CHECK(st_core_primitive_execute(spec->intrinsic_id, integer_value(0),
              spec->method_arity ? (st_value_t[]){ integer_value(0) } : NULL,
              spec->method_arity, &result) !=
              ST_CORE_PRIMITIVE_ERR_UNKNOWN_INTRINSIC);
        for (previous = 0u; previous < index; ++previous)
            CHECK(specs[previous].intrinsic_id != spec->intrinsic_id);
    }
    CHECK(st_primitive_catalog_count(&catalog) == count);
    CHECK(st_primitive_catalog_lookup(&catalog, "IntAsFloatPrimitive", 19u)
          == NULL);
    CHECK(st_primitive_catalog_lookup(&catalog, "ClassPrimitive", 14u)
          == NULL);
    CHECK(st_primitive_catalog_lookup(&catalog, "SizePrimitive", 13u)
          == NULL);
    st_primitive_catalog_destroy(&catalog);
}

static const char *image_directory(void)
{
    if (access("st-image", R_OK) == 0) return "st-image";
    if (access("samples/smalltalk/st-image", R_OK) == 0)
        return "samples/smalltalk/st-image";
    return NULL;
}

static void test_real_image_catalog_subset(void)
{
    const char *directory = image_directory();
    st_source_bundle_t bundle;
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t result;
    const st_ast_unit_t **units = NULL;
    const st_primitive_spec_t *specs;
    size_t spec_count = 0u;
    size_t index;
    CHECK(directory != NULL);
    if (!directory) return;
    CHECK(st_source_bundle_load(&bundle, directory, NULL, 0u, NULL) ==
          ST_SOURCE_LOAD_OK);
    if (bundle.diagnostic.status != ST_SOURCE_LOAD_OK) return;
    units = malloc(bundle.count * sizeof(*units));
    CHECK(units != NULL);
    if (!units) {
        st_source_bundle_destroy(&bundle);
        return;
    }
    for (index = 0u; index < bundle.count; ++index)
        units[index] = &bundle.files[index].ast;

    CHECK(st_primitive_catalog_init(&catalog,
          (st_primitive_allocator_t){0}));
    specs = st_core_primitive_specs(&spec_count);
    for (index = 0u; index < spec_count; ++index)
        CHECK(st_primitive_catalog_register(&catalog, &specs[index], NULL) ==
              ST_PRIMITIVE_OK);
    st_primitive_result_init(&result);
    CHECK(st_primitive_resolve(&result, units, bundle.count, &catalog, NULL) ==
          ST_PRIMITIVE_OK);
    /* Exactly these 19 pragma uses have executable handlers today. The other
     * 50 uses must remain hard diagnostics until their RT/boxing contracts
     * exist; registering names without implementations would be a stub. */
    CHECK(result.binding_count == 19u);
    CHECK(result.diagnostic_count == 50u);
    for (index = 0u; index < result.diagnostic_count; ++index)
        CHECK(result.diagnostics[index].code ==
              ST_PRIMITIVE_DIAG_MISSING_IMPLEMENTATION);
    st_primitive_result_destroy(&result);
    st_primitive_catalog_destroy(&catalog);
    free(units);
    st_source_bundle_destroy(&bundle);
}

static void test_identity_and_comparisons(void)
{
    static const uint32_t intrinsic_ids[] = {
        ST_INTRINSIC_INT_EQUALS, ST_INTRINSIC_INT_NOT_EQUALS,
        ST_INTRINSIC_INT_LESS_THAN, ST_INTRINSIC_INT_LESS_EQUALS,
        ST_INTRINSIC_INT_GREATER_THAN, ST_INTRINSIC_INT_GREATER_EQUALS
    };
    size_t iteration;
    st_value_t result = 0;
    st_value_t argument = integer_value(42);
    bool boolean = false;
    CHECK(st_core_primitive_execute(ST_INTRINSIC_IDENTITY, integer_value(42),
          &argument, 1u, &result) == ST_CORE_PRIMITIVE_OK);
    CHECK(st_value_to_boolean(result, &boolean) && boolean);
    argument = st_value_true();
    CHECK(st_core_primitive_execute(ST_INTRINSIC_IDENTITY, st_value_false(),
          &argument, 1u, &result) == ST_CORE_PRIMITIVE_OK);
    CHECK(st_value_to_boolean(result, &boolean) && !boolean);

    CHECK(binary(ST_INTRINSIC_INT_EQUALS, -7, -7, &result) ==
          ST_CORE_PRIMITIVE_OK && result == st_value_true());
    CHECK(binary(ST_INTRINSIC_INT_NOT_EQUALS, -7, 8, &result) ==
          ST_CORE_PRIMITIVE_OK && result == st_value_true());
    CHECK(binary(ST_INTRINSIC_INT_LESS_THAN, -7, 8, &result) ==
          ST_CORE_PRIMITIVE_OK && result == st_value_true());
    CHECK(binary(ST_INTRINSIC_INT_LESS_EQUALS, 8, 8, &result) ==
          ST_CORE_PRIMITIVE_OK && result == st_value_true());
    CHECK(binary(ST_INTRINSIC_INT_GREATER_THAN, 9, 8, &result) ==
          ST_CORE_PRIMITIVE_OK && result == st_value_true());
    CHECK(binary(ST_INTRINSIC_INT_GREATER_EQUALS, 8, 8, &result) ==
          ST_CORE_PRIMITIVE_OK && result == st_value_true());

    for (iteration = 0u; iteration < 100000u; ++iteration) {
        int64_t left = random_small_integer();
        int64_t right = random_small_integer();
        bool expected[] = {
            left == right, left != right, left < right,
            left <= right, left > right, left >= right
        };
        size_t operation;
        for (operation = 0u;
             operation < sizeof(intrinsic_ids) / sizeof(intrinsic_ids[0]);
             ++operation) {
            CHECK(binary(intrinsic_ids[operation], left, right, &result) ==
                  ST_CORE_PRIMITIVE_OK);
            CHECK(st_value_to_boolean(result, &boolean));
            CHECK(boolean == expected[operation]);
        }
    }
}

static void check_wide_binary(uint32_t intrinsic, int64_t left, int64_t right,
                              wide_int_t expected)
{
    st_value_t result = UINT64_MAX;
    st_core_primitive_status_t status = binary(intrinsic, left, right, &result);
    if (in_small_integer_range(expected)) {
        CHECK(status == ST_CORE_PRIMITIVE_OK);
        if (status == ST_CORE_PRIMITIVE_OK)
            CHECK(result_integer(result) == (int64_t)expected);
    } else {
        CHECK(status == ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED);
        CHECK(result == 0u);
    }
}

static void test_arithmetic_properties(void)
{
    static const int64_t edges[] = {
        ST_SMALL_INTEGER_MIN, ST_SMALL_INTEGER_MIN + 1, -1073741824, -2, -1,
        0, 1, 2, 1073741824, ST_SMALL_INTEGER_MAX - 1,
        ST_SMALL_INTEGER_MAX
    };
    size_t left_index;
    size_t right_index;
    size_t iteration;
    st_value_t result;
    for (left_index = 0u; left_index < sizeof(edges) / sizeof(edges[0]);
         ++left_index) {
        for (right_index = 0u;
             right_index < sizeof(edges) / sizeof(edges[0]); ++right_index) {
            int64_t left = edges[left_index];
            int64_t right = edges[right_index];
            check_wide_binary(ST_INTRINSIC_INT_ADD, left, right,
                              (wide_int_t)left + right);
            check_wide_binary(ST_INTRINSIC_INT_SUBTRACT, left, right,
                              (wide_int_t)left - right);
            check_wide_binary(ST_INTRINSIC_INT_MULTIPLY, left, right,
                              (wide_int_t)left * right);
        }
    }
    for (iteration = 0u; iteration < 200000u; ++iteration) {
        int64_t left = random_small_integer();
        int64_t right = random_small_integer();
        check_wide_binary(ST_INTRINSIC_INT_ADD, left, right,
                          (wide_int_t)left + right);
        check_wide_binary(ST_INTRINSIC_INT_SUBTRACT, left, right,
                          (wide_int_t)left - right);
        check_wide_binary(ST_INTRINSIC_INT_MULTIPLY, left, right,
                          (wide_int_t)left * right);
    }
    CHECK(unary(ST_INTRINSIC_INT_NEGATE, ST_SMALL_INTEGER_MIN, &result) ==
          ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED && result == 0u);
    CHECK(unary(ST_INTRINSIC_INT_NEGATE, ST_SMALL_INTEGER_MAX, &result) ==
          ST_CORE_PRIMITIVE_OK &&
          result_integer(result) == -ST_SMALL_INTEGER_MAX);
}

static void reference_floor_divmod(int64_t left, int64_t right,
                                   int64_t *quotient_out,
                                   int64_t *remainder_out)
{
    wide_int_t quotient = (wide_int_t)left / (wide_int_t)right;
    wide_int_t remainder = (wide_int_t)left % (wide_int_t)right;
    if (remainder != 0 && ((remainder < 0) != (right < 0))) {
        --quotient;
        remainder += right;
    }
    *quotient_out = (int64_t)quotient;
    *remainder_out = (int64_t)remainder;
}

static void test_division_and_modulo(void)
{
    size_t iteration;
    st_value_t result = UINT64_MAX;
    CHECK(binary(ST_INTRINSIC_INT_FLOOR_DIVIDE, 1, 0, &result) ==
          ST_CORE_PRIMITIVE_ERR_DIVISION_BY_ZERO && result == 0u);
    CHECK(binary(ST_INTRINSIC_INT_MODULO, 1, 0, &result) ==
          ST_CORE_PRIMITIVE_ERR_DIVISION_BY_ZERO && result == 0u);
    CHECK(binary(ST_INTRINSIC_INT_FLOOR_DIVIDE, ST_SMALL_INTEGER_MIN, -1,
          &result) == ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED);
    CHECK(binary(ST_INTRINSIC_INT_MODULO, ST_SMALL_INTEGER_MIN, -1,
          &result) == ST_CORE_PRIMITIVE_OK && result_integer(result) == 0);

    for (iteration = 0u; iteration < 200000u; ++iteration) {
        int64_t left = random_small_integer();
        int64_t right = random_small_integer();
        int64_t quotient;
        int64_t remainder;
        st_core_primitive_status_t status;
        if (right == 0) right = 1;
        reference_floor_divmod(left, right, &quotient, &remainder);
        status = binary(ST_INTRINSIC_INT_FLOOR_DIVIDE, left, right, &result);
        if (quotient < ST_SMALL_INTEGER_MIN ||
            quotient > ST_SMALL_INTEGER_MAX) {
            CHECK(status == ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED);
        } else {
            CHECK(status == ST_CORE_PRIMITIVE_OK);
            if (status == ST_CORE_PRIMITIVE_OK)
                CHECK(result_integer(result) == quotient);
        }
        CHECK(binary(ST_INTRINSIC_INT_MODULO, left, right, &result) ==
              ST_CORE_PRIMITIVE_OK);
        CHECK(result_integer(result) == remainder);
        CHECK((wide_int_t)quotient * right + remainder == left);
        CHECK(remainder == 0 || ((remainder < 0) == (right < 0)));
    }
}

static int64_t reference_shift_right(int64_t value, uint64_t count)
{
    if (count >= 61u) return value < 0 ? -1 : 0;
    if (value >= 0) return (int64_t)((uint64_t)value >> (unsigned)count);
    {
        uint64_t magnitude = (uint64_t)(-value);
        uint64_t divisor = UINT64_C(1) << (unsigned)count;
        return -(int64_t)((magnitude + divisor - 1u) / divisor);
    }
}

static void test_bits_and_shifts(void)
{
    size_t iteration;
    st_value_t result;
    for (iteration = 0u; iteration < 200000u; ++iteration) {
        int64_t left = random_small_integer();
        int64_t right = random_small_integer();
        CHECK(binary(ST_INTRINSIC_INT_BIT_AND, left, right, &result) ==
              ST_CORE_PRIMITIVE_OK);
        CHECK((uint64_t)result_integer(result) ==
              ((uint64_t)left & (uint64_t)right));
        CHECK(binary(ST_INTRINSIC_INT_BIT_OR, left, right, &result) ==
              ST_CORE_PRIMITIVE_OK);
        CHECK((uint64_t)result_integer(result) ==
              ((uint64_t)left | (uint64_t)right));
        CHECK(binary(ST_INTRINSIC_INT_BIT_XOR, left, right, &result) ==
              ST_CORE_PRIMITIVE_OK);
        CHECK((uint64_t)result_integer(result) ==
              ((uint64_t)left ^ (uint64_t)right));
    }

    CHECK(binary(ST_INTRINSIC_INT_SHIFT, 0, ST_SMALL_INTEGER_MAX, &result) ==
          ST_CORE_PRIMITIVE_OK && result_integer(result) == 0);
    CHECK(binary(ST_INTRINSIC_INT_SHIFT, 1, 61, &result) ==
          ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED);
    CHECK(binary(ST_INTRINSIC_INT_SHIFT, -1, 60, &result) ==
          ST_CORE_PRIMITIVE_OK && result_integer(result) ==
          ST_SMALL_INTEGER_MIN);
    CHECK(binary(ST_INTRINSIC_INT_SHIFT, -2, -1, &result) ==
          ST_CORE_PRIMITIVE_OK && result_integer(result) == -1);
    CHECK(binary(ST_INTRINSIC_INT_SHIFT, -3, -1, &result) ==
          ST_CORE_PRIMITIVE_OK && result_integer(result) == -2);
    CHECK(binary(ST_INTRINSIC_INT_SHIFT, -3, ST_SMALL_INTEGER_MIN, &result) ==
          ST_CORE_PRIMITIVE_OK && result_integer(result) == -1);
    CHECK(binary(ST_INTRINSIC_INT_SHIFT, 3, ST_SMALL_INTEGER_MIN, &result) ==
          ST_CORE_PRIMITIVE_OK && result_integer(result) == 0);

    for (iteration = 0u; iteration < 100000u; ++iteration) {
        int64_t value = random_small_integer();
        int64_t count = (int64_t)(random_u64() % 161u) - 80;
        st_core_primitive_status_t status = binary(
            ST_INTRINSIC_INT_SHIFT, value, count, &result);
        if (count < 0) {
            CHECK(status == ST_CORE_PRIMITIVE_OK);
            CHECK(result_integer(result) ==
                  reference_shift_right(value, (uint64_t)(-count)));
        } else if (value == 0) {
            CHECK(status == ST_CORE_PRIMITIVE_OK);
            CHECK(result_integer(result) == 0);
        } else if (count >= 61) {
            CHECK(status == ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED);
        } else {
            wide_int_t expected = (wide_int_t)value;
            unsigned shift = (unsigned)count;
            expected *= ((wide_int_t)1 << shift);
            if (in_small_integer_range(expected)) {
                CHECK(status == ST_CORE_PRIMITIVE_OK);
                CHECK(result_integer(result) == (int64_t)expected);
            } else {
                CHECK(status == ST_CORE_PRIMITIVE_ERR_PROMOTION_REQUIRED);
            }
        }
    }
}

static void test_characters(void)
{
    static const int64_t valid[] = { 0, 1, 127, 255, 0xd7ff, 0xe000,
                                     0x10ffff };
    static const int64_t invalid[] = { -1, 0xd800, 0xdfff, 0x110000,
                                       ST_SMALL_INTEGER_MAX };
    size_t index;
    st_value_t result;
    st_value_t class_receiver = st_value_nil();
    for (index = 0u; index < sizeof(valid) / sizeof(valid[0]); ++index) {
        st_value_t argument = integer_value(valid[index]);
        CHECK(st_core_primitive_execute(ST_INTRINSIC_CHARACTER_NEW,
              class_receiver, &argument, 1u, &result) == ST_CORE_PRIMITIVE_OK);
        CHECK(st_value_kind(result) == ST_VALUE_CHARACTER);
        CHECK(st_core_primitive_execute(ST_INTRINSIC_CHARACTER_CODE, result,
              NULL, 0u, &result) == ST_CORE_PRIMITIVE_OK);
        CHECK(result_integer(result) == valid[index]);
    }
    for (index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        st_value_t argument = integer_value(invalid[index]);
        CHECK(st_core_primitive_execute(ST_INTRINSIC_CHARACTER_NEW,
              class_receiver, &argument, 1u, &result) ==
              ST_CORE_PRIMITIVE_ERR_INVALID_CODE_POINT);
        CHECK(result == 0u);
    }
    CHECK(st_core_primitive_execute(ST_INTRINSIC_CHARACTER_CODE,
          integer_value(65), NULL, 0u, &result) ==
          ST_CORE_PRIMITIVE_ERR_TYPE_MISMATCH);
}

int main(void)
{
    test_api_contract();
    test_specs_and_catalog();
    test_real_image_catalog_subset();
    test_identity_and_comparisons();
    test_arithmetic_properties();
    test_division_and_modulo();
    test_bits_and_shifts();
    test_characters();
    if (failures != 0u) {
        fprintf(stderr, "core primitive regression: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("core primitive regression: PASS");
    return EXIT_SUCCESS;
}
