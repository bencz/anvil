#include "st_parser.h"

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
} fixture_t;

static bool fixture_init(fixture_t *fixture, const char *source)
{
    memset(fixture, 0, sizeof(*fixture));
    if (!st_ast_unit_init(&fixture->unit, "parser-test.st")) {
        return false;
    }
    if (!st_parser_init_cstr(&fixture->parser, &fixture->unit, source)) {
        st_ast_unit_destroy(&fixture->unit);
        return false;
    }
    return true;
}

static void fixture_destroy(fixture_t *fixture)
{
    st_parser_destroy(&fixture->parser);
    st_ast_unit_destroy(&fixture->unit);
}

static bool text_is(st_ast_string_t string, const char *text)
{
    size_t length = strlen(text);
    return string.length == length
        && memcmp(string.data, text, length) == 0;
}

static st_ast_node_t *only_expression(st_ast_node_t *method)
{
    CHECK(method != NULL && method->kind == ST_AST_METHOD);
    if (method == NULL || method->kind != ST_AST_METHOD
            || method->as.method.body == NULL) {
        return NULL;
    }
    CHECK(method->as.method.body->as.block.expressions.count == 1u);
    return method->as.method.body->as.block.expressions.count == 1u
        ? method->as.method.body->as.block.expressions.items[0] : NULL;
}

static void test_basic_method_and_literals(void)
{
    fixture_t fixture;
    st_ast_node_t *method;
    st_ast_node_t *expression;
    static const struct {
        const char *source;
        st_ast_kind_t kind;
    } cases[] = {
        { "foo [ 'hi' ]", ST_AST_STRING },
        { "foo [ #sym ]", ST_AST_SYMBOL },
        { "foo [ $a ]", ST_AST_CHARACTER },
        { "foo [ true ]", ST_AST_TRUE },
        { "foo [ false ]", ST_AST_FALSE },
        { "foo [ nil ]", ST_AST_NIL },
        { "foo [ Object ]", ST_AST_VARIABLE }
    };
    size_t index;

    CHECK(fixture_init(&fixture, "foo [ ^42 ]"));
    method = st_parse_method(&fixture.parser);
    CHECK(method != NULL);
    CHECK(st_parser_at_end(&fixture.parser));
    CHECK(text_is(method->as.method.selector, "foo"));
    CHECK(method->as.method.arguments.count == 0u);
    expression = only_expression(method);
    CHECK(expression != NULL && expression->as.expression.returns);
    CHECK(expression->as.expression.receiver->kind == ST_AST_INTEGER);
    CHECK(text_is(expression->as.expression.receiver->as.integer.spelling,
                  "42"));
    CHECK(expression->as.expression.receiver->as.integer.radix == 10u);
    fixture_destroy(&fixture);

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); index++) {
        CHECK(fixture_init(&fixture, cases[index].source));
        method = st_parse_method(&fixture.parser);
        expression = only_expression(method);
        CHECK(expression != NULL);
        if (expression != NULL) {
            CHECK(expression->as.expression.receiver->kind == cases[index].kind);
        }
        fixture_destroy(&fixture);
    }
}

static void test_method_patterns_and_class_side(void)
{
    fixture_t fixture;
    st_ast_node_t *method;

    CHECK(fixture_init(&fixture, "bar: a and: b [ ^a ]"));
    method = st_parse_method(&fixture.parser);
    CHECK(method != NULL && text_is(method->as.method.selector, "bar:and:"));
    CHECK(method->as.method.arguments.count == 2u);
    CHECK(text_is(method->as.method.arguments.items[0]->as.variable.name, "a"));
    CHECK(text_is(method->as.method.arguments.items[1]->as.variable.name, "b"));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "+ rhs [ ^rhs ]"));
    method = st_parse_method(&fixture.parser);
    CHECK(method != NULL && text_is(method->as.method.selector, "+"));
    CHECK(method->as.method.arguments.count == 1u);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "class answer [ ^42 ]"));
    method = st_parse_method(&fixture.parser);
    CHECK(method != NULL && method->as.method.class_side);
    CHECK(text_is(method->as.method.class_name, "class"));
    CHECK(text_is(method->as.method.selector, "answer"));
    fixture_destroy(&fixture);
}

