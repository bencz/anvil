#include "st_parser.h"
#include "st_sema.h"

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

typedef struct {
    st_ast_unit_t unit;
    st_parser_t parser;
    st_ast_node_t *method;
} fixture_t;

static bool text_is(st_ast_string_t value, const char *expected)
{
    size_t length = strlen(expected);
    return value.length == length
        && (length == 0u || memcmp(value.data, expected, length) == 0);
}

static bool fixture_init(fixture_t *fixture, const char *source)
{
    memset(fixture, 0, sizeof(*fixture));
    if (!st_ast_unit_init(&fixture->unit, "sema-test.st")) return false;
    if (!st_parser_init_cstr(&fixture->parser, &fixture->unit, source)) {
        st_ast_unit_destroy(&fixture->unit);
        return false;
    }
    fixture->method = st_parse_method(&fixture->parser);
    if (fixture->method == NULL || !st_parser_at_end(&fixture->parser)) {
        fprintf(stderr, "fixture parse failed for: %s (%s)\n", source,
                st_parse_status_string(st_parser_status(&fixture->parser)));
        st_parser_destroy(&fixture->parser);
        st_ast_unit_destroy(&fixture->unit);
        memset(fixture, 0, sizeof(*fixture));
        return false;
    }
    return true;
}

static void fixture_destroy(fixture_t *fixture)
{
    st_parser_destroy(&fixture->parser);
    st_ast_unit_destroy(&fixture->unit);
}

static st_sema_status_t analyze(st_sema_result_t *result,
                                const st_ast_node_t *method,
                                const st_sema_external_t *entries,
                                size_t count, bool has_super)
{
    st_sema_catalog_t catalog;
    memset(&catalog, 0, sizeof(catalog));
    catalog.entries = entries;
    catalog.count = count;
    catalog.has_lexical_super = has_super;
    st_sema_result_init(result);
    return st_sema_analyze_method(result, method, &catalog);
}

static const st_sema_binding_t *find_binding(const st_sema_result_t *result,
                                             st_sema_binding_kind_t kind,
                                             const char *name)
{
    size_t index;
    for (index = 0u; index < result->binding_count; index++) {
        if (result->bindings[index].kind == kind
                && text_is(result->bindings[index].name, name)) {
            return &result->bindings[index];
        }
    }
    return NULL;
}

static size_t binding_index(const st_sema_result_t *result,
                            const st_sema_binding_t *binding)
{
    return binding == NULL ? SIZE_MAX : (size_t)(binding - result->bindings);
}

static size_t diagnostic_count(const st_sema_result_t *result,
                               st_sema_diagnostic_code_t code)
{
    size_t index;
    size_t count = 0u;
    for (index = 0u; index < result->diagnostic_count; index++) {
        if (result->diagnostics[index].code == code) count++;
    }
    return count;
}

static st_ast_node_t *method_expression(fixture_t *fixture, size_t index)
{
    st_ast_node_t *body = fixture->method->as.method.body;
    CHECK(body != NULL && body->kind == ST_AST_BLOCK);
    CHECK(index < body->as.block.expressions.count);
    return index < body->as.block.expressions.count
        ? body->as.block.expressions.items[index] : NULL;
}

static st_ast_node_t *receiver_of(st_ast_node_t *expression)
{
    CHECK(expression != NULL && expression->kind == ST_AST_EXPRESSION);
    return expression != NULL && expression->kind == ST_AST_EXPRESSION
        ? expression->as.expression.receiver : NULL;
}

