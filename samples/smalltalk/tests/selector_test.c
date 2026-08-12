#include "st_selector.h"

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

static void expect_selector(const st_selector_table_t *table,
                            st_selector_id_t id, const char *spelling,
                            st_selector_kind_t kind, uint32_t arity)
{
    const st_selector_t *selector = st_selector_get(table, id);
    CHECK(selector != NULL);
    if (!selector) return;
    CHECK(selector->length == strlen(spelling));
    CHECK(memcmp(selector->bytes, spelling, selector->length) == 0);
    CHECK(selector->bytes[selector->length] == '\0');
    CHECK(selector->kind == kind);
    CHECK(selector->arity == arity);
}

static void test_selector_shapes_and_identity(void)
{
    st_selector_table_t table = { 0 };
    st_selector_id_t size_id = 0;
    st_selector_id_t plus_id = 0;
    st_selector_id_t at_put_id = 0;
    st_selector_id_t duplicate = 0;
    CHECK(st_selector_table_init(&table, (st_selector_allocator_t){ 0 },
                                 UINT64_C(0x123456789abcdef0)));
    CHECK(st_selector_intern(&table, "size", 4, &size_id) == ST_SELECTOR_OK);
    CHECK(st_selector_intern(&table, "+", 1, &plus_id) == ST_SELECTOR_OK);
    CHECK(st_selector_intern(&table, "at:put:", 7, &at_put_id) ==
          ST_SELECTOR_OK);
    CHECK(size_id != plus_id && plus_id != at_put_id);
    CHECK(st_selector_intern(&table, "size", 4, &duplicate) == ST_SELECTOR_OK);
    CHECK(duplicate == size_id);
    CHECK(st_selector_count(&table) == 3);
    expect_selector(&table, size_id, "size", ST_SELECTOR_UNARY, 0);
    expect_selector(&table, plus_id, "+", ST_SELECTOR_BINARY, 1);
    expect_selector(&table, at_put_id, "at:put:", ST_SELECTOR_KEYWORD, 2);
    duplicate = 0;
    CHECK(st_selector_lookup(&table, "at:put:", 7, &duplicate));
    CHECK(duplicate == at_put_id);
    CHECK(!st_selector_lookup(&table, "absent", 6, &duplicate));
    CHECK(duplicate == ST_SELECTOR_INVALID_ID);
    const st_selector_t *stable = st_selector_get(&table, size_id);
    CHECK(st_selector_table_freeze(&table));
    CHECK(st_selector_table_is_frozen(&table));
    CHECK(st_selector_table_freeze(&table));
    CHECK(st_selector_intern(&table, "late", 4, &duplicate) ==
          ST_SELECTOR_ERR_FROZEN);
    CHECK(duplicate == ST_SELECTOR_INVALID_ID);
    CHECK(st_selector_get(&table, size_id) == stable);
    CHECK(st_selector_lookup(&table, "size", 4, &duplicate));
    CHECK(duplicate == size_id);
    st_selector_table_destroy(&table);
}

static void test_growth_and_masked_probing(void)
{
    st_selector_table_t table = { 0 };
    char spelling[40];
    size_t index;
    CHECK(st_selector_table_init(&table, (st_selector_allocator_t){ 0 }, 0));
    for (index = 0; index < 10000; index++) {
        int length = snprintf(spelling, sizeof(spelling), "selector%zu:", index);
        st_selector_id_t id = 0;
        CHECK(length > 0 && (size_t)length < sizeof(spelling));
        CHECK(st_selector_intern(&table, spelling, (size_t)length, &id) ==
              ST_SELECTOR_OK);
        CHECK(id == index + 1);
    }
    CHECK(st_selector_count(&table) == 10000);
    CHECK(table.table_capacity != 0 &&
          (table.table_capacity & (table.table_capacity - 1)) == 0);
    CHECK(table.count * 4 <= table.table_capacity * 3);
    for (index = 0; index < 10000; index += 37) {
        int length = snprintf(spelling, sizeof(spelling), "selector%zu:", index);
        st_selector_id_t id = 0;
        CHECK(st_selector_lookup(&table, spelling, (size_t)length, &id));
        CHECK(id == index + 1);
    }
    st_selector_table_destroy(&table);
}

