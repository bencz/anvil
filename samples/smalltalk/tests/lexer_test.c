#include "st_lexer.h"

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

static const st_token_t *current(st_lexer_t *lexer)
{
    const st_token_t *token = st_lexer_current(lexer);
    CHECK(token != NULL);
    return token;
}

static void expect(st_lexer_t *lexer, st_token_kind_t kind, const char *text)
{
    const st_token_t *token = current(lexer);
    if (token == NULL) {
        return;
    }
    CHECK(token->kind == kind);
    CHECK(token->length == strlen(text));
    CHECK(strcmp(token->text, text) == 0);
    CHECK(token->span.end.offset >= token->span.begin.offset);
    CHECK(st_lexer_advance(lexer, NULL));
}

static void test_reference_sequence(void)
{
    static const char source[] =
        "123 abc efg: [ ] #foo $a 'abc' 'a''b' '' '''' := 16r09AF #( ;";
    st_lexer_t lexer;

    CHECK(st_lexer_init_cstr(&lexer, source));
    expect(&lexer, ST_TOKEN_NUMBER, "123");
    expect(&lexer, ST_TOKEN_IDENTIFIER, "abc");
    expect(&lexer, ST_TOKEN_KEYWORD, "efg:");
    expect(&lexer, ST_TOKEN_LEFT_BRACKET, "[");
    expect(&lexer, ST_TOKEN_RIGHT_BRACKET, "]");
    expect(&lexer, ST_TOKEN_SYMBOL_PREFIX, "#");
    expect(&lexer, ST_TOKEN_IDENTIFIER, "foo");
    expect(&lexer, ST_TOKEN_CHARACTER, "$a");
    expect(&lexer, ST_TOKEN_STRING, "abc");
    expect(&lexer, ST_TOKEN_STRING, "a'b");
    expect(&lexer, ST_TOKEN_STRING, "");
    expect(&lexer, ST_TOKEN_STRING, "'");
    expect(&lexer, ST_TOKEN_ASSIGN, ":=");
    expect(&lexer, ST_TOKEN_NUMBER, "16r09AF");
    expect(&lexer, ST_TOKEN_LITERAL_ARRAY_BEGIN, "#(");
    expect(&lexer, ST_TOKEN_SEMICOLON, ";");
    expect(&lexer, ST_TOKEN_EOF, "");
    st_lexer_destroy(&lexer);
}

static void test_selectors_and_numbers(void)
{
    static const char source[] =
        "- -- < <= > >= | || + ~= 1. 1.25 1e-2 1e+ 2.5s 3.14s2 3s0 2r101 36rZ";
    st_lexer_t lexer;

    CHECK(st_lexer_init_cstr(&lexer, source));
    expect(&lexer, ST_TOKEN_MINUS, "-");
    expect(&lexer, ST_TOKEN_BINARY_SELECTOR, "--");
    expect(&lexer, ST_TOKEN_LESS_THAN, "<");
    expect(&lexer, ST_TOKEN_BINARY_SELECTOR, "<=");
    expect(&lexer, ST_TOKEN_GREATER_THAN, ">");
    expect(&lexer, ST_TOKEN_BINARY_SELECTOR, ">=");
    expect(&lexer, ST_TOKEN_VERTICAL_BAR, "|");
    expect(&lexer, ST_TOKEN_BINARY_SELECTOR, "||");
    expect(&lexer, ST_TOKEN_BINARY_SELECTOR, "+");
    expect(&lexer, ST_TOKEN_BINARY_SELECTOR, "~=");
    expect(&lexer, ST_TOKEN_NUMBER, "1");
    expect(&lexer, ST_TOKEN_PERIOD, ".");
    expect(&lexer, ST_TOKEN_NUMBER, "1.25");
    expect(&lexer, ST_TOKEN_NUMBER, "1e-2");
    expect(&lexer, ST_TOKEN_NUMBER, "1");
    expect(&lexer, ST_TOKEN_IDENTIFIER, "e");
    expect(&lexer, ST_TOKEN_BINARY_SELECTOR, "+");
    expect(&lexer, ST_TOKEN_NUMBER, "2.5s");
    expect(&lexer, ST_TOKEN_NUMBER, "3.14s2");
    expect(&lexer, ST_TOKEN_NUMBER, "3s0");
    expect(&lexer, ST_TOKEN_NUMBER, "2r101");
    expect(&lexer, ST_TOKEN_NUMBER, "36rZ");
    st_lexer_destroy(&lexer);
}