static void test_dense_bindings_types_and_references(void)
{
    fixture_t fixture;
    st_sema_result_t result;
    const st_sema_binding_t *left;
    const st_sema_binding_t *right;
    const st_sema_binding_t *total;
    st_ast_node_t *assignment;
    st_ast_node_t *returned;
    const st_sema_reference_t *reference;

    CHECK(fixture_init(&fixture,
        "sum: left with: right [ | total::Number | "
        "total := left. ^total ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result));
    CHECK(result.scope_count == 1u);
    CHECK(result.scopes[0].parent == ST_SEMA_INVALID_ID);
    CHECK(result.scopes[0].first_binding == 0u);
    CHECK(result.scopes[0].binding_count == 3u);
    CHECK(result.binding_count == 3u);
    left = find_binding(&result, ST_SEMA_BIND_METHOD_ARGUMENT, "left");
    right = find_binding(&result, ST_SEMA_BIND_METHOD_ARGUMENT, "right");
    total = find_binding(&result, ST_SEMA_BIND_TEMPORARY, "total");
    CHECK(left != NULL && left->slot == 0u);
    CHECK(right != NULL && right->slot == 1u);
    CHECK(total != NULL && total->slot == 2u);
    CHECK(left != NULL && (left->flags & ST_SEMA_BINDING_READONLY) != 0u);
    CHECK(total != NULL && (total->flags & ST_SEMA_BINDING_ASSIGNED) != 0u);
    CHECK(total != NULL && total->has_type && text_is(total->type_name, "Number"));

    assignment = method_expression(&fixture, 0u);
    returned = method_expression(&fixture, 1u);
    reference = st_sema_reference_for_node(
        &result, assignment->as.expression.assignments.items[0]);
    CHECK(reference != NULL
        && reference->binding == binding_index(&result, total)
        && reference->access == ST_SEMA_ACCESS_WRITE);
    reference = st_sema_reference_for_node(&result, receiver_of(assignment));
    CHECK(reference != NULL
        && reference->binding == binding_index(&result, left)
        && reference->access == ST_SEMA_ACCESS_READ);
    reference = st_sema_reference_for_node(&result, receiver_of(returned));
    CHECK(reference != NULL && reference->binding == binding_index(&result, total));
    CHECK(result.return_count == 1u);
    CHECK(result.returns[0].kind == ST_SEMA_RETURN_LOCAL_METHOD);
    CHECK(result.returns[0].block == ST_SEMA_INVALID_ID);
    CHECK(st_sema_reference_for_node(&result, fixture.method) == NULL);
    CHECK(st_sema_block_for_node(&result, fixture.method) == NULL);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);
}

static void test_duplicates_shadowing_and_reserved_names(void)
{
    fixture_t fixture;
    st_sema_result_t result;
    const st_sema_reference_t *inner_reference;
    const st_sema_reference_t *outer_reference;
    st_ast_node_t *block;

    CHECK(fixture_init(&fixture,
        "foo: x [ [ :x | x ] value. x ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result));
    CHECK(result.scope_count == 2u && result.binding_count == 2u);
    CHECK(result.scopes[1].parent == 0u);
    CHECK(result.scopes[1].first_binding == 1u);
    CHECK(result.scopes[1].binding_count == 1u);
    block = receiver_of(method_expression(&fixture, 0u));
    inner_reference = st_sema_reference_for_node(
        &result, receiver_of(block->as.block.expressions.items[0]));
    outer_reference = st_sema_reference_for_node(
        &result, receiver_of(method_expression(&fixture, 1u)));
    CHECK(inner_reference != NULL && outer_reference != NULL);
    CHECK(inner_reference != NULL && outer_reference != NULL
        && inner_reference->binding != outer_reference->binding);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo: x bar: x [ x ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(diagnostic_count(&result, ST_SEMA_DIAG_DUPLICATE_DECLARATION) == 1u);
    CHECK(result.diagnostics[0].has_related_span);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo: x [ | x | x ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(diagnostic_count(&result, ST_SEMA_DIAG_DUPLICATE_DECLARATION) == 1u);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ [ :x | | x | x ] ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(diagnostic_count(&result, ST_SEMA_DIAG_DUPLICATE_DECLARATION) == 1u);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo: self [ self ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(diagnostic_count(&result, ST_SEMA_DIAG_RESERVED_DECLARATION) == 1u);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);
}

static void check_readonly_source(const char *source, bool has_super)
{
    fixture_t fixture;
    st_sema_result_t result;
    CHECK(fixture_init(&fixture, source));
    CHECK(analyze(&result, fixture.method, NULL, 0u, has_super) == ST_SEMA_OK);
    CHECK(diagnostic_count(&result, ST_SEMA_DIAG_READONLY_ASSIGNMENT) == 1u);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);
}

static void test_readonly_and_pseudo_variables(void)
{
    fixture_t fixture;
    st_sema_result_t result;
    const st_sema_binding_t *temporary;

    check_readonly_source("foo: x [ x := 1 ]", false);
    check_readonly_source("foo [ [ :x | x := 1 ] ]", false);
    check_readonly_source("foo [ self := 1 ]", false);
    check_readonly_source("foo [ super := 1 ]", true);
    check_readonly_source("foo [ thisContext := 1 ]", false);
    check_readonly_source("foo [ nil := 1 ]", false);
    check_readonly_source("foo [ true := 1 ]", false);
    check_readonly_source("foo [ false := 1 ]", false);

    CHECK(fixture_init(&fixture, "foo [ | x | x := 1 ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result));
    temporary = find_binding(&result, ST_SEMA_BIND_TEMPORARY, "x");
    CHECK(temporary != NULL
        && (temporary->flags & ST_SEMA_BINDING_ASSIGNED) != 0u
        && (temporary->flags & ST_SEMA_BINDING_READONLY) == 0u);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ thisContext ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result) && result.requires_context);
    CHECK(find_binding(&result, ST_SEMA_BIND_THIS_CONTEXT,
                       "thisContext") != NULL);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);
}

static void test_catalog_resolution_and_no_implicit_forward(void)
{
    fixture_t fixture;
    st_sema_result_t result;
    static const st_sema_external_t entries[] = {
        { { "iv", 2u }, ST_SEMA_EXTERNAL_INSTANCE_VARIABLE, 3u, 30u },
        { { "cv", 2u }, ST_SEMA_EXTERNAL_CLASS_VARIABLE, 4u, 40u },
        { { "Global", 6u }, ST_SEMA_EXTERNAL_GLOBAL, 5u, 50u },
        { { "Future", 6u }, ST_SEMA_EXTERNAL_FORWARD_GLOBAL, 6u, 60u }
    };
    const st_sema_binding_t *binding;

    CHECK(fixture_init(&fixture,
        "foo [ iv. cv. Global. Future. Missing ]"));
    CHECK(analyze(&result, fixture.method, entries,
                  sizeof(entries) / sizeof(entries[0]), false) == ST_SEMA_OK);
    CHECK(diagnostic_count(&result, ST_SEMA_DIAG_UNDEFINED_NAME) == 1u);
    CHECK(result.reference_count == 4u);
    binding = find_binding(&result, ST_SEMA_BIND_INSTANCE_VARIABLE, "iv");
    CHECK(binding != NULL && binding->slot == 3u && binding->external_id == 30u);
    CHECK(find_binding(&result, ST_SEMA_BIND_CLASS_VARIABLE, "cv") != NULL);
    CHECK(find_binding(&result, ST_SEMA_BIND_GLOBAL, "Global") != NULL);
    CHECK(find_binding(&result, ST_SEMA_BIND_FORWARD_GLOBAL, "Future") != NULL);
    CHECK(find_binding(&result, ST_SEMA_BIND_GLOBAL, "Missing") == NULL);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ | iv | iv ]"));
    CHECK(analyze(&result, fixture.method, entries,
                  sizeof(entries) / sizeof(entries[0]), false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result));
    CHECK(find_binding(&result, ST_SEMA_BIND_TEMPORARY, "iv") != NULL);
    CHECK(find_binding(&result, ST_SEMA_BIND_INSTANCE_VARIABLE, "iv") == NULL);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    {
        const st_sema_external_t precedence[] = {
            { { "Name", 4u }, ST_SEMA_EXTERNAL_GLOBAL, 8u, 80u },
            { { "Name", 4u }, ST_SEMA_EXTERNAL_INSTANCE_VARIABLE, 9u, 90u }
        };
        CHECK(fixture_init(&fixture, "foo [ Name ]"));
        CHECK(analyze(&result, fixture.method, precedence, 2u, false)
              == ST_SEMA_OK);
        CHECK(st_sema_succeeded(&result));
        CHECK(find_binding(&result, ST_SEMA_BIND_INSTANCE_VARIABLE,
                           "Name") != NULL);
        CHECK(find_binding(&result, ST_SEMA_BIND_GLOBAL, "Name") == NULL);
        st_sema_result_destroy(&result);
        fixture_destroy(&fixture);
    }

    {
        const st_sema_external_t duplicates[] = {
            { { "Name", 4u }, ST_SEMA_EXTERNAL_GLOBAL, 0u, 1u },
            { { "Name", 4u }, ST_SEMA_EXTERNAL_GLOBAL, 0u, 2u }
        };
        CHECK(fixture_init(&fixture, "foo [ Name ]"));
        CHECK(analyze(&result, fixture.method, duplicates, 2u, false)
              == ST_SEMA_ERR_INVALID_ARGUMENT);
        CHECK(!st_sema_succeeded(&result));
        st_sema_result_destroy(&result);
        fixture_destroy(&fixture);
    }
}

static bool block_captures(const st_sema_result_t *result,
                           const st_sema_block_t *block,
                           st_sema_binding_id_t binding,
                           st_sema_capture_mode_t mode)
{
    size_t index;
    if (block == NULL) return false;
    for (index = 0u; index < block->capture_count; index++) {
        const st_sema_capture_t *capture =
            &result->captures[block->capture_offset + index];
        if (capture->binding == binding && capture->mode == mode) return true;
    }
    return false;
}

static void test_transitive_captures_cells_and_siblings(void)
{
    fixture_t fixture;
    st_sema_result_t result;
    const st_sema_binding_t *x;
    st_ast_node_t *outer;
    st_ast_node_t *inner;
    const st_sema_block_t *outer_info;
    const st_sema_block_t *inner_info;
    size_t x_id;

    CHECK(fixture_init(&fixture,
        "foo [ | x | x := 1. [ x := x + 1. [ x ] ] ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result));
    x = find_binding(&result, ST_SEMA_BIND_TEMPORARY, "x");
    CHECK(x != NULL);
    x_id = binding_index(&result, x);
    CHECK(x != NULL
        && (x->flags & (ST_SEMA_BINDING_ASSIGNED
                        | ST_SEMA_BINDING_CAPTURED
                        | ST_SEMA_BINDING_NEEDS_CELL))
           == (ST_SEMA_BINDING_ASSIGNED
               | ST_SEMA_BINDING_CAPTURED
               | ST_SEMA_BINDING_NEEDS_CELL));
    outer = receiver_of(method_expression(&fixture, 1u));
    inner = receiver_of(outer->as.block.expressions.items[1]);
    outer_info = st_sema_block_for_node(&result, outer);
    inner_info = st_sema_block_for_node(&result, inner);
    CHECK(outer_info != NULL && inner_info != NULL);
    CHECK(inner_info != NULL && inner_info->parent == 0u);
    CHECK(block_captures(&result, outer_info, (st_sema_binding_id_t)x_id,
                         ST_SEMA_CAPTURE_CELL));
    CHECK(block_captures(&result, inner_info, (st_sema_binding_id_t)x_id,
                         ST_SEMA_CAPTURE_CELL));
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    /* Final capture representation is computed after the whole method, so a
     * write after closure creation must still upgrade the capture to a cell. */
    CHECK(fixture_init(&fixture, "foo [ | x | [ x ]. x := 2 ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result));
    x = find_binding(&result, ST_SEMA_BIND_TEMPORARY, "x");
    x_id = binding_index(&result, x);
    CHECK(x != NULL && (x->flags & ST_SEMA_BINDING_NEEDS_CELL) != 0u);
    CHECK(block_captures(&result, &result.blocks[0], (st_sema_binding_id_t)x_id,
                         ST_SEMA_CAPTURE_CELL));
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ | x | [ x ]. [ x ] ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result) && result.block_count == 2u);
    x = find_binding(&result, ST_SEMA_BIND_TEMPORARY, "x");
    x_id = binding_index(&result, x);
    CHECK(block_captures(&result, &result.blocks[0], (st_sema_binding_id_t)x_id,
                         ST_SEMA_CAPTURE_VALUE));
    CHECK(block_captures(&result, &result.blocks[1], (st_sema_binding_id_t)x_id,
                         ST_SEMA_CAPTURE_VALUE));
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ [ | y | [ y ] ] ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result) && result.block_count == 2u);
    x = find_binding(&result, ST_SEMA_BIND_TEMPORARY, "y");
    x_id = binding_index(&result, x);
    CHECK(result.blocks[0].capture_count == 0u);
    CHECK(block_captures(&result, &result.blocks[1], (st_sema_binding_id_t)x_id,
                         ST_SEMA_CAPTURE_VALUE));
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture,
        "foo [ | recursive | recursive := [ recursive value ] ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result));
    x = find_binding(&result, ST_SEMA_BIND_TEMPORARY, "recursive");
    x_id = binding_index(&result, x);
    CHECK(x != NULL && (x->flags & ST_SEMA_BINDING_NEEDS_CELL) != 0u);
    CHECK(block_captures(&result, &result.blocks[0], (st_sema_binding_id_t)x_id,
                         ST_SEMA_CAPTURE_CELL));
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);
}

static void test_ivar_self_capture_super_and_context(void)
{
    fixture_t fixture;
    st_sema_result_t result;
    static const st_sema_external_t ivar = {
        { "value", 5u }, ST_SEMA_EXTERNAL_INSTANCE_VARIABLE, 7u, 70u
    };
    const st_sema_binding_t *self;
    size_t self_id;

    CHECK(fixture_init(&fixture, "foo [ [ [ value ] ] ]"));
    CHECK(analyze(&result, fixture.method, &ivar, 1u, true) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result) && result.block_count == 2u);
    self = find_binding(&result, ST_SEMA_BIND_SELF, "self");
    self_id = binding_index(&result, self);
    CHECK(self != NULL && (self->flags & ST_SEMA_BINDING_CAPTURED) != 0u);
    CHECK(block_captures(&result, &result.blocks[0],
                         (st_sema_binding_id_t)self_id, ST_SEMA_CAPTURE_SELF));
    CHECK(block_captures(&result, &result.blocks[1],
                         (st_sema_binding_id_t)self_id, ST_SEMA_CAPTURE_SELF));
    CHECK(find_binding(&result, ST_SEMA_BIND_INSTANCE_VARIABLE, "value")
          != NULL);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ super initialize ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(diagnostic_count(&result, ST_SEMA_DIAG_INVALID_SUPER) == 1u);
    CHECK(result.reference_count == 1u
        && result.references[0].access == ST_SEMA_ACCESS_SUPER_RECEIVER);
    st_sema_result_destroy(&result);
    CHECK(analyze(&result, fixture.method, NULL, 0u, true) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result));
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ [ thisContext ] ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result) && result.requires_context);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);
}

