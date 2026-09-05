#include "st_parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define BINARY_KINDS (ST_TOKEN_BINARY_SELECTOR | ST_TOKEN_VERTICAL_BAR \
    | ST_TOKEN_MINUS | ST_TOKEN_LESS_THAN | ST_TOKEN_GREATER_THAN)

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} string_builder_t;

static st_ast_node_t *parse_methods_method(st_parser_t *parser);
static bool parse_methods(st_parser_t *parser, st_ast_list_t *methods);
static bool parse_pragmas(st_parser_t *parser, st_ast_list_t *pragmas);
static st_ast_node_t *parse_pragma(st_parser_t *parser);
static bool parse_temporaries(st_parser_t *parser, st_ast_list_t *temporaries);
static bool parse_expressions(st_parser_t *parser, st_ast_list_t *expressions);
static st_ast_node_t *parse_binary_object(st_parser_t *parser);
static st_ast_node_t *parse_unary_object(st_parser_t *parser);
static st_ast_node_t *parse_primary(st_parser_t *parser);
static st_ast_node_t *parse_literal(st_parser_t *parser);
static st_ast_node_t *parse_variable(st_parser_t *parser);
static st_ast_node_t *parse_variable_declaration(st_parser_t *parser);
static st_ast_node_t *parse_array(st_parser_t *parser);
static st_ast_node_t *parse_unary_message(st_parser_t *parser);
static st_ast_node_t *parse_binary_message(st_parser_t *parser);
static st_ast_node_t *parse_keyword_message(st_parser_t *parser);

static const st_token_t *current(const st_parser_t *parser)
{
    return st_lexer_current(&parser->lexer);
}

static void set_error(st_parser_t *parser, st_parse_status_t status,
                      st_token_kind_t expected)
{
    const st_token_t *token;
    if (parser == NULL || parser->error.status != ST_PARSE_OK) {
        return;
    }
    parser->error.status = status;
    parser->error.expected = expected;
    token = current(parser);
    if (token != NULL) {
        parser->error.actual = token->kind;
        parser->error.span = token->span;
    }
    parser->error.lexer_status = st_lexer_status(&parser->lexer);
    parser->error.ast_status = st_ast_unit_status(parser->unit);
}

static bool sync_subsystem_error(st_parser_t *parser)
{
    if (st_lexer_status(&parser->lexer) != ST_LEXER_OK) {
        set_error(parser, ST_PARSE_ERR_LEXER, ST_TOKEN_NONE);
        return false;
    }
    if (st_ast_unit_status(parser->unit) != ST_AST_OK) {
        set_error(parser, ST_PARSE_ERR_AST, ST_TOKEN_NONE);
        parser->error.ast_status = st_ast_unit_status(parser->unit);
        return false;
    }
    return parser->error.status == ST_PARSE_OK;
}

static bool enter_nesting(st_parser_t *parser)
{
    if (parser->nesting_depth >= parser->nesting_limit) {
        set_error(parser, ST_PARSE_ERR_NESTING_LIMIT, ST_TOKEN_NONE);
        return false;
    }
    parser->nesting_depth++;
    return true;
}

static void leave_nesting(st_parser_t *parser)
{
    if (parser->nesting_depth != 0u) parser->nesting_depth--;
}

static bool lexical_token_ok(st_parser_t *parser, const st_token_t *token)
{
    if (token == NULL) {
        return sync_subsystem_error(parser);
    }
    if (token->kind == ST_TOKEN_UNTERMINATED_STRING
            || token->kind == ST_TOKEN_UNTERMINATED_COMMENT
            || token->kind == ST_TOKEN_UNTERMINATED_CHARACTER
            || token->kind == ST_TOKEN_UNKNOWN) {
        set_error(parser,
                  token->kind == ST_TOKEN_UNKNOWN && token->length != 0u
                      && token->text[0] == '$'
                    ? ST_PARSE_ERR_INVALID_CHARACTER
                    : ST_PARSE_ERR_UNEXPECTED_TOKEN,
                  ST_TOKEN_NONE);
        return false;
    }
    return true;
}

static bool advance(st_parser_t *parser)
{
    const st_token_t *token = NULL;
    if (!st_lexer_advance(&parser->lexer, &token)) {
        return sync_subsystem_error(parser);
    }
    return lexical_token_ok(parser, token);
}

static const st_token_t *peek(st_parser_t *parser, size_t distance)
{
    const st_token_t *token = NULL;
    if (!st_lexer_peek(&parser->lexer, distance, &token)) {
        (void)sync_subsystem_error(parser);
        return NULL;
    }
    return lexical_token_ok(parser, token) ? token : NULL;
}

static bool expect(st_parser_t *parser, st_token_kind_t kinds)
{
    const st_token_t *token = current(parser);
    if (!lexical_token_ok(parser, token)) {
        return false;
    }
    if (!st_token_is(token, kinds)) {
        set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN, kinds);
        return false;
    }
    return advance(parser);
}

static st_source_span_t close_span(st_parser_t *parser,
                                   st_source_position_t begin)
{
    st_source_span_t span;
    const st_token_t *previous = st_lexer_previous(&parser->lexer);
    span.begin = begin;
    span.end = previous != NULL ? previous->span.end : begin;
    return span;
}

static bool append_node(st_parser_t *parser, st_ast_list_t *list,
                        st_ast_node_t *node)
{
    if (node == NULL || !st_ast_list_append(parser->unit, list, node)) {
        return sync_subsystem_error(parser);
    }
    return true;
}

static bool copy_bytes(st_parser_t *parser, const void *bytes, size_t length,
                       st_ast_string_t *out)
{
    if (!st_ast_copy_string(parser->unit, bytes, length, out)) {
        return sync_subsystem_error(parser);
    }
    return true;
}

static bool copy_token(st_parser_t *parser, const st_token_t *token,
                       st_ast_string_t *out)
{
    return copy_bytes(parser, token->text, token->length, out);
}

static void builder_destroy(string_builder_t *builder)
{
    free(builder->data);
    memset(builder, 0, sizeof(*builder));
}

static bool builder_append(st_parser_t *parser, string_builder_t *builder,
                           const char *bytes, size_t length)
{
    size_t required;
    size_t capacity;
    char *data;
    if (length > SIZE_MAX - builder->length - 1u) {
        set_error(parser, ST_PARSE_ERR_OUT_OF_MEMORY, ST_TOKEN_NONE);
        return false;
    }
    required = builder->length + length + 1u;
    if (required > builder->capacity) {
        capacity = builder->capacity == 0u ? 64u : builder->capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2u) {
                set_error(parser, ST_PARSE_ERR_OUT_OF_MEMORY, ST_TOKEN_NONE);
                return false;
            }
            capacity *= 2u;
        }
        data = realloc(builder->data, capacity);
        if (data == NULL) {
            set_error(parser, ST_PARSE_ERR_OUT_OF_MEMORY, ST_TOKEN_NONE);
            return false;
        }
        builder->data = data;
        builder->capacity = capacity;
    }
    if (length != 0u) {
        memcpy(builder->data + builder->length, bytes, length);
    }
    builder->length += length;
    builder->data[builder->length] = '\0';
    return true;
}