static void test_invalid_spellings(void)
{
    static const char *invalid[] = {
        "", "1abc", "abc:def", "abc::", ":abc", "abc:+", "a b", "a\0b"
    };
    st_selector_table_t table = { 0 };
    size_t index;
    CHECK(st_selector_table_init(&table, (st_selector_allocator_t){ 0 }, 1));
    for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        size_t length = index == 7 ? 3 : strlen(invalid[index]);
        st_selector_id_t id = UINT32_MAX;
        CHECK(st_selector_intern(&table, invalid[index], length, &id) ==
              (length == 0 ? ST_SELECTOR_ERR_INVALID_ARGUMENT
                           : ST_SELECTOR_ERR_INVALID_SPELLING));
        CHECK(id == ST_SELECTOR_INVALID_ID);
    }
    CHECK(st_selector_count(&table) == 0);
    CHECK(st_selector_intern(&table, NULL, 1, NULL) ==
          ST_SELECTOR_ERR_INVALID_ARGUMENT);
    st_selector_table_destroy(&table);
}

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t live;
} fault_allocator_t;

static void *fault_allocate(void *user, size_t size)
{
    fault_allocator_t *fault = user;
    fault->calls++;
    if (fault->calls == fault->fail_at) return NULL;
    void *pointer = malloc(size);
    if (pointer) fault->live++;
    return pointer;
}

static void fault_deallocate(void *user, void *pointer)
{
    fault_allocator_t *fault = user;
    if (pointer) {
        CHECK(fault->live != 0);
        fault->live--;
        free(pointer);
    }
}

static void run_fault_scenario(size_t prefix_count, const char *new_selector,
                               size_t expected_allocations)
{
    size_t fail_at;
    bool observed_success = false;
    for (fail_at = 1; fail_at <= expected_allocations + 1; fail_at++) {
        fault_allocator_t fault = { .fail_at = fail_at };
        st_selector_table_t table = { 0 };
        st_selector_allocator_t allocator = {
            .allocate = fault_allocate,
            .deallocate = fault_deallocate,
            .user = &fault
        };
        st_selector_id_t added = UINT32_MAX;
        CHECK(st_selector_table_init(&table, allocator, 7));
        fault.fail_at = SIZE_MAX;
        size_t prefix;
        for (prefix = 0; prefix < prefix_count; prefix++) {
            char spelling[32];
            int length = snprintf(spelling, sizeof(spelling), "base%zu", prefix);
            st_selector_id_t id = 0;
            CHECK(length > 0 && (size_t)length < sizeof(spelling));
            CHECK(st_selector_intern(&table, spelling, (size_t)length, &id) ==
                  ST_SELECTOR_OK);
            CHECK(id == prefix + 1);
        }
        size_t baseline_count = st_selector_count(&table);
        size_t baseline_live = fault.live;
        fault.calls = 0;
        fault.fail_at = fail_at;
        st_selector_status_t status = st_selector_intern(
            &table, new_selector, strlen(new_selector), &added);
        if (status == ST_SELECTOR_OK) {
            observed_success = true;
            CHECK(added != ST_SELECTOR_INVALID_ID);
            CHECK(st_selector_count(&table) == baseline_count + 1);
        } else {
            CHECK(status == ST_SELECTOR_ERR_OUT_OF_MEMORY);
            CHECK(added == ST_SELECTOR_INVALID_ID);
            CHECK(st_selector_count(&table) == baseline_count);
            CHECK(fault.live == baseline_live);
            for (prefix = 0; prefix < prefix_count; prefix++) {
                char spelling[32];
                int length = snprintf(spelling, sizeof(spelling),
                                      "base%zu", prefix);
                st_selector_id_t found = 0;
                CHECK(st_selector_lookup(&table, spelling, (size_t)length,
                                         &found));
                CHECK(found == prefix + 1);
            }
        }
        st_selector_table_destroy(&table);
        CHECK(fault.live == 0);
        if (observed_success) break;
    }
    CHECK(observed_success);
}

static void test_transactional_out_of_memory(void)
{
    /* Empty insertion allocates spelling, descriptor vector and hash table. */
    run_fault_scenario(0, "first", 3);
    /* The ninth selector grows only the descriptor vector. */
    run_fault_scenario(8, "vectorGrowth", 2);
    /* At 75% load, the thirteenth selector grows only the hash table. */
    run_fault_scenario(12, "tableGrowth", 2);
}