static void test_returns_and_malformed_ast(void)
{
    fixture_t fixture;
    st_sema_result_t result;
    st_ast_node_t *block;
    st_ast_node_t *message;

    CHECK(fixture_init(&fixture, "foo [ ^1 ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result) && result.return_count == 1u);
    CHECK(result.returns[0].kind == ST_SEMA_RETURN_LOCAL_METHOD);
    CHECK(!result.requires_context && !result.may_be_nonlocal_return_home);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ [ [ ^1 ] ] ]"));
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&result) && result.return_count == 1u);
    CHECK(result.returns[0].kind == ST_SEMA_RETURN_HOME_METHOD);
    CHECK(result.requires_context && result.may_be_nonlocal_return_home);
    CHECK(result.returns[0].block == 1u);
    CHECK(!result.blocks[0].has_nonlocal_return);
    CHECK(result.blocks[1].has_nonlocal_return);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo: x [ x ]"));
    fixture.method->as.method.body->as.block.arguments.count = 0u;
    CHECK(analyze(&result, fixture.method, NULL, 0u, false) == ST_SEMA_OK);
    CHECK(diagnostic_count(&result, ST_SEMA_DIAG_MALFORMED_AST) == 1u);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ super initialize ]"));
    block = fixture.method->as.method.body;
    message = block->as.block.expressions.items[0]
        ->as.expression.messages.items[0];
    message->as.message.super_send = false;
    CHECK(analyze(&result, fixture.method, NULL, 0u, true) == ST_SEMA_OK);
    CHECK(diagnostic_count(&result, ST_SEMA_DIAG_MALFORMED_AST) == 1u);
    st_sema_result_destroy(&result);
    fixture_destroy(&fixture);
}

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t outstanding;
} fault_allocator_t;