static bool builder_finish(st_parser_t *parser, string_builder_t *builder,
                           st_ast_string_t *out)
{
    return copy_bytes(parser, builder->data, builder->length, out);
}

static bool string_equals(st_ast_string_t string, const char *literal)
{
    size_t length = strlen(literal);
    return string.length == length
        && memcmp(string.data, literal, length) == 0;
}

static bool node_is_variable(const st_ast_node_t *node, const char *name)
{
    return node != NULL && node->kind == ST_AST_VARIABLE
        && string_equals(node->as.variable.name, name);
}

static bool parser_initialize(st_parser_t *parser, st_ast_unit_t *unit)
{
    if (parser == NULL || unit == NULL || st_ast_unit_status(unit) != ST_AST_OK) {
        if (parser != NULL) {
            memset(parser, 0, sizeof(*parser));
            parser->error.status = ST_PARSE_ERR_INVALID_ARGUMENT;
        }
        return false;
    }
    memset(parser, 0, sizeof(*parser));
    parser->unit = unit;
    parser->nesting_limit = ST_PARSER_MAX_NESTING;
    return true;
}

static bool parser_init_failed(st_parser_t *parser)
{
    st_parse_error_t error = parser->error;
    st_lexer_destroy(&parser->lexer);
    parser->error = error;
    return false;
}

static bool parser_lexer_init_failed(st_parser_t *parser)
{
    st_lexer_status_t status = st_lexer_status(&parser->lexer);
    set_error(parser,
              status == ST_LEXER_ERR_INVALID_ARGUMENT
                ? ST_PARSE_ERR_INVALID_ARGUMENT
                : (status == ST_LEXER_ERR_OUT_OF_MEMORY
                    ? ST_PARSE_ERR_OUT_OF_MEMORY : ST_PARSE_ERR_LEXER),
              ST_TOKEN_NONE);
    return parser_init_failed(parser);
}

bool st_parser_init_memory(st_parser_t *parser, st_ast_unit_t *unit,
                           const void *source, size_t length)
{
    if (!parser_initialize(parser, unit)) {
        return false;
    }
    if (!st_lexer_init_memory(&parser->lexer, source, length)) {
        return parser_lexer_init_failed(parser);
    }
    if (!lexical_token_ok(parser, current(parser))) {
        return parser_init_failed(parser);
    }
    return true;
}

bool st_parser_init_cstr(st_parser_t *parser, st_ast_unit_t *unit,
                         const char *source)
{
    if (!parser_initialize(parser, unit)) {
        return false;
    }
    if (!st_lexer_init_cstr(&parser->lexer, source)) {
        return parser_lexer_init_failed(parser);
    }
    if (!lexical_token_ok(parser, current(parser))) {
        return parser_init_failed(parser);
    }
    return true;
}

bool st_parser_init_file(st_parser_t *parser, st_ast_unit_t *unit, FILE *file)
{
    if (!parser_initialize(parser, unit)) {
        return false;
    }
    if (!st_lexer_init_file(&parser->lexer, file)) {
        return parser_lexer_init_failed(parser);
    }
    if (!lexical_token_ok(parser, current(parser))) {
        return parser_init_failed(parser);
    }
    return true;
}

bool st_parser_reinit_memory(st_parser_t *parser, st_ast_unit_t *unit,
                             const void *source, size_t length)
{
    if (parser == NULL) return false;
    st_parser_destroy(parser);
    return st_parser_init_memory(parser, unit, source, length);
}

bool st_parser_reinit_cstr(st_parser_t *parser, st_ast_unit_t *unit,
                           const char *source)
{
    if (parser == NULL) return false;
    st_parser_destroy(parser);
    return st_parser_init_cstr(parser, unit, source);
}

bool st_parser_reinit_file(st_parser_t *parser, st_ast_unit_t *unit, FILE *file)
{
    if (parser == NULL) return false;
    st_parser_destroy(parser);
    return st_parser_init_file(parser, unit, file);
}

bool st_parser_set_nesting_limit(st_parser_t *parser, size_t limit)
{
    if (parser == NULL || parser->unit == NULL
            || current(parser) == NULL
            || parser->error.status != ST_PARSE_OK || limit == 0u
            || parser->nesting_depth != 0u) {
        if (parser != NULL) {
            set_error(parser, ST_PARSE_ERR_INVALID_ARGUMENT, ST_TOKEN_NONE);
        }
        return false;
    }
    parser->nesting_limit = limit;
    return true;
}

void st_parser_destroy(st_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    st_lexer_destroy(&parser->lexer);
    memset(parser, 0, sizeof(*parser));
}

st_parse_status_t st_parser_status(const st_parser_t *parser)
{
    return parser == NULL ? ST_PARSE_ERR_INVALID_ARGUMENT : parser->error.status;
}

const st_parse_error_t *st_parser_error(const st_parser_t *parser)
{
    return parser == NULL ? NULL : &parser->error;
}

const char *st_parse_status_string(st_parse_status_t status)
{
    switch (status) {
    case ST_PARSE_OK: return "ok";
    case ST_PARSE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_PARSE_ERR_LEXER: return "lexer failure";
    case ST_PARSE_ERR_AST: return "AST allocation failure";
    case ST_PARSE_ERR_OUT_OF_MEMORY: return "parser out of memory";
    case ST_PARSE_ERR_UNEXPECTED_TOKEN: return "unexpected token";
    case ST_PARSE_ERR_INVALID_NUMBER: return "invalid numeric literal";
    case ST_PARSE_ERR_INVALID_CHARACTER: return "invalid character literal";
    case ST_PARSE_ERR_NESTING_LIMIT: return "maximum nesting depth exceeded";
    case ST_PARSE_ERR_MISSING_SEPARATOR: return "missing top-level separator";
    default: return "invalid parser status";
    }
}

bool st_parser_at_end(const st_parser_t *parser)
{
    const st_token_t *token = parser == NULL ? NULL : current(parser);
    return token != NULL && token->kind == ST_TOKEN_EOF;
}

static bool method_first(const st_token_t *token)
{
    return token != NULL && st_token_is(token,
        ST_TOKEN_IDENTIFIER | ST_TOKEN_KEYWORD | BINARY_KINDS);
}