static void test_message_precedence_chains_and_cascades(void)
{
    fixture_t fixture;
    st_ast_node_t *expression;
    st_ast_node_t *message;

    CHECK(fixture_init(&fixture, "foo [ 3 + 4 ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    CHECK(expression->as.expression.messages.count == 1u);
    message = expression->as.expression.messages.items[0];
    CHECK(text_is(message->as.message.selector, "+"));
    CHECK(message->as.message.arguments.count == 1u);
    CHECK(message->as.message.arguments.items[0]->kind == ST_AST_INTEGER);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ self at: 1 put: 2 ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    message = expression->as.expression.messages.items[0];
    CHECK(text_is(message->as.message.selector, "at:put:"));
    CHECK(message->as.message.arguments.count == 2u);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ 3 factorial printString ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    CHECK(expression->as.expression.receiver->kind == ST_AST_EXPRESSION);
    CHECK(text_is(expression->as.expression.messages.items[0]->as.message.selector,
                  "printString"));
    CHECK(text_is(expression->as.expression.receiver->as.expression.messages
                      .items[0]->as.message.selector,
                  "factorial"));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ x add: 1; add: 2; add: 3 ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    CHECK(expression->as.expression.receiver->kind == ST_AST_VARIABLE);
    CHECK(text_is(expression->as.expression.receiver->as.variable.name, "x"));
    CHECK(expression->as.expression.messages.count == 3u);
    CHECK(!expression->as.expression.messages.items[0]
               ->as.message.starts_cascade);
    CHECK(expression->as.expression.messages.items[1]
              ->as.message.starts_cascade);
    CHECK(expression->as.expression.messages.items[2]
              ->as.message.starts_cascade);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ (x foo) bar; baz ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    CHECK(expression->as.expression.receiver->kind == ST_AST_EXPRESSION);
    CHECK(expression->as.expression.receiver->as.expression.parenthesized);
    CHECK(text_is(expression->as.expression.receiver->as.expression.receiver
                      ->as.variable.name,
                  "x"));
    CHECK(expression->as.expression.receiver->as.expression.messages.count
          == 1u);
    CHECK(text_is(expression->as.expression.receiver->as.expression.messages
                      .items[0]->as.message.selector,
                  "foo"));
    CHECK(expression->as.expression.messages.count == 2u);
    CHECK(text_is(expression->as.expression.messages.items[0]
                      ->as.message.selector,
                  "bar"));
    CHECK(expression->as.expression.messages.items[1]
              ->as.message.starts_cascade);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ x foo bar; baz ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    CHECK(expression->as.expression.receiver->kind == ST_AST_EXPRESSION);
    CHECK(text_is(expression->as.expression.receiver->as.expression.receiver->as.variable.name, "x"));
    CHECK(text_is(expression->as.expression.receiver->as.expression.messages.items[0]
                      ->as.message.selector,
                  "foo"));
    CHECK(expression->as.expression.messages.count == 2u);
    CHECK(text_is(expression->as.expression.messages.items[0]
                      ->as.message.selector,
                  "bar"));
    CHECK(text_is(expression->as.expression.messages.items[1]
                      ->as.message.selector,
                  "baz"));
    CHECK(!expression->as.expression.messages.items[0]
               ->as.message.starts_cascade);
    CHECK(expression->as.expression.messages.items[1]
              ->as.message.starts_cascade);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ super initialize; reset ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    CHECK(expression->as.expression.messages.count == 2u);
    CHECK(expression->as.expression.messages.items[0]->as.message.super_send);
    CHECK(expression->as.expression.messages.items[1]->as.message.super_send);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ OrderedCollection new add: 1; yourself ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    CHECK(expression->as.expression.messages.count == 2u);
    CHECK(text_is(expression->as.expression.messages.items[0]->as.message.selector, "add:"));
    CHECK(expression->as.expression.messages.items[1]->as.message.starts_cascade);
    CHECK(expression->as.expression.receiver->kind == ST_AST_EXPRESSION);
    CHECK(text_is(expression->as.expression.receiver->as.expression.messages.items[0]->as.message.selector, "new"));
    CHECK(text_is(expression->as.expression.receiver->as.expression.receiver->as.variable.name, "OrderedCollection"));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ super factory initialize; yourself ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    CHECK(expression->as.expression.receiver->as.expression.messages.items[0]->as.message.super_send);
    CHECK(!expression->as.expression.messages.items[0]->as.message.super_send);
    CHECK(!expression->as.expression.messages.items[1]->as.message.super_send);
    fixture_destroy(&fixture);
}

static void test_blocks_temporaries_and_assignment(void)
{
    fixture_t fixture;
    st_ast_node_t *method;
    st_ast_node_t *expression;
    st_ast_node_t *block;

    CHECK(fixture_init(&fixture, "foo [ | x | x := 5 ]"));
    method = st_parse_method(&fixture.parser);
    CHECK(method->as.method.body->as.block.temporaries.count == 1u);
    expression = only_expression(method);
    CHECK(expression->as.expression.assignments.count == 1u);
    CHECK(text_is(expression->as.expression.assignments.items[0]->as.variable.name,
                  "x"));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ [:a :b | | t | t := a + b. ^t] ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    block = expression->as.expression.receiver;
    CHECK(block->kind == ST_AST_BLOCK);
    CHECK(block->as.block.arguments.count == 2u);
    CHECK(block->as.block.temporaries.count == 1u);
    CHECK(block->as.block.expressions.count == 2u);
    CHECK(block->as.block.expressions.items[1]->as.expression.returns);
    fixture_destroy(&fixture);
}

static void test_exact_numeric_and_character_literals(void)
{
    fixture_t fixture;
    st_ast_node_t *literal;

    CHECK(fixture_init(&fixture,
        "foo [ 1234567890123456789012345678901234567890 ]"));
    literal = only_expression(st_parse_method(&fixture.parser))
        ->as.expression.receiver;
    CHECK(literal->kind == ST_AST_INTEGER);
    CHECK(text_is(literal->as.integer.spelling,
        "1234567890123456789012345678901234567890"));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ 16r-FF ]"));
    literal = only_expression(st_parse_method(&fixture.parser))
        ->as.expression.receiver;
    CHECK(literal->kind == ST_AST_INTEGER && literal->as.integer.negative);
    CHECK(literal->as.integer.radix == 16u);
    CHECK(text_is(literal->as.integer.spelling, "FF"));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ -1.25e3. 3.1400s4 ]"));
    st_ast_node_t *method = st_parse_method(&fixture.parser);
    CHECK(method->as.method.body->as.block.expressions.count == 2u);
    literal = method->as.method.body->as.block.expressions.items[0]
        ->as.expression.receiver;
    CHECK(literal->kind == ST_AST_FLOAT && literal->as.real.negative);
    CHECK(text_is(literal->as.real.spelling, "1.25e3"));
    literal = method->as.method.body->as.block.expressions.items[1]
        ->as.expression.receiver;
    CHECK(literal->kind == ST_AST_SCALED_DECIMAL);
    CHECK(text_is(literal->as.real.spelling, "3.1400s4"));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ $<16r1F642> ]"));
    literal = only_expression(st_parse_method(&fixture.parser))
        ->as.expression.receiver;
    CHECK(literal->kind == ST_AST_CHARACTER);
    CHECK(literal->as.character == UINT32_C(0x1F642));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ $\xF0\x9F\x99\x82 ]"));
    literal = only_expression(st_parse_method(&fixture.parser))
        ->as.expression.receiver;
    CHECK(literal->kind == ST_AST_CHARACTER);
    CHECK(literal->as.character == UINT32_C(0x1F642));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ $\xE0\xA0\x80 ]"));
    literal = only_expression(st_parse_method(&fixture.parser))
        ->as.expression.receiver;
    CHECK(literal->kind == ST_AST_CHARACTER);
    CHECK(literal->as.character == UINT32_C(0x800));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ $\xF4\x8F\xBF\xBF ]"));
    literal = only_expression(st_parse_method(&fixture.parser))
        ->as.expression.receiver;
    CHECK(literal->kind == ST_AST_CHARACTER);
    CHECK(literal->as.character == UINT32_C(0x10FFFF));
    fixture_destroy(&fixture);
}