static void test_comments_spans_and_separation(void)
{
    static const char source[] = "foo\n  \"a \"\"quote\"\"\"bar:baz";
    st_lexer_t lexer;
    const st_token_t *token;

    CHECK(st_lexer_init_cstr(&lexer, source));
    token = current(&lexer);
    CHECK(token->span.begin.offset == 0u);
    CHECK(token->span.begin.line == 1u && token->span.begin.column == 1u);
    CHECK(!token->separated);
    expect(&lexer, ST_TOKEN_IDENTIFIER, "foo");

    token = current(&lexer);
    CHECK(token->separated);
    CHECK(token->span.begin.line == 2u);
    CHECK(token->span.begin.column == 16u);
    expect(&lexer, ST_TOKEN_KEYWORD, "bar:");

    token = current(&lexer);
    CHECK(!token->separated);
    expect(&lexer, ST_TOKEN_IDENTIFIER, "baz");
    st_lexer_destroy(&lexer);
}

static void test_lookahead_and_previous(void)
{
    st_lexer_t lexer;
    const st_token_t *token;
    size_t distance;

    CHECK(st_lexer_init_cstr(&lexer, "a b c d e f g h"));
    for (distance = 0u; distance <= ST_LEXER_LOOKAHEAD; distance++) {
        CHECK(st_lexer_peek(&lexer, distance, &token));
        CHECK(token->kind == ST_TOKEN_IDENTIFIER);
        CHECK(token->text[0] == (char)('a' + distance));
    }
    CHECK(st_lexer_current(&lexer) != NULL);
    CHECK(strcmp(st_lexer_current(&lexer)->text, "a") == 0);
    CHECK(st_lexer_advance(&lexer, &token));
    CHECK(strcmp(token->text, "b") == 0);
    CHECK(st_lexer_previous(&lexer) != NULL);
    CHECK(strcmp(st_lexer_previous(&lexer)->text, "a") == 0);

    /* Maximum forward lookahead must not evict the previous token, which the
     * parser uses to close the current source span. */
    CHECK(st_lexer_peek(&lexer, ST_LEXER_LOOKAHEAD, &token));
    CHECK(strcmp(token->text, "h") == 0);
    CHECK(st_lexer_previous(&lexer) != NULL);
    CHECK(strcmp(st_lexer_previous(&lexer)->text, "a") == 0);

    token = (const st_token_t *)1;
    CHECK(!st_lexer_peek(&lexer, ST_LEXER_LOOKAHEAD + 1u, &token));
    CHECK(token == NULL);
    CHECK(st_lexer_status(&lexer) == ST_LEXER_ERR_LOOKAHEAD);
    st_lexer_destroy(&lexer);
}

static void test_unterminated_input(void)
{
    st_lexer_t lexer;

    CHECK(st_lexer_init_cstr(&lexer, "'abc"));
    expect(&lexer, ST_TOKEN_UNTERMINATED_STRING, "abc");
    st_lexer_destroy(&lexer);

    CHECK(st_lexer_init_cstr(&lexer, "\"abc"));
    expect(&lexer, ST_TOKEN_UNTERMINATED_COMMENT, "abc");
    st_lexer_destroy(&lexer);

    CHECK(st_lexer_init_cstr(&lexer, "$"));
    expect(&lexer, ST_TOKEN_UNTERMINATED_CHARACTER, "$");
    st_lexer_destroy(&lexer);
}