static st_ast_node_t *parse_variable_internal(st_parser_t *parser,
                                               bool allow_type)
{
    const st_token_t *token = current(parser);
    st_source_position_t begin;
    st_ast_kind_t kind = ST_AST_VARIABLE;
    st_ast_node_t *node;
    if (token == NULL || token->kind != ST_TOKEN_IDENTIFIER) {
        set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN, ST_TOKEN_IDENTIFIER);
        return NULL;
    }
    begin = token->span.begin;
    if (token->length == 3u && memcmp(token->text, "nil", 3u) == 0) {
        kind = ST_AST_NIL;
    } else if (token->length == 4u && memcmp(token->text, "true", 4u) == 0) {
        kind = ST_AST_TRUE;
    } else if (token->length == 5u && memcmp(token->text, "false", 5u) == 0) {
        kind = ST_AST_FALSE;
    }
    node = st_ast_new_node(parser->unit, kind, token->span);
    if (node == NULL
            || (kind == ST_AST_VARIABLE
                ? !copy_token(parser, token, &node->as.variable.name)
                : !copy_token(parser, token, &node->as.text))) {
        (void)sync_subsystem_error(parser);
        return NULL;
    }
    if (!advance(parser)) return NULL;
    if (kind == ST_AST_VARIABLE && current(parser)->kind == ST_TOKEN_COLON) {
        const st_token_t *second_colon = peek(parser, 1u);
        if (second_colon == NULL) return NULL;
        if (second_colon->kind == ST_TOKEN_COLON) {
            if (!allow_type) {
                set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                          ST_TOKEN_NONE);
                return NULL;
            }
            if (!advance(parser) || !advance(parser)
                    || current(parser)->kind != ST_TOKEN_IDENTIFIER) {
                if (parser->error.status == ST_PARSE_OK) {
                    set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                              ST_TOKEN_IDENTIFIER);
                }
                return NULL;
            }
            node->as.variable.has_type = true;
            if (!copy_token(parser, current(parser),
                            &node->as.variable.type_name)
                    || !advance(parser)) {
                return NULL;
            }
            node->span = close_span(parser, begin);
        }
    }
    return node;
}

static st_ast_node_t *parse_variable(st_parser_t *parser)
{
    return parse_variable_internal(parser, false);
}

static st_ast_node_t *parse_variable_declaration(st_parser_t *parser)
{
    return parse_variable_internal(parser, true);
}

static bool parse_temporaries(st_parser_t *parser, st_ast_list_t *temporaries)
{
    if (current(parser)->kind != ST_TOKEN_VERTICAL_BAR) {
        return true;
    }
    if (!advance(parser)) {
        return false;
    }
    while (current(parser)->kind == ST_TOKEN_IDENTIFIER) {
        if (!append_node(parser, temporaries,
                         parse_variable_declaration(parser))) {
            return false;
        }
    }
    return expect(parser, ST_TOKEN_VERTICAL_BAR);
}

static st_ast_node_t *new_message(st_parser_t *parser,
                                  st_source_position_t begin,
                                  st_ast_string_t selector,
                                  st_ast_list_t arguments)
{
    st_ast_node_t *node = st_ast_new_node(
        parser->unit, ST_AST_MESSAGE, close_span(parser, begin));
    if (node == NULL) {
        (void)sync_subsystem_error(parser);
        return NULL;
    }
    node->as.message.selector = selector;
    node->as.message.arguments = arguments;
    return node;
}

static st_ast_node_t *parse_unary_message(st_parser_t *parser)
{
    const st_token_t *token = current(parser);
    st_source_position_t begin;
    st_ast_string_t selector;
    st_ast_list_t arguments = {0};
    if (token->kind != ST_TOKEN_IDENTIFIER) {
        set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN, ST_TOKEN_IDENTIFIER);
        return NULL;
    }
    begin = token->span.begin;
    if (!copy_token(parser, token, &selector) || !advance(parser)) {
        return NULL;
    }
    return new_message(parser, begin, selector, arguments);
}

static st_ast_node_t *parse_binary_message(st_parser_t *parser)
{
    const st_token_t *token = current(parser);
    st_source_position_t begin;
    st_ast_string_t selector;
    st_ast_list_t arguments = {0};
    st_ast_node_t *argument;
    if (!st_token_is(token, BINARY_KINDS)) {
        set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN, BINARY_KINDS);
        return NULL;
    }
    begin = token->span.begin;
    if (!copy_token(parser, token, &selector) || !advance(parser)) {
        return NULL;
    }
    argument = parse_unary_object(parser);
    if (!append_node(parser, &arguments, argument)) {
        return NULL;
    }
    return new_message(parser, begin, selector, arguments);
}

static st_ast_node_t *parse_keyword_message(st_parser_t *parser)
{
    st_source_position_t begin = current(parser)->span.begin;
    string_builder_t builder = {0};
    st_ast_string_t selector = {0};
    st_ast_list_t arguments = {0};
    st_ast_node_t *result = NULL;

    while (current(parser)->kind == ST_TOKEN_KEYWORD) {
        const st_token_t *token = current(parser);
        st_ast_node_t *argument;
        if (!builder_append(parser, &builder, token->text, token->length)
                || !advance(parser)) {
            goto done;
        }
        argument = parse_binary_object(parser);
        if (!append_node(parser, &arguments, argument)) {
            goto done;
        }
    }
    if (!builder_finish(parser, &builder, &selector)) {
        goto done;
    }
    result = new_message(parser, begin, selector, arguments);
done:
    builder_destroy(&builder);
    return result;
}

static st_ast_node_t *wrap_expression(st_parser_t *parser,
                                      st_ast_node_t *receiver,
                                      st_ast_node_t *message)
{
    st_source_span_t span;
    st_ast_node_t *expression;
    if (receiver == NULL || message == NULL) {
        return NULL;
    }
    span.begin = receiver->span.begin;
    span.end = message->span.end;
    expression = st_ast_new_node(parser->unit, ST_AST_EXPRESSION, span);
    if (expression == NULL
            || !append_node(parser, &expression->as.expression.messages,
                            message)) {
        (void)sync_subsystem_error(parser);
        return NULL;
    }
    expression->as.expression.receiver = receiver;
    if (node_is_variable(receiver, "super")) {
        message->as.message.super_send = true;
    }
    return expression;
}

static st_ast_node_t *parse_unary_object(st_parser_t *parser)
{
    st_ast_node_t *value = parse_primary(parser);
    if (value == NULL) {
        return NULL;
    }
    while (current(parser)->kind == ST_TOKEN_IDENTIFIER) {
        value = wrap_expression(parser, value, parse_unary_message(parser));
        if (value == NULL) {
            return NULL;
        }
    }
    return value;
}

static st_ast_node_t *parse_binary_object(st_parser_t *parser)
{
    st_ast_node_t *value = parse_unary_object(parser);
    if (value == NULL) {
        return NULL;
    }
    while (st_token_is(current(parser), BINARY_KINDS)) {
        value = wrap_expression(parser, value, parse_binary_message(parser));
        if (value == NULL) {
            return NULL;
        }
    }
    return value;
}