static void test_complete_literal_and_parenthesized_spans(void)
{
    fixture_t fixture;
    st_ast_node_t *expression;
    st_ast_node_t *literal;

    CHECK(fixture_init(&fixture, "foo [ -1 ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    literal = expression->as.expression.receiver;
    CHECK(literal->span.begin.offset == 6u);
    CHECK(literal->span.end.offset == 8u);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "foo [ ( -7 ) ]"));
    expression = only_expression(st_parse_method(&fixture.parser));
    CHECK(expression->span.begin.offset == 6u);
    CHECK(expression->span.end.offset == 12u);
    literal = expression->as.expression.receiver;
    CHECK(literal->span.begin.offset == 8u);
    CHECK(literal->span.end.offset == 10u);
    fixture_destroy(&fixture);
}

static void test_literal_arrays(void)
{
    fixture_t fixture;
    st_ast_node_t *array;

    CHECK(fixture_init(&fixture,
        "foo [ #(1 $a 's' #sym implicit keyword: -7 - #(2) (3)) ]"));
    array = only_expression(st_parse_method(&fixture.parser))
        ->as.expression.receiver;
    CHECK(array->kind == ST_AST_LITERAL_ARRAY);
    CHECK(array->as.array.elements.count == 10u);
    CHECK(array->as.array.elements.items[3]->kind == ST_AST_SYMBOL);
    CHECK(array->as.array.elements.items[4]->kind == ST_AST_SYMBOL);
    CHECK(text_is(array->as.array.elements.items[4]->as.text, "implicit"));
    CHECK(text_is(array->as.array.elements.items[5]->as.text, "keyword:"));
    CHECK(array->as.array.elements.items[6]->kind == ST_AST_INTEGER);
    CHECK(array->as.array.elements.items[6]->as.integer.negative);
    CHECK(array->as.array.elements.items[7]->kind == ST_AST_SYMBOL);
    CHECK(text_is(array->as.array.elements.items[7]->as.text, "-"));
    CHECK(array->as.array.elements.items[8]->kind == ST_AST_LITERAL_ARRAY);
    CHECK(array->as.array.elements.items[9]->kind == ST_AST_LITERAL_ARRAY);
    fixture_destroy(&fixture);
}

