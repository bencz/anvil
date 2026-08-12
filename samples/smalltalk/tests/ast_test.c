#include "st_ast.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                         \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

static st_source_span_t span(size_t begin, size_t end)
{
    st_source_span_t result = {
        .begin = { begin, 1u, begin + 1u },
        .end = { end, 1u, end + 1u }
    };
    return result;
}

static void test_nodes_and_lists(void)
{
    st_ast_unit_t unit;
    st_ast_node_t *method;
    st_ast_node_t *block;
    st_ast_node_t *message;
    size_t index;

    CHECK(st_ast_unit_init(&unit, "example.st"));
    CHECK(unit.source_name.length == 10u);
    CHECK(strcmp(unit.source_name.data, "example.st") == 0);

    method = st_ast_new_node(&unit, ST_AST_METHOD, span(0u, 8u));
    block = st_ast_new_node(&unit, ST_AST_BLOCK, span(4u, 8u));
    message = st_ast_new_node(&unit, ST_AST_MESSAGE, span(1u, 3u));
    CHECK(method != NULL && block != NULL && message != NULL);
    CHECK(!message->as.message.starts_cascade);
    method->as.method.body = block;
    CHECK(st_ast_copy_string(&unit, "answer", 6u,
                             &method->as.method.selector));

    for (index = 0u; index < 1000u; index++) {
        st_ast_node_t *literal = st_ast_new_node(
            &unit, ST_AST_INTEGER, span(index, index + 1u));
        CHECK(literal != NULL);
        if (literal == NULL) {
            break;
        }
        literal->as.integer.radix = 10u;
        CHECK(st_ast_copy_string(&unit, "1", 1u,
                                 &literal->as.integer.spelling));
        CHECK(st_ast_list_append(&unit, &block->as.block.expressions, literal));
    }
    CHECK(block->as.block.expressions.count == 1000u);
    CHECK(block->as.block.expressions.capacity >= 1000u);
    CHECK(block->as.block.expressions.items[999]->kind == ST_AST_INTEGER);
    CHECK(unit.arena.bytes_reserved >= unit.arena.block_size);
    CHECK(st_ast_unit_status(&unit) == ST_AST_OK);
    st_ast_unit_destroy(&unit);
}

static void test_binary_string_and_alignment(void)
{
    static const unsigned char bytes[] = { 'a', 0, 'b' };
    st_ast_unit_t unit;
    st_ast_string_t string;
    uint64_t *wide;

    CHECK(st_ast_unit_init(&unit, "binary.st"));
    CHECK(st_ast_copy_string(&unit, bytes, sizeof(bytes), &string));
    CHECK(string.length == sizeof(bytes));
    CHECK(memcmp(string.data, bytes, sizeof(bytes)) == 0);
    CHECK(string.data[sizeof(bytes)] == '\0');

    wide = st_ast_alloc(&unit, sizeof(*wide), alignof(uint64_t));
    CHECK(wide != NULL);
    CHECK(((uintptr_t)wide % alignof(uint64_t)) == 0u);
    CHECK(*wide == 0u);
    st_ast_unit_destroy(&unit);
}

static void test_invalid_arguments_are_sticky(void)
{
    st_ast_unit_t unit;
    st_ast_string_t string;

    CHECK(!st_ast_unit_init(&unit, NULL));
    CHECK(st_ast_unit_status(&unit) == ST_AST_ERR_INVALID_ARGUMENT);
    st_ast_unit_destroy(&unit);

    CHECK(st_ast_unit_init(&unit, "bad.st"));
    CHECK(st_ast_alloc(&unit, 4u, 3u) == NULL);
    CHECK(st_ast_unit_status(&unit) == ST_AST_ERR_INVALID_ARGUMENT);
    CHECK(!st_ast_copy_string(&unit, "x", 1u, &string));
    CHECK(string.data == NULL && string.length == 0u);
    st_ast_unit_destroy(&unit);

    CHECK(st_ast_unit_init(&unit, "overflow.st"));
    CHECK(st_ast_alloc(&unit, SIZE_MAX, alignof(max_align_t)) == NULL);
    CHECK(st_ast_unit_status(&unit) == ST_AST_ERR_OVERFLOW);
    st_ast_unit_destroy(&unit);
}

static void test_kind_names(void)
{
    CHECK(strcmp(st_ast_kind_name(ST_AST_CLASS), "class") == 0);
    CHECK(strcmp(st_ast_kind_name(ST_AST_LITERAL_ARRAY), "literal array") == 0);
    CHECK(strcmp(st_ast_kind_name((st_ast_kind_t)999), "invalid AST node") == 0);
}

int main(void)
{
    test_nodes_and_lists();
    test_binary_string_and_alignment();
    test_invalid_arguments_are_sticky();
    test_kind_names();
    if (failures != 0u) {
        fprintf(stderr, "smalltalk AST: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk AST: PASS");
    return EXIT_SUCCESS;
}