static bool expression_first(const st_token_t *token)
{
    return token != NULL && st_token_is(token,
        ST_TOKEN_RETURN | ST_TOKEN_NUMBER | ST_TOKEN_IDENTIFIER
        | ST_TOKEN_SYMBOL_PREFIX | ST_TOKEN_CHARACTER | ST_TOKEN_STRING
        | ST_TOKEN_LITERAL_ARRAY_BEGIN | ST_TOKEN_LEFT_PAREN
        | ST_TOKEN_LEFT_BRACKET | ST_TOKEN_MINUS);
}

st_ast_node_t *st_parse_expression(st_parser_t *parser)
{
    st_source_position_t begin;
    st_ast_node_t *expression;
    st_ast_node_t *receiver;
    st_ast_node_t *keyword_message = NULL;
    bool returns = false;

    if (parser == NULL || parser->error.status != ST_PARSE_OK
            || !expression_first(current(parser))) {
        if (parser != NULL) {
            set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                ST_TOKEN_RETURN | ST_TOKEN_NUMBER | ST_TOKEN_IDENTIFIER
                | ST_TOKEN_SYMBOL_PREFIX | ST_TOKEN_CHARACTER | ST_TOKEN_STRING
                | ST_TOKEN_LITERAL_ARRAY_BEGIN | ST_TOKEN_LEFT_PAREN
                | ST_TOKEN_LEFT_BRACKET | ST_TOKEN_MINUS);
        }
        return NULL;
    }
    begin = current(parser)->span.begin;
    if (current(parser)->kind == ST_TOKEN_RETURN) {
        returns = true;
        if (!advance(parser)) {
            return NULL;
        }
    }

    expression = st_ast_new_node(parser->unit, ST_AST_EXPRESSION,
                                 (st_source_span_t){begin, begin});
    if (expression == NULL) {
        (void)sync_subsystem_error(parser);
        return NULL;
    }
    expression->as.expression.returns = returns;

    while (current(parser)->kind == ST_TOKEN_IDENTIFIER) {
        const st_token_t *next = peek(parser, 1u);
        if (next == NULL) {
            return NULL;
        }
        if (next->kind != ST_TOKEN_ASSIGN) {
            break;
        }
        if (!append_node(parser, &expression->as.expression.assignments,
                         parse_variable(parser))
                || !expect(parser, ST_TOKEN_ASSIGN)) {
            return NULL;
        }
    }

    receiver = parse_binary_object(parser);
    if (receiver == NULL) {
        return NULL;
    }

    if (current(parser)->kind == ST_TOKEN_KEYWORD) {
        keyword_message = parse_keyword_message(parser);
        if (keyword_message == NULL) return NULL;
    }

    /* Cascades retain the receiver of the message immediately before the
     * first semicolon. In `self new initialize; yourself`, that receiver is
     * the result of `self new`, evaluated once, rather than the class. */
    if (keyword_message != NULL) {
        expression->as.expression.receiver = receiver;
        if (!append_node(parser, &expression->as.expression.messages,
                         keyword_message)) {
            return NULL;
        }
        if (node_is_variable(receiver, "super")) {
            keyword_message->as.message.super_send = true;
        }
    } else if (receiver->kind == ST_AST_EXPRESSION) {
        expression->as.expression.receiver = receiver->as.expression.receiver;
        expression->as.expression.messages = receiver->as.expression.messages;
    } else {
        expression->as.expression.receiver = receiver;
    }

    while (current(parser)->kind == ST_TOKEN_SEMICOLON) {
        st_ast_node_t *message;

        if (expression->as.expression.messages.count == 0u
                || (keyword_message == NULL && receiver->kind == ST_AST_EXPRESSION
                    && receiver->as.expression.parenthesized)) {
            set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN, ST_TOKEN_PERIOD);
            return NULL;
        }

        if (!advance(parser)) {
            return NULL;
        }
        if (current(parser)->kind == ST_TOKEN_IDENTIFIER) {
            message = parse_unary_message(parser);
        } else if (st_token_is(current(parser), BINARY_KINDS)) {
            message = parse_binary_message(parser);
        } else if (current(parser)->kind == ST_TOKEN_KEYWORD) {
            message = parse_keyword_message(parser);
        } else {
            set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                      ST_TOKEN_IDENTIFIER | ST_TOKEN_KEYWORD | BINARY_KINDS);
            return NULL;
        }
        message->as.message.starts_cascade = true;
        if (!append_node(parser, &expression->as.expression.messages, message)) {
            return NULL;
        }
        if (node_is_variable(expression->as.expression.receiver, "super")) {
            message->as.message.super_send = true;
        }
    }
    expression->span = close_span(parser, begin);
    return expression;
}

static bool parse_expressions(st_parser_t *parser, st_ast_list_t *expressions)
{
    while (expression_first(current(parser))) {
        st_ast_node_t *expression = st_parse_expression(parser);
        if (!append_node(parser, expressions, expression)) {
            return false;
        }
        if (current(parser)->kind != ST_TOKEN_PERIOD) {
            break;
        }
        if (!advance(parser)) {
            return false;
        }
        if (expression->as.expression.returns) {
            break;
        }
    }
    return true;
}

st_ast_node_t *st_parse_block(st_parser_t *parser)
{
    st_source_position_t begin;
    st_ast_node_t *block;
    st_ast_node_t *result = NULL;
    if (parser == NULL || parser->error.status != ST_PARSE_OK
            || current(parser)->kind != ST_TOKEN_LEFT_BRACKET) {
        if (parser != NULL) {
            set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                      ST_TOKEN_LEFT_BRACKET);
        }
        return NULL;
    }
    if (!enter_nesting(parser)) return NULL;
    begin = current(parser)->span.begin;
    block = st_ast_new_node(parser->unit, ST_AST_BLOCK,
                            (st_source_span_t){begin, begin});
    if (block == NULL || !advance(parser)) {
        (void)sync_subsystem_error(parser);
        goto done;
    }

    if (current(parser)->kind == ST_TOKEN_COLON) {
        while (current(parser)->kind == ST_TOKEN_COLON) {
            if (!advance(parser)
                    || current(parser)->kind != ST_TOKEN_IDENTIFIER
                    || !append_node(parser, &block->as.block.arguments,
                                    parse_variable_declaration(parser))) {
                if (parser->error.status == ST_PARSE_OK) {
                    set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                              ST_TOKEN_IDENTIFIER);
                }
                goto done;
            }
        }
        if (!expect(parser, ST_TOKEN_VERTICAL_BAR)) {
            goto done;
        }
    }
    if (!parse_temporaries(parser, &block->as.block.temporaries)
            || !parse_expressions(parser, &block->as.block.expressions)
            || !expect(parser, ST_TOKEN_RIGHT_BRACKET)) {
        goto done;
    }
    block->span = close_span(parser, begin);
    result = block;
done:
    leave_nesting(parser);
    return result;
}