static void test_classes_namespaces_extensions_and_pragmas(void)
{
    fixture_t fixture;
    st_ast_node_t *class_node;
    st_ast_node_t *method;

    CHECK(fixture_init(&fixture,
        "Foo := Object [ <shape: #fixed> | iv | "
        "bar [ ^1 ] class baz: x [ ^x ] ]"));
    class_node = st_parse_class(&fixture.parser);
    CHECK(class_node != NULL && st_parser_at_end(&fixture.parser));
    CHECK(text_is(class_node->as.class_decl.name->as.variable.name, "Foo"));
    CHECK(text_is(class_node->as.class_decl.super_name->as.variable.name, "Object"));
    CHECK(class_node->as.class_decl.pragmas.count == 1u);
    CHECK(class_node->as.class_decl.variables.count == 1u);
    CHECK(class_node->as.class_decl.methods.count == 2u);
    method = class_node->as.class_decl.methods.items[1];
    CHECK(method->as.method.class_side);
    CHECK(text_is(method->as.method.selector, "baz:"));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture,
        "Vec3 := Value [ | x::Float64 y::Float64 | x [ ^x ] ]"));
    class_node = st_parse_class(&fixture.parser);
    CHECK(class_node != NULL);
    CHECK(class_node->as.class_decl.variables.count == 2u);
    CHECK(class_node->as.class_decl.variables.items[0]->as.variable.has_type);
    CHECK(text_is(class_node->as.class_decl.variables.items[0]
                      ->as.variable.type_name,
                  "Float64"));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "Foo extend [ probe [ ^1 ] ]"));
    class_node = st_parse_class(&fixture.parser);
    CHECK(class_node != NULL && class_node->as.class_decl.is_extension);
    CHECK(class_node->as.class_decl.methods.count == 1u);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture,
        "Outer := Namespace [ A := Object [ ] B := A [ ] ]"));
    class_node = st_parse_class(&fixture.parser);
    CHECK(class_node != NULL && class_node->as.class_decl.is_namespace);
    CHECK(class_node->as.class_decl.members.count == 2u);
    fixture_destroy(&fixture);
}