static void *fault_allocate(void *user, size_t size)
{
    fault_allocator_t *fault = user;
    void *memory;
    if (fault->calls++ >= fault->fail_at) return NULL;
    memory = malloc(size);
    if (memory != NULL) fault->outstanding++;
    return memory;
}

static void fault_deallocate(void *user, void *pointer)
{
    fault_allocator_t *fault = user;
    if (pointer != NULL) {
        CHECK(fault->outstanding != 0u);
        if (fault->outstanding != 0u) fault->outstanding--;
        free(pointer);
    }
}

static void test_allocator_fault_injection(void)
{
    fixture_t fixture;
    static const st_sema_external_t entries[] = {
        { { "iv", 2u }, ST_SEMA_EXTERNAL_INSTANCE_VARIABLE, 0u, 1u },
        { { "Global", 6u }, ST_SEMA_EXTERNAL_GLOBAL, 0u, 2u }
    };
    size_t fail_at;
    bool reached_success = false;

    CHECK(fixture_init(&fixture,
        "foo: arg [ | x | x := 1. [ [ x + arg + iv + Global ] ]. ^x ]"));
    for (fail_at = 0u; fail_at < 256u; fail_at++) {
        fault_allocator_t fault;
        st_sema_catalog_t catalog;
        st_sema_result_t result;
        st_sema_status_t status;
        memset(&fault, 0, sizeof(fault));
        fault.fail_at = fail_at;
        memset(&catalog, 0, sizeof(catalog));
        catalog.entries = entries;
        catalog.count = sizeof(entries) / sizeof(entries[0]);
        catalog.has_lexical_super = true;
        catalog.allocator.allocate = fault_allocate;
        catalog.allocator.deallocate = fault_deallocate;
        catalog.allocator.user = &fault;
        st_sema_result_init(&result);
        status = st_sema_analyze_method(&result, fixture.method, &catalog);
        if (status == ST_SEMA_OK) {
            CHECK(st_sema_succeeded(&result));
            reached_success = true;
        } else {
            CHECK(status == ST_SEMA_ERR_OUT_OF_MEMORY);
        }
        st_sema_result_destroy(&result);
        CHECK(fault.outstanding == 0u);
        if (reached_success) break;
    }
    CHECK(reached_success);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ 1 ]"));
    {
        fault_allocator_t fault;
        st_sema_result_t result;
        st_sema_catalog_t catalog;
        memset(&fault, 0, sizeof(fault));
        memset(&catalog, 0, sizeof(catalog));
        catalog.allocator.allocate = fault_allocate;
        catalog.allocator.user = &fault;
        st_sema_result_init(&result);
        CHECK(st_sema_analyze_method(&result, fixture.method, &catalog)
              == ST_SEMA_ERR_INVALID_ARGUMENT);
        st_sema_result_destroy(&result);
        CHECK(fault.outstanding == 0u);
    }
    fixture_destroy(&fixture);
}