static unsigned digit_value(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') return (unsigned)(byte - '0');
    if (byte >= 'A' && byte <= 'Z') return 10u + (unsigned)(byte - 'A');
    if (byte >= 'a' && byte <= 'z') return 10u + (unsigned)(byte - 'a');
    return 36u;
}

static bool parse_unsigned_decimal(const char *text, size_t length,
                                   unsigned *value_out)
{
    unsigned value = 0u;
    size_t index;
    if (length == 0u) return false;
    for (index = 0u; index < length; index++) {
        unsigned digit;
        if (text[index] < '0' || text[index] > '9') return false;
        digit = (unsigned)(text[index] - '0');
        if (value > (UINT_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *value_out = value;
    return true;
}

static st_ast_node_t *parse_number(st_parser_t *parser, bool outer_negative)
{
    const st_token_t *token = current(parser);
    const char *digits;
    size_t digits_length;
    const char *radix_mark;
    const char *scaled_mark;
    st_ast_kind_t kind;
    st_ast_node_t *node;
    bool negative = outer_negative;
    unsigned radix = 10u;
    size_t index;

    if (token->kind != ST_TOKEN_NUMBER) {
        set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN, ST_TOKEN_NUMBER);
        return NULL;
    }
    radix_mark = memchr(token->text, 'r', token->length);
    scaled_mark = memchr(token->text, 's', token->length);
    if (radix_mark != NULL) {
        size_t prefix_length = (size_t)(radix_mark - token->text);
        digits = radix_mark + 1;
        digits_length = token->length - prefix_length - 1u;
        if (!parse_unsigned_decimal(token->text, prefix_length, &radix)
                || radix < 2u || radix > 36u) {
            set_error(parser, ST_PARSE_ERR_INVALID_NUMBER, ST_TOKEN_NONE);
            return NULL;
        }
        if (digits_length != 0u && digits[0] == '-') {
            if (outer_negative) {
                set_error(parser, ST_PARSE_ERR_INVALID_NUMBER, ST_TOKEN_NONE);
                return NULL;
            }
            negative = true;
            digits++;
            digits_length--;
        }
        if (digits_length == 0u) {
            set_error(parser, ST_PARSE_ERR_INVALID_NUMBER, ST_TOKEN_NONE);
            return NULL;
        }
        for (index = 0u; index < digits_length; index++) {
            if (digit_value((unsigned char)digits[index]) >= radix) {
                set_error(parser, ST_PARSE_ERR_INVALID_NUMBER, ST_TOKEN_NONE);
                return NULL;
            }
        }
        kind = ST_AST_INTEGER;
    } else if (scaled_mark != NULL) {
        digits = token->text;
        digits_length = token->length;
        kind = ST_AST_SCALED_DECIMAL;
    } else if (memchr(token->text, '.', token->length) != NULL
            || memchr(token->text, 'e', token->length) != NULL
            || memchr(token->text, 'E', token->length) != NULL) {
        digits = token->text;
        digits_length = token->length;
        kind = ST_AST_FLOAT;
    } else {
        digits = token->text;
        digits_length = token->length;
        kind = ST_AST_INTEGER;
    }

    node = st_ast_new_node(parser->unit, kind, token->span);
    if (node == NULL) {
        (void)sync_subsystem_error(parser);
        return NULL;
    }
    if (kind == ST_AST_INTEGER) {
        node->as.integer.radix = radix;
        node->as.integer.negative = negative;
        if (!copy_bytes(parser, digits, digits_length,
                        &node->as.integer.spelling)) {
            return NULL;
        }
    } else {
        node->as.real.negative = negative;
        if (!copy_bytes(parser, digits, digits_length,
                        &node->as.real.spelling)) {
            return NULL;
        }
    }
    return advance(parser) ? node : NULL;
}

static st_ast_node_t *parse_symbol_after_prefix(st_parser_t *parser,
                                                 st_source_position_t begin)
{
    const st_token_t *token = current(parser);
    string_builder_t builder = {0};
    st_ast_node_t *node = NULL;
    st_ast_string_t text = {0};

    if (!st_token_is(token, ST_TOKEN_IDENTIFIER | ST_TOKEN_KEYWORD
                            | ST_TOKEN_STRING | BINARY_KINDS)) {
        set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                  ST_TOKEN_IDENTIFIER | ST_TOKEN_KEYWORD | ST_TOKEN_STRING
                  | BINARY_KINDS);
        return NULL;
    }
    do {
        if (!builder_append(parser, &builder, token->text, token->length)
                || !advance(parser)) {
            goto done;
        }
        if (token->kind != ST_TOKEN_KEYWORD) {
            break;
        }
        token = current(parser);
    } while (token->kind == ST_TOKEN_KEYWORD && !token->separated);

    if (!builder_finish(parser, &builder, &text)) {
        goto done;
    }
    node = st_ast_new_node(parser->unit, ST_AST_SYMBOL,
                           close_span(parser, begin));
    if (node != NULL) {
        node->as.text = text;
    } else {
        (void)sync_subsystem_error(parser);
    }
done:
    builder_destroy(&builder);
    return node;
}

static st_ast_node_t *parse_character(st_parser_t *parser)
{
    const st_token_t *token = current(parser);
    const st_token_t *next = NULL;
    st_ast_node_t *node;
    uint32_t value;
    bool numeric_escape;
    st_source_position_t begin = token->span.begin;
    if (token->length == 2u && token->text[1] == '<') {
        next = peek(parser, 1u);
        if (next == NULL) return NULL;
    }
    numeric_escape = token->length == 2u && token->text[1] == '<'
        && next != NULL && next->kind == ST_TOKEN_NUMBER;
    if (token->length >= 2u && !numeric_escape) {
        const unsigned char *bytes = (const unsigned char *)token->text + 1u;
        size_t length = token->length - 1u;
        if (length == 1u && bytes[0] < 0x80u) {
            value = bytes[0];
        } else if (length == 2u && bytes[0] >= 0xC2u
                && bytes[0] <= 0xDFu) {
            value = ((uint32_t)(bytes[0] & 0x1Fu) << 6)
                  | (uint32_t)(bytes[1] & 0x3Fu);
        } else if (length == 3u && bytes[0] >= 0xE0u
                && bytes[0] <= 0xEFu
                && !(bytes[0] == 0xE0u && bytes[1] < 0xA0u)
                && !(bytes[0] == 0xEDu && bytes[1] > 0x9Fu)) {
            value = ((uint32_t)(bytes[0] & 0x0Fu) << 12)
                  | ((uint32_t)(bytes[1] & 0x3Fu) << 6)
                  | (uint32_t)(bytes[2] & 0x3Fu);
        } else if (length == 4u && bytes[0] >= 0xF0u
                && bytes[0] <= 0xF4u
                && !(bytes[0] == 0xF0u && bytes[1] < 0x90u)
                && !(bytes[0] == 0xF4u && bytes[1] > 0x8Fu)) {
            value = ((uint32_t)(bytes[0] & 0x07u) << 18)
                  | ((uint32_t)(bytes[1] & 0x3Fu) << 12)
                  | ((uint32_t)(bytes[2] & 0x3Fu) << 6)
                  | (uint32_t)(bytes[3] & 0x3Fu);
        } else {
            set_error(parser, ST_PARSE_ERR_INVALID_CHARACTER, ST_TOKEN_NONE);
            return NULL;
        }
        if (value > UINT32_C(0x10FFFF)
                || (value >= 0xD800u && value <= 0xDFFFu)) {
            set_error(parser, ST_PARSE_ERR_INVALID_CHARACTER, ST_TOKEN_NONE);
            return NULL;
        }
        node = st_ast_new_node(parser->unit, ST_AST_CHARACTER, token->span);
        if (node == NULL || !advance(parser)) {
            (void)sync_subsystem_error(parser);
            return NULL;
        }
        node->as.character = value;
        return node;
    }
    if (numeric_escape) {
        st_ast_node_t *number;
        unsigned long accumulator = 0u;
        size_t index;
        if (!advance(parser)) return NULL;
        number = parse_number(parser, false);
        if (number == NULL || number->kind != ST_AST_INTEGER
                || number->as.integer.negative) {
            set_error(parser, ST_PARSE_ERR_INVALID_CHARACTER, ST_TOKEN_NONE);
            return NULL;
        }
        for (index = 0u; index < number->as.integer.spelling.length; index++) {
            unsigned digit = digit_value(
                (unsigned char)number->as.integer.spelling.data[index]);
            if (accumulator > (UINT32_MAX - digit) / number->as.integer.radix) {
                set_error(parser, ST_PARSE_ERR_INVALID_CHARACTER, ST_TOKEN_NONE);
                return NULL;
            }
            accumulator = accumulator * number->as.integer.radix + digit;
        }
        if (!expect(parser, ST_TOKEN_GREATER_THAN)
                || accumulator > UINT32_C(0x10FFFF)
                || (accumulator >= 0xD800u && accumulator <= 0xDFFFu)) {
            if (parser->error.status == ST_PARSE_OK) {
                set_error(parser, ST_PARSE_ERR_INVALID_CHARACTER, ST_TOKEN_NONE);
            }
            return NULL;
        }
        node = st_ast_new_node(parser->unit, ST_AST_CHARACTER,
                               close_span(parser, begin));
        if (node == NULL) {
            (void)sync_subsystem_error(parser);
            return NULL;
        }
        node->as.character = (uint32_t)accumulator;
        return node;
    }
    set_error(parser, ST_PARSE_ERR_INVALID_CHARACTER, ST_TOKEN_NONE);
    return NULL;
}

static bool literal_array_item_first(const st_token_t *token)
{
    return token != NULL && st_token_is(token,
        ST_TOKEN_NUMBER | ST_TOKEN_MINUS | ST_TOKEN_SYMBOL_PREFIX
        | ST_TOKEN_STRING | ST_TOKEN_CHARACTER | ST_TOKEN_LITERAL_ARRAY_BEGIN
        | ST_TOKEN_LEFT_PAREN | ST_TOKEN_IDENTIFIER | ST_TOKEN_KEYWORD
        | BINARY_KINDS);
}

static st_ast_node_t *parse_array_item(st_parser_t *parser)
{
    const st_token_t *token = current(parser);
    if (token->kind == ST_TOKEN_MINUS) {
        const st_token_t *next = peek(parser, 1u);
        if (next == NULL) return NULL;
        if (next->kind == ST_TOKEN_NUMBER) {
            return parse_literal(parser);
        }
        return parse_symbol_after_prefix(parser, token->span.begin);
    }
    if (token->kind == ST_TOKEN_IDENTIFIER) {
        if ((token->length == 3u && memcmp(token->text, "nil", 3u) == 0)
                || (token->length == 4u && memcmp(token->text, "true", 4u) == 0)
                || (token->length == 5u && memcmp(token->text, "false", 5u) == 0)) {
            return parse_variable(parser);
        }
        return parse_symbol_after_prefix(parser, token->span.begin);
    }
    if (token->kind == ST_TOKEN_KEYWORD || st_token_is(token, BINARY_KINDS)) {
        return parse_symbol_after_prefix(parser, token->span.begin);
    }
    return parse_literal(parser);
}

static st_ast_node_t *parse_array(st_parser_t *parser)
{
    st_source_position_t begin = current(parser)->span.begin;
    st_ast_node_t *array = NULL;
    st_ast_node_t *result = NULL;
    if (!enter_nesting(parser)) return NULL;
    array = st_ast_new_node(parser->unit, ST_AST_LITERAL_ARRAY,
                            (st_source_span_t){begin, begin});
    if (array == NULL || !advance(parser)) {
        (void)sync_subsystem_error(parser);
        goto done;
    }
    while (literal_array_item_first(current(parser))) {
        if (!append_node(parser, &array->as.array.elements,
                         parse_array_item(parser))) {
            goto done;
        }
    }
    if (!expect(parser, ST_TOKEN_RIGHT_PAREN)) {
        goto done;
    }
    array->span = close_span(parser, begin);
    result = array;
done:
    leave_nesting(parser);
    return result;
}

static st_ast_node_t *parse_literal(st_parser_t *parser)
{
    const st_token_t *token = current(parser);
    st_ast_node_t *node;
    switch (token->kind) {
    case ST_TOKEN_MINUS: {
        st_source_position_t begin = token->span.begin;
        if (!advance(parser)) return NULL;
        node = parse_number(parser, true);
        if (node != NULL) node->span.begin = begin;
        return node;
    }
    case ST_TOKEN_NUMBER:
        return parse_number(parser, false);
    case ST_TOKEN_SYMBOL_PREFIX: {
        st_source_position_t begin = token->span.begin;
        if (!advance(parser)) return NULL;
        return parse_symbol_after_prefix(parser, begin);
    }
    case ST_TOKEN_STRING:
        node = st_ast_new_node(parser->unit, ST_AST_STRING, token->span);
        if (node == NULL || !copy_token(parser, token, &node->as.text)) {
            (void)sync_subsystem_error(parser);
            return NULL;
        }
        return advance(parser) ? node : NULL;
    case ST_TOKEN_CHARACTER:
        return parse_character(parser);
    case ST_TOKEN_LITERAL_ARRAY_BEGIN:
    case ST_TOKEN_LEFT_PAREN:
        return parse_array(parser);
    default:
        set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
            ST_TOKEN_NUMBER | ST_TOKEN_SYMBOL_PREFIX | ST_TOKEN_STRING
            | ST_TOKEN_CHARACTER | ST_TOKEN_LITERAL_ARRAY_BEGIN);
        return NULL;
    }
}