static void test_compilation_unit_and_file(void)
{
    fixture_t fixture;
    st_ast_unit_t unit;
    st_parser_t parser;
    FILE *file;

    CHECK(fixture_init(&fixture, "A := Object [ ] B := A [ ]"));
    CHECK(st_parse_compilation_unit(&fixture.parser));
    CHECK(fixture.unit.declarations.count == 2u);
    CHECK(fixture.unit.forms.count == 2u);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture,
        "A := Object [ ] [ | x | x := A new ] A extend [ probe [ ^1 ] ]"));
    CHECK(st_parse_source_unit(&fixture.parser));
    CHECK(fixture.unit.forms.count == 3u);
    CHECK(fixture.unit.declarations.count == 2u);
    CHECK(fixture.unit.forms.items[0]->kind == ST_AST_CLASS);
    CHECK(fixture.unit.forms.items[1]->kind == ST_AST_BLOCK);
    CHECK(fixture.unit.forms.items[2]->as.class_decl.is_extension);
    fixture_destroy(&fixture);

    file = tmpfile();
    CHECK(file != NULL);
    if (file == NULL) return;
    CHECK(fputs("fileMethod [ ^7 ]", file) >= 0);
    rewind(file);
    CHECK(st_ast_unit_init(&unit, "file.st"));
    CHECK(st_parser_init_file(&parser, &unit, file));
    CHECK(st_parse_method(&parser) != NULL);
    CHECK(st_parser_at_end(&parser));
    st_parser_destroy(&parser);
    st_ast_unit_destroy(&unit);
    CHECK(fclose(file) == 0);
}

static char *nested_method_source(const char *opening, char closing,
                                  size_t depth)
{
    static const char prefix[] = "foo [ ";
    static const char suffix[] = " ]";
    size_t opening_length = strlen(opening);
    size_t length = sizeof(prefix) - 1u + depth * opening_length + 1u
        + depth + sizeof(suffix) - 1u;
    char *source = malloc(length + 1u);
    char *cursor;
    size_t index;
    if (source == NULL) return NULL;
    cursor = source;
    memcpy(cursor, prefix, sizeof(prefix) - 1u);
    cursor += sizeof(prefix) - 1u;
    for (index = 0u; index < depth; index++) {
        memcpy(cursor, opening, opening_length);
        cursor += opening_length;
    }
    *cursor++ = '1';
    for (index = 0u; index < depth; index++) *cursor++ = closing;
    memcpy(cursor, suffix, sizeof(suffix));
    return source;
}

static void check_method_nesting(const char *opening, char closing)
{
    fixture_t fixture;
    char *source = nested_method_source(opening, closing, 3u);
    CHECK(source != NULL);
    if (source == NULL) return;
    CHECK(fixture_init(&fixture, source));
    CHECK(st_parser_set_nesting_limit(&fixture.parser, 4u));
    CHECK(st_parse_method(&fixture.parser) != NULL);
    CHECK(st_parser_at_end(&fixture.parser));
    fixture_destroy(&fixture);
    free(source);

    source = nested_method_source(opening, closing, 4u);
    CHECK(source != NULL);
    if (source == NULL) return;
    CHECK(fixture_init(&fixture, source));
    CHECK(st_parser_set_nesting_limit(&fixture.parser, 4u));
    CHECK(st_parse_method(&fixture.parser) == NULL);
    CHECK(st_parser_status(&fixture.parser) == ST_PARSE_ERR_NESTING_LIMIT);
    CHECK(st_parser_error(&fixture.parser)->span.begin.offset > 0u);
    CHECK(fixture.parser.nesting_depth == 0u);
    fixture_destroy(&fixture);
    free(source);
}