static void test_public_strings_and_argument_validation(void)
{
    st_sema_result_t result;
    st_sema_catalog_t catalog;
    memset(&catalog, 0, sizeof(catalog));
    st_sema_result_init(&result);
    CHECK(st_sema_analyze_method(&result, NULL, &catalog)
          == ST_SEMA_ERR_INVALID_ARGUMENT);
    CHECK(!st_sema_succeeded(&result));
    CHECK(strcmp(st_sema_status_string(ST_SEMA_ERR_OUT_OF_MEMORY),
                 "out of memory") == 0);
    CHECK(strcmp(st_sema_diagnostic_string(ST_SEMA_DIAG_UNDEFINED_NAME),
                 "undefined name") == 0);
    CHECK(st_sema_reference_for_node(NULL, NULL) == NULL);
    CHECK(st_sema_block_for_node(NULL, NULL) == NULL);
    st_sema_result_destroy(&result);
    CHECK(result.implementation == NULL && result.binding_count == 0u);
}

int main(void)
{
    test_dense_bindings_types_and_references();
    test_duplicates_shadowing_and_reserved_names();
    test_readonly_and_pseudo_variables();
    test_catalog_resolution_and_no_implicit_forward();
    test_transitive_captures_cells_and_siblings();
    test_ivar_self_capture_super_and_context();
    test_returns_and_malformed_ast();
    test_allocator_fault_injection();
    test_public_strings_and_argument_validation();
    if (failures != 0u) {
        fprintf(stderr, "smalltalk sema: %u failure(s)\n", failures);
        return 1;
    }
    puts("smalltalk sema: all tests passed");
    return 0;
}