static void test_utf8_character(void)
{
    static const unsigned char valid[][4] = {
        { 0xE0u, 0xA0u, 0x80u, 0u },
        { 0xEDu, 0x9Fu, 0xBFu, 0u },
        { 0xF0u, 0x90u, 0x80u, 0x80u },
        { 0xF4u, 0x8Fu, 0xBFu, 0xBFu }
    };
    static const size_t valid_lengths[] = { 3u, 3u, 4u, 4u };
    st_lexer_t lexer;
    const st_token_t *token;
    size_t valid_index;
    CHECK(st_lexer_init_cstr(&lexer, "$\xF0\x9F\x99\x82"));
    token = current(&lexer);
    CHECK(token->kind == ST_TOKEN_CHARACTER);
    CHECK(token->length == 5u);
    CHECK(memcmp(token->text, "$\xF0\x9F\x99\x82", 5u) == 0);
    st_lexer_destroy(&lexer);

    for (valid_index = 0u;
            valid_index < sizeof(valid_lengths) / sizeof(valid_lengths[0]);
            valid_index++) {
        unsigned char source[5] = { '$', 0u, 0u, 0u, 0u };
        memcpy(source + 1u, valid[valid_index], valid_lengths[valid_index]);
        CHECK(st_lexer_init_memory(&lexer, source,
                                   valid_lengths[valid_index] + 1u));
        CHECK(current(&lexer)->kind == ST_TOKEN_CHARACTER);
        st_lexer_destroy(&lexer);
    }

    {
        static const unsigned char invalid[][5] = {
            { '$', 0xC0u, 0x80u, 0u, 0u },       /* overlong U+0000 */
            { '$', 0xE0u, 0x80u, 0x80u, 0u },    /* overlong U+0000 */
            { '$', 0xEDu, 0xA0u, 0x80u, 0u },    /* surrogate U+D800 */
            { '$', 0xF0u, 0x80u, 0x80u, 0x80u }, /* overlong U+0000 */
            { '$', 0xF4u, 0x90u, 0x80u, 0x80u }  /* beyond U+10FFFF */
        };
        static const size_t lengths[] = { 3u, 4u, 4u, 5u, 5u };
        size_t index;
        for (index = 0u; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
            CHECK(st_lexer_init_memory(&lexer, invalid[index], lengths[index]));
            token = current(&lexer);
            CHECK(token->kind == ST_TOKEN_UNKNOWN);
            st_lexer_destroy(&lexer);
        }
    }
}

static void test_closed_comment_does_not_grow_token(void)
{
    size_t body_length = 4096u;
    char *source = malloc(body_length + 4u);
    st_lexer_t lexer;
    const st_token_t *token;

    CHECK(source != NULL);
    if (source == NULL) return;
    source[0] = '"';
    memset(source + 1u, 'x', body_length);
    source[body_length + 1u] = '"';
    source[body_length + 2u] = 'a';
    source[body_length + 3u] = '\0';
    CHECK(st_lexer_init_cstr(&lexer, source));
    token = current(&lexer);
    CHECK(token->kind == ST_TOKEN_IDENTIFIER);
    CHECK(token->capacity == ST_TOKEN_INLINE_CAPACITY);
    CHECK(token->text == token->inline_text);
    CHECK(token->span.begin.offset == body_length + 2u);
    st_lexer_destroy(&lexer);
    free(source);
}

static void test_terminal_eof_and_reinit(void)
{
    char long_source[ST_TOKEN_INLINE_CAPACITY * 2u];
    st_lexer_t lexer;
    const st_token_t *eof;
    const st_token_t *token;
    size_t cursor;
    size_t produced;
    size_t index;

    memset(long_source, 'a', sizeof(long_source) - 1u);
    long_source[sizeof(long_source) - 1u] = '\0';
    CHECK(st_lexer_init_cstr(&lexer, long_source));
    token = current(&lexer);
    CHECK(token->text != token->inline_text);
    CHECK(st_lexer_advance(&lexer, NULL));
    eof = current(&lexer);
    CHECK(eof->kind == ST_TOKEN_EOF);
    cursor = lexer.cursor;
    produced = lexer.produced;
    for (index = 0u; index < 32u; index++) {
        token = NULL;
        CHECK(st_lexer_advance(&lexer, &token));
        CHECK(token == eof);
        CHECK(lexer.cursor == cursor && lexer.produced == produced);
        CHECK(st_lexer_peek(&lexer, ST_LEXER_LOOKAHEAD, &token));
        CHECK(token == eof);
    }

    CHECK(st_lexer_reinit_cstr(&lexer, "second"));
    expect(&lexer, ST_TOKEN_IDENTIFIER, "second");
    expect(&lexer, ST_TOKEN_EOF, "");
    CHECK(st_lexer_reinit_memory(&lexer, NULL, 0u));
    CHECK(st_lexer_current(&lexer)->kind == ST_TOKEN_EOF);
    st_lexer_destroy(&lexer);
}