static void test_nesting_limits(void)
{
    fixture_t fixture;

    check_method_nesting("(", ')');
    check_method_nesting("[", ']');
    check_method_nesting("#(", ')');

    CHECK(fixture_init(&fixture,
        "A := Namespace [ B := Namespace [ C := Object [ ] ] ]"));
    CHECK(st_parser_set_nesting_limit(&fixture.parser, 3u));
    CHECK(st_parse_class(&fixture.parser) != NULL);
    CHECK(st_parser_at_end(&fixture.parser));
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture,
        "A := Namespace [ B := Namespace [ C := Namespace [ "
        "D := Object [ ] ] ] ]"));
    CHECK(st_parser_set_nesting_limit(&fixture.parser, 3u));
    CHECK(st_parse_class(&fixture.parser) == NULL);
    CHECK(st_parser_status(&fixture.parser) == ST_PARSE_ERR_NESTING_LIMIT);
    CHECK(fixture.parser.nesting_depth == 0u);
    fixture_destroy(&fixture);
}

static void test_long_cascade_chain_is_iterative(void)
{
    static const char prefix[] = "foo [ x ";
    static const char suffix[] = "; z ]";
    const size_t sends = 4096u;
    size_t length = sizeof(prefix) - 1u + sends * 2u + sizeof(suffix) - 1u;
    char *source = malloc(length + 1u);
    char *cursor;
    size_t index;
    fixture_t fixture;
    st_ast_node_t *expression;

    CHECK(source != NULL);
    if (source == NULL) return;
    cursor = source;
    memcpy(cursor, prefix, sizeof(prefix) - 1u);
    cursor += sizeof(prefix) - 1u;
    for (index = 0u; index < sends; index++) {
        *cursor++ = 'm';
        *cursor++ = ' ';
    }
    memcpy(cursor, suffix, sizeof(suffix));

    CHECK(fixture_init(&fixture, source));
    expression = only_expression(st_parse_method(&fixture.parser));
    CHECK(expression != NULL);
    CHECK(expression->as.expression.messages.count == 2u);
    CHECK(expression->as.expression.messages.items[1]
              ->as.message.starts_cascade);

    st_ast_node_t *receiver = expression->as.expression.receiver;

    for (index = 1u; index < sends; index++) {
        CHECK(receiver->kind == ST_AST_EXPRESSION);
        CHECK(receiver->as.expression.messages.count == 1u);
        receiver = receiver->as.expression.receiver;
    }

    CHECK(receiver->kind == ST_AST_VARIABLE);
    CHECK(text_is(receiver->as.variable.name, "x"));
    fixture_destroy(&fixture);
    free(source);
}

static void test_top_level_separators_and_reinit(void)
{
    fixture_t fixture;
    st_ast_unit_t unit;
    st_parser_t parser;
    st_ast_node_t *method;

    CHECK(fixture_init(&fixture, "[1] [2]"));
    CHECK(st_parse_source_unit(&fixture.parser));
    CHECK(fixture.unit.forms.count == 2u);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "[1]\"separator\"[2]"));
    CHECK(st_parse_source_unit(&fixture.parser));
    CHECK(fixture.unit.forms.count == 2u);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "1.'two'"));
    CHECK(st_parse_source_unit(&fixture.parser));
    CHECK(fixture.unit.forms.count == 2u);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture, "[1][2]"));
    CHECK(!st_parse_source_unit(&fixture.parser));
    CHECK(st_parser_status(&fixture.parser) == ST_PARSE_ERR_MISSING_SEPARATOR);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture,
        "A := Object [ ]B := Object [ ]"));
    CHECK(!st_parse_compilation_unit(&fixture.parser));
    CHECK(st_parser_status(&fixture.parser) == ST_PARSE_ERR_MISSING_SEPARATOR);
    fixture_destroy(&fixture);

    CHECK(st_ast_unit_init(&unit, "reinit.st"));
    CHECK(st_parser_init_cstr(&parser, &unit, "first [ ^1 ]"));
    CHECK(st_parser_reinit_cstr(&parser, &unit, "second [ ^2 ]"));
    method = st_parse_method(&parser);
    CHECK(method != NULL);
    CHECK(text_is(method->as.method.selector, "second"));
    CHECK(st_parser_at_end(&parser));
    st_parser_destroy(&parser);
    st_ast_unit_destroy(&unit);

    CHECK(st_ast_unit_init(&unit, "failed-init.st"));
    CHECK(!st_parser_init_cstr(&parser, &unit, "$\xE0\x80\x80"));
    CHECK(st_parser_status(&parser) == ST_PARSE_ERR_INVALID_CHARACTER);
    CHECK(st_parser_error(&parser)->span.begin.offset == 0u);
    st_parser_destroy(&parser);
    CHECK(!st_parser_init_cstr(&parser, &unit, NULL));
    CHECK(st_parser_status(&parser) == ST_PARSE_ERR_INVALID_ARGUMENT);
    CHECK(st_parser_error(&parser)->lexer_status
          == ST_LEXER_ERR_INVALID_ARGUMENT);
    st_parser_destroy(&parser);
    st_ast_unit_destroy(&unit);
}