static st_ast_node_t *parse_primary(st_parser_t *parser)
{
    if (current(parser)->kind == ST_TOKEN_IDENTIFIER) {
        return parse_variable(parser);
    }
    if (current(parser)->kind == ST_TOKEN_LEFT_BRACKET) {
        return st_parse_block(parser);
    }
    if (current(parser)->kind == ST_TOKEN_LEFT_PAREN) {
        st_ast_node_t *expression;
        st_source_position_t begin = current(parser)->span.begin;
        if (!enter_nesting(parser)) return NULL;
        if (!advance(parser)) {
            leave_nesting(parser);
            return NULL;
        }
        expression = st_parse_expression(parser);
        if (expression == NULL || !expect(parser, ST_TOKEN_RIGHT_PAREN)) {
            leave_nesting(parser);
            return NULL;
        }
        expression->span = close_span(parser, begin);
        expression->as.expression.parenthesized = true;
        leave_nesting(parser);
        return expression;
    }
    return parse_literal(parser);
}

static st_ast_node_t *parse_pragma(st_parser_t *parser)
{
    st_source_position_t begin = current(parser)->span.begin;
    string_builder_t builder = {0};
    st_ast_list_t arguments = {0};
    st_ast_string_t selector = {0};
    st_ast_node_t *result = NULL;
    while (current(parser)->kind == ST_TOKEN_KEYWORD) {
        const st_token_t *token = current(parser);
        st_ast_node_t *argument;
        if (!builder_append(parser, &builder, token->text, token->length)
                || !advance(parser)) {
            goto done;
        }
        if (current(parser)->kind == ST_TOKEN_IDENTIFIER) {
            argument = parse_variable(parser);
        } else {
            argument = parse_literal(parser);
        }
        if (!append_node(parser, &arguments, argument)) {
            goto done;
        }
    }
    if (!builder_finish(parser, &builder, &selector)) {
        goto done;
    }
    result = new_message(parser, begin, selector, arguments);
done:
    builder_destroy(&builder);
    return result;
}