static void test_long_token_and_embedded_nul(void)
{
    char source[514];
    unsigned char binary[] = { 'a', 0, 'b' };
    st_lexer_t lexer;
    const st_token_t *token;

    memset(source, 'a', sizeof(source) - 1u);
    source[sizeof(source) - 1u] = '\0';
    CHECK(st_lexer_init_cstr(&lexer, source));
    token = current(&lexer);
    CHECK(token->kind == ST_TOKEN_IDENTIFIER);
    CHECK(token->length == sizeof(source) - 1u);
    CHECK(token->capacity > ST_TOKEN_INLINE_CAPACITY);
    st_lexer_destroy(&lexer);

    CHECK(st_lexer_init_memory(&lexer, binary, sizeof(binary)));
    expect(&lexer, ST_TOKEN_IDENTIFIER, "a");
    /* NUL is data for span-based input, but outside the supported ASCII grammar. */
    token = current(&lexer);
    CHECK(token->kind == ST_TOKEN_UNKNOWN);
    CHECK(token->length == 1u && token->text[0] == '\0');
    CHECK(st_lexer_advance(&lexer, NULL));
    expect(&lexer, ST_TOKEN_IDENTIFIER, "b");
    st_lexer_destroy(&lexer);
}

static void test_file_input(void)
{
    FILE *file = tmpfile();
    st_lexer_t lexer;
    const st_token_t *token;

    CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    CHECK(fputs("  answer: 42", file) >= 0);
    rewind(file);
    CHECK(st_lexer_init_file(&lexer, file));
    token = current(&lexer);
    CHECK(token->separated);
    expect(&lexer, ST_TOKEN_KEYWORD, "answer:");
    expect(&lexer, ST_TOKEN_NUMBER, "42");
    expect(&lexer, ST_TOKEN_EOF, "");
    CHECK(st_lexer_reinit_cstr(&lexer, "again"));
    expect(&lexer, ST_TOKEN_IDENTIFIER, "again");
    st_lexer_destroy(&lexer);
    CHECK(fclose(file) == 0);
}

static void test_invalid_arguments(void)
{
    st_lexer_t lexer;
    const st_token_t *token = (const st_token_t *)1;

    CHECK(!st_lexer_init_memory(&lexer, NULL, 1u));
    CHECK(st_lexer_status(&lexer) == ST_LEXER_ERR_INVALID_ARGUMENT);
    st_lexer_destroy(&lexer);

    CHECK(!st_lexer_init_cstr(&lexer, NULL));
    CHECK(st_lexer_status(&lexer) == ST_LEXER_ERR_INVALID_ARGUMENT);
    CHECK(!st_lexer_advance(&lexer, &token));
    CHECK(token == NULL);
    st_lexer_destroy(&lexer);
}

int main(void)
{
    test_reference_sequence();
    test_selectors_and_numbers();
    test_comments_spans_and_separation();
    test_lookahead_and_previous();
    test_unterminated_input();
    test_utf8_character();
    test_closed_comment_does_not_grow_token();
    test_terminal_eof_and_reinit();
    test_long_token_and_embedded_nul();
    test_file_input();
    test_invalid_arguments();

    if (failures != 0u) {
        fprintf(stderr, "smalltalk lexer: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk lexer: PASS");
    return EXIT_SUCCESS;
}