static void expect_parse_error(const char *source, bool parse_class,
                               st_parse_status_t expected_status)
{
    fixture_t fixture;
    st_ast_node_t *node;
    CHECK(fixture_init(&fixture, source));
    node = parse_class ? st_parse_class(&fixture.parser)
                       : st_parse_method(&fixture.parser);
    if (node != NULL || st_parser_status(&fixture.parser) != expected_status) {
        fprintf(stderr, "unexpected parse result for: %s (got %s, expected %s)\n",
                source,
                st_parse_status_string(st_parser_status(&fixture.parser)),
                st_parse_status_string(expected_status));
    }
    CHECK(node == NULL);
    CHECK(st_parser_status(&fixture.parser) == expected_status);
    CHECK(st_parser_error(&fixture.parser)->span.begin.line >= 1u);
    /* Errors are sticky: another parse cannot silently recover a partial AST. */
    CHECK(st_parse_method(&fixture.parser) == NULL);
    CHECK(st_parser_status(&fixture.parser) == expected_status);
    fixture_destroy(&fixture);
}

static void test_errors(void)
{
    expect_parse_error("foo [ 'unterminated ]", false,
                       ST_PARSE_ERR_UNEXPECTED_TOKEN);
    expect_parse_error("foo [ x := ]", false,
                       ST_PARSE_ERR_UNEXPECTED_TOKEN);
    expect_parse_error("foo [ (1 + 2 ]", false,
                       ST_PARSE_ERR_UNEXPECTED_TOKEN);
    expect_parse_error("123 [ ]", false,
                       ST_PARSE_ERR_UNEXPECTED_TOKEN);
    expect_parse_error("Foo := [ ]", true,
                       ST_PARSE_ERR_UNEXPECTED_TOKEN);
    expect_parse_error("Foo := Object", true,
                       ST_PARSE_ERR_UNEXPECTED_TOKEN);
    expect_parse_error("foo [ 1r0 ]", false,
                       ST_PARSE_ERR_INVALID_NUMBER);
    expect_parse_error("foo [ 2r102 ]", false,
                       ST_PARSE_ERR_INVALID_NUMBER);
    expect_parse_error("foo [ -16r-FF ]", false,
                       ST_PARSE_ERR_INVALID_NUMBER);
    expect_parse_error("foo [ $<16r110000> ]", false,
                       ST_PARSE_ERR_INVALID_CHARACTER);
    expect_parse_error("foo [ $\xE0\x80\x80 ]", false,
                       ST_PARSE_ERR_INVALID_CHARACTER);
    expect_parse_error("foo [ $\xED\xA0\x80 ]", false,
                       ST_PARSE_ERR_INVALID_CHARACTER);
    expect_parse_error("foo [ $\xF4\x90\x80\x80 ]", false,
                       ST_PARSE_ERR_INVALID_CHARACTER);
}

int main(void)
{
    test_basic_method_and_literals();
    test_method_patterns_and_class_side();
    test_message_precedence_chains_and_cascades();
    test_blocks_temporaries_and_assignment();
    test_exact_numeric_and_character_literals();
    test_complete_literal_and_parenthesized_spans();
    test_literal_arrays();
    test_classes_namespaces_extensions_and_pragmas();
    test_compilation_unit_and_file();
    test_nesting_limits();
    test_long_cascade_chain_is_iterative();
    test_top_level_separators_and_reinit();
    test_errors();

    if (failures != 0u) {
        fprintf(stderr, "smalltalk parser: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk parser: PASS");
    return EXIT_SUCCESS;
}