static bool parse_pragmas(st_parser_t *parser, st_ast_list_t *pragmas)
{
    for (;;) {
        const st_token_t *next;
        if (current(parser)->kind != ST_TOKEN_LESS_THAN) {
            return true;
        }
        next = peek(parser, 1u);
        if (next == NULL) return false;
        if (next->kind != ST_TOKEN_KEYWORD) {
            return true;
        }
        if (!advance(parser)
                || !append_node(parser, pragmas, parse_pragma(parser))
                || !expect(parser, ST_TOKEN_GREATER_THAN)) {
            return false;
        }
    }
}

static bool parse_method_pattern(st_parser_t *parser, st_ast_node_t *method)
{
    const st_token_t *token = current(parser);
    string_builder_t builder = {0};
    bool result = false;

    if (token->kind == ST_TOKEN_IDENTIFIER) {
        if (!copy_token(parser, token, &method->as.method.selector)
                || !advance(parser)) {
            return false;
        }
        return true;
    }
    if (st_token_is(token, BINARY_KINDS)) {
        if (!copy_token(parser, token, &method->as.method.selector)
                || !advance(parser)
                || current(parser)->kind != ST_TOKEN_IDENTIFIER
                || !append_node(parser, &method->as.method.arguments,
                                parse_variable_declaration(parser))) {
            if (parser->error.status == ST_PARSE_OK) {
                set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                          ST_TOKEN_IDENTIFIER);
            }
            return false;
        }
        return true;
    }
    if (token->kind != ST_TOKEN_KEYWORD) {
        set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                  ST_TOKEN_IDENTIFIER | ST_TOKEN_KEYWORD | BINARY_KINDS);
        return false;
    }
    while (current(parser)->kind == ST_TOKEN_KEYWORD) {
        token = current(parser);
        if (!builder_append(parser, &builder, token->text, token->length)
                || !advance(parser)
                || current(parser)->kind != ST_TOKEN_IDENTIFIER
                || !append_node(parser, &method->as.method.arguments,
                                parse_variable_declaration(parser))) {
            if (parser->error.status == ST_PARSE_OK) {
                set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                          ST_TOKEN_IDENTIFIER);
            }
            goto done;
        }
    }
    result = builder_finish(parser, &builder, &method->as.method.selector);
done:
    builder_destroy(&builder);
    return result;
}

static st_ast_node_t *parse_methods_method(st_parser_t *parser)
{
    return st_parse_method(parser);
}

st_ast_node_t *st_parse_method(st_parser_t *parser)
{
    st_source_position_t begin;
    st_ast_node_t *method;
    st_ast_node_t *body;
    const st_token_t *next;
    st_ast_node_t *result = NULL;

    if (parser == NULL || parser->error.status != ST_PARSE_OK
            || !method_first(current(parser))) {
        if (parser != NULL) {
            set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                      ST_TOKEN_IDENTIFIER | ST_TOKEN_KEYWORD | BINARY_KINDS);
        }
        return NULL;
    }
    begin = current(parser)->span.begin;
    method = st_ast_new_node(parser->unit, ST_AST_METHOD,
                             (st_source_span_t){begin, begin});
    if (method == NULL) {
        (void)sync_subsystem_error(parser);
        return NULL;
    }

    if (current(parser)->kind == ST_TOKEN_IDENTIFIER) {
        next = peek(parser, 1u);
        if (next == NULL) return NULL;
        if (method_first(next)) {
            if (!copy_token(parser, current(parser),
                            &method->as.method.class_name)) {
                return NULL;
            }
            method->as.method.class_side =
                string_equals(method->as.method.class_name, "class");
            if (!advance(parser)) return NULL;
        }
    }
    if (!parse_method_pattern(parser, method)
            || current(parser)->kind != ST_TOKEN_LEFT_BRACKET) {
        if (parser->error.status == ST_PARSE_OK) {
            set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                      ST_TOKEN_LEFT_BRACKET);
        }
        return NULL;
    }
    if (!enter_nesting(parser)) return NULL;

    body = st_ast_new_node(parser->unit, ST_AST_BLOCK,
                           (st_source_span_t){current(parser)->span.begin,
                                              current(parser)->span.begin});
    if (body == NULL || !advance(parser)) {
        (void)sync_subsystem_error(parser);
        goto done;
    }
    body->as.block.arguments = method->as.method.arguments;
    if (!parse_pragmas(parser, &method->as.method.pragmas)
            || !parse_temporaries(parser, &body->as.block.temporaries)
            || !parse_expressions(parser, &body->as.block.expressions)
            || !expect(parser, ST_TOKEN_RIGHT_BRACKET)) {
        goto done;
    }
    body->span.end = st_lexer_previous(&parser->lexer)->span.end;
    method->as.method.body = body;
    method->span = close_span(parser, begin);
    result = method;
done:
    leave_nesting(parser);
    return result;
}

static bool parse_methods(st_parser_t *parser, st_ast_list_t *methods)
{
    while (method_first(current(parser))) {
        if (!append_node(parser, methods, parse_methods_method(parser))) {
            return false;
        }
    }
    return true;
}