static void test_program_snapshot_order_and_transaction(void)
{
    st_ast_node_t send_alpha = {
        .kind = ST_AST_MESSAGE,
        .as.message.selector = {"gamma", 5u}
    };
    st_ast_node_t send_only = {
        .kind = ST_AST_MESSAGE,
        .as.message.selector = {"missing:", 8u}
    };
    st_ast_node_t *first_messages[] = {&send_alpha};
    st_ast_node_t *second_messages[] = {&send_only};
    st_ast_node_t first_receiver = {.kind = ST_AST_TRUE};
    st_ast_node_t second_receiver = {.kind = ST_AST_FALSE};
    st_ast_node_t first_expression = {
        .kind = ST_AST_EXPRESSION,
        .as.expression = {
            .receiver = &first_receiver,
            .messages = {first_messages, 1u, 1u}
        }
    };
    st_ast_node_t second_expression = {
        .kind = ST_AST_EXPRESSION,
        .as.expression = {
            .receiver = &second_receiver,
            .messages = {second_messages, 1u, 1u}
        }
    };
    st_ast_node_t *first_expressions[] = {&first_expression};
    st_ast_node_t *second_expressions[] = {&second_expression};
    st_ast_node_t first_body = {
        .kind = ST_AST_BLOCK,
        .as.block.expressions = {first_expressions, 1u, 1u}
    };
    st_ast_node_t second_body = {
        .kind = ST_AST_BLOCK,
        .as.block.expressions = {second_expressions, 1u, 1u}
    };
    st_ast_node_t first_method = {
        .kind = ST_AST_METHOD,
        .as.method = {.selector = {"alpha", 5u}, .body = &first_body}
    };
    st_ast_node_t second_method = {
        .kind = ST_AST_METHOD,
        .as.method = {.selector = {"beta:", 5u}, .body = &second_body}
    };
    st_ast_node_t *methods[] = {&first_method, &second_method};
    st_ast_node_t class_node = {
        .kind = ST_AST_CLASS,
        .as.class_decl.methods = {methods, 2u, 2u}
    };
    st_ast_node_t *forms[] = {&class_node};
    st_ast_unit_t unit = {.forms = {forms, 1u, 1u}};
    const st_ast_unit_t *units[] = {&unit};
    st_selector_table_t first = {0};
    st_selector_table_t second = {0};
    st_selector_id_t selector_id;

    CHECK(st_selector_table_build_for_units(
              &first, units, 1u, (st_selector_allocator_t){0}, 19u)
          == ST_SELECTOR_OK);
    CHECK(st_selector_table_build_for_units(
              &second, units, 1u, (st_selector_allocator_t){0}, 19u)
          == ST_SELECTOR_OK);
    CHECK(st_selector_table_is_frozen(&first));
    CHECK(st_selector_count(&first) == 4u);
    expect_selector(&first, 1u, "alpha", ST_SELECTOR_UNARY, 0u);
    expect_selector(&first, 2u, "beta:", ST_SELECTOR_KEYWORD, 1u);
    expect_selector(&first, 3u, "gamma", ST_SELECTOR_UNARY, 0u);
    expect_selector(&first, 4u, "missing:", ST_SELECTOR_KEYWORD, 1u);
    for (selector_id = 1u; selector_id <= st_selector_count(&first);
         selector_id++) {
        const st_selector_t *left = st_selector_get(&first, selector_id);
        const st_selector_t *right = st_selector_get(&second, selector_id);
        CHECK(left != NULL && right != NULL);
        if (left != NULL && right != NULL) {
            CHECK(left->hash == right->hash);
            CHECK(left->length == right->length);
            CHECK(memcmp(left->bytes, right->bytes, left->length) == 0);
        }
    }
    st_selector_table_destroy(&second);
    st_selector_table_destroy(&first);

    {
        fault_allocator_t fault = {.fail_at = 1u};
        st_selector_allocator_t allocator = {
            fault_allocate, fault_deallocate, &fault
        };
        st_selector_table_t failed = {0};
        CHECK(st_selector_table_build_for_units(
                  &failed, units, 1u, allocator, 19u)
              == ST_SELECTOR_ERR_OUT_OF_MEMORY);
        CHECK(!failed.initialized && failed.selectors == NULL
              && failed.entries == NULL && fault.live == 0u);
    }
}

int main(void)
{
    test_selector_shapes_and_identity();
    test_growth_and_masked_probing();
    test_invalid_spellings();
    test_transactional_out_of_memory();
    test_program_snapshot_order_and_transaction();
    if (failures != 0) {
        fprintf(stderr, "smalltalk selectors: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk selectors: PASS");
    return EXIT_SUCCESS;
}