st_ast_node_t *st_parse_class(st_parser_t *parser)
{
    st_source_position_t begin;
    st_ast_node_t *node;
    st_ast_node_t *result = NULL;
    if (parser == NULL || parser->error.status != ST_PARSE_OK
            || current(parser)->kind != ST_TOKEN_IDENTIFIER) {
        if (parser != NULL) {
            set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                      ST_TOKEN_IDENTIFIER);
        }
        return NULL;
    }
    if (!enter_nesting(parser)) return NULL;
    begin = current(parser)->span.begin;
    node = st_ast_new_node(parser->unit, ST_AST_CLASS,
                           (st_source_span_t){begin, begin});
    if (node == NULL) {
        (void)sync_subsystem_error(parser);
        goto done;
    }
    node->as.class_decl.name = parse_variable(parser);
    if (node->as.class_decl.name == NULL) goto done;

    if (current(parser)->kind == ST_TOKEN_IDENTIFIER
            && current(parser)->length == 6u
            && memcmp(current(parser)->text, "extend", 6u) == 0) {
        node->as.class_decl.is_extension = true;
        if (!advance(parser) || !expect(parser, ST_TOKEN_LEFT_BRACKET)
                || !parse_methods(parser, &node->as.class_decl.methods)
                || !expect(parser, ST_TOKEN_RIGHT_BRACKET)) {
            goto done;
        }
        node->span = close_span(parser, begin);
        result = node;
        goto done;
    }

    if (!expect(parser, ST_TOKEN_ASSIGN)
            || current(parser)->kind != ST_TOKEN_IDENTIFIER) {
        if (parser->error.status == ST_PARSE_OK) {
            set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                      ST_TOKEN_IDENTIFIER);
        }
        goto done;
    }
    node->as.class_decl.super_name = parse_variable(parser);
    if (node->as.class_decl.super_name == NULL) goto done;

    if (node_is_variable(node->as.class_decl.super_name, "Namespace")) {
        node->as.class_decl.is_namespace = true;
        if (!expect(parser, ST_TOKEN_LEFT_BRACKET)) goto done;
        while (current(parser)->kind == ST_TOKEN_IDENTIFIER) {
            if (!append_node(parser, &node->as.class_decl.members,
                             st_parse_class(parser))) {
                goto done;
            }
        }
        if (!expect(parser, ST_TOKEN_RIGHT_BRACKET)) goto done;
        node->span = close_span(parser, begin);
        result = node;
        goto done;
    }

    if (!expect(parser, ST_TOKEN_LEFT_BRACKET)
            || !parse_pragmas(parser, &node->as.class_decl.pragmas)
            || !parse_temporaries(parser, &node->as.class_decl.variables)
            || !parse_methods(parser, &node->as.class_decl.methods)
            || !expect(parser, ST_TOKEN_RIGHT_BRACKET)) {
        goto done;
    }
    node->span = close_span(parser, begin);
    result = node;
done:
    leave_nesting(parser);
    return result;
}

st_ast_node_t *st_parse_method_or_block(st_parser_t *parser)
{
    if (parser == NULL || parser->error.status != ST_PARSE_OK) {
        return NULL;
    }
    return current(parser)->kind == ST_TOKEN_LEFT_BRACKET
        ? st_parse_block(parser) : st_parse_method(parser);
}

bool st_parse_compilation_unit(st_parser_t *parser)
{
    bool first = true;
    if (parser == NULL || parser->error.status != ST_PARSE_OK) {
        return false;
    }
    while (!st_parser_at_end(parser)) {
        if (!first && !current(parser)->separated) {
            set_error(parser, ST_PARSE_ERR_MISSING_SEPARATOR, ST_TOKEN_NONE);
            return false;
        }
        st_ast_node_t *declaration = st_parse_class(parser);
        if (!append_node(parser, &parser->unit->declarations, declaration)
                || !append_node(parser, &parser->unit->forms, declaration)) {
            return false;
        }
        first = false;
    }
    return true;
}

static bool class_form_first(st_parser_t *parser)
{
    const st_token_t *first = current(parser);
    const st_token_t *second;
    const st_token_t *third;
    const st_token_t *fourth;
    if (first == NULL || first->kind != ST_TOKEN_IDENTIFIER) {
        return false;
    }
    second = peek(parser, 1u);
    if (second == NULL) return false;
    if (second->kind == ST_TOKEN_IDENTIFIER && second->length == 6u
            && memcmp(second->text, "extend", 6u) == 0) {
        third = peek(parser, 2u);
        return third != NULL && third->kind == ST_TOKEN_LEFT_BRACKET;
    }
    if (second->kind != ST_TOKEN_ASSIGN) {
        return false;
    }
    third = peek(parser, 2u);
    fourth = peek(parser, 3u);
    return third != NULL && fourth != NULL
        && third->kind == ST_TOKEN_IDENTIFIER
        && fourth->kind == ST_TOKEN_LEFT_BRACKET;
}

bool st_parse_source_unit(st_parser_t *parser)
{
    bool first = true;
    bool period_separator = false;
    if (parser == NULL || parser->error.status != ST_PARSE_OK) {
        return false;
    }
    while (!st_parser_at_end(parser)) {
        st_ast_node_t *form;
        bool declaration = class_form_first(parser);
        if (parser->error.status != ST_PARSE_OK) return false;
        if (!first && !period_separator && !current(parser)->separated) {
            set_error(parser, ST_PARSE_ERR_MISSING_SEPARATOR, ST_TOKEN_NONE);
            return false;
        }
        period_separator = false;
        if (declaration) {
            form = st_parse_class(parser);
        } else if (current(parser)->kind == ST_TOKEN_LEFT_BRACKET) {
            form = st_parse_block(parser);
        } else if (expression_first(current(parser))) {
            form = st_parse_expression(parser);
            if (form != NULL && current(parser)->kind == ST_TOKEN_PERIOD) {
                period_separator = true;
                if (!advance(parser)) return false;
            }
        } else {
            set_error(parser, ST_PARSE_ERR_UNEXPECTED_TOKEN,
                      ST_TOKEN_IDENTIFIER | ST_TOKEN_LEFT_BRACKET
                      | ST_TOKEN_RETURN | ST_TOKEN_NUMBER
                      | ST_TOKEN_SYMBOL_PREFIX | ST_TOKEN_CHARACTER
                      | ST_TOKEN_STRING | ST_TOKEN_LITERAL_ARRAY_BEGIN
                      | ST_TOKEN_LEFT_PAREN | ST_TOKEN_MINUS);
            return false;
        }
        if (!append_node(parser, &parser->unit->forms, form)) {
            return false;
        }
        if (declaration
                && !append_node(parser, &parser->unit->declarations, form)) {
            return false;
        }
        first = false;
    }
    return true;
}
