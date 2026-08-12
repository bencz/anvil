#include "st_lexer.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void token_init(st_token_t *token)
{
    memset(token, 0, sizeof(*token));
    token->text = token->inline_text;
    token->capacity = sizeof(token->inline_text);
}

static void token_reset(st_token_t *token)
{
    token->kind = ST_TOKEN_NONE;
    memset(&token->span, 0, sizeof(token->span));
    token->separated = false;
    token->length = 0;
    token->text[0] = '\0';
}

static bool token_reserve(st_lexer_t *lexer, st_token_t *token,
                          size_t required)
{
    size_t capacity = token->capacity;
    char *storage;

    if (required <= capacity) {
        return true;
    }
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) {
            lexer->status = ST_LEXER_ERR_OUT_OF_MEMORY;
            return false;
        }
        capacity *= 2u;
    }

    if (token->text == token->inline_text) {
        storage = malloc(capacity);
        if (storage != NULL) {
            memcpy(storage, token->inline_text, token->length + 1u);
        }
    } else {
        storage = realloc(token->text, capacity);
    }
    if (storage == NULL) {
        lexer->status = ST_LEXER_ERR_OUT_OF_MEMORY;
        return false;
    }
    token->text = storage;
    token->capacity = capacity;
    return true;
}

static bool token_append(st_lexer_t *lexer, st_token_t *token,
                         unsigned char byte)
{
    if (token->length > SIZE_MAX - 2u
            || !token_reserve(lexer, token, token->length + 2u)) {
        return false;
    }
    token->text[token->length++] = (char)byte;
    token->text[token->length] = '\0';
    return true;
}

static st_source_position_t current_position(const st_lexer_t *lexer)
{
    st_source_position_t position;
    position.offset = lexer->offset;
    position.line = lexer->line;
    position.column = lexer->column;
    return position;
}

static unsigned char byte_at(const st_lexer_t *lexer, size_t lookahead)
{
    if (lookahead > lexer->source_length - lexer->offset) {
        return 0;
    }
    if (lexer->offset + lookahead >= lexer->source_length) {
        return 0;
    }
    return lexer->source[lexer->offset + lookahead];
}

static bool at_end(const st_lexer_t *lexer)
{
    return lexer->offset >= lexer->source_length;
}

static unsigned char consume_byte(st_lexer_t *lexer)
{
    unsigned char byte;

    if (lexer->offset >= lexer->source_length) {
        return 0;
    }
    byte = lexer->source[lexer->offset++];
    if (byte == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    return byte;
}

static bool is_space(unsigned char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r'
        || byte == '\f';
}

static bool is_digit(unsigned char byte)
{
    return byte >= '0' && byte <= '9';
}

static bool is_identifier_begin(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z')
        || (byte >= 'A' && byte <= 'Z') || byte == '_';
}

static bool is_identifier_continue(unsigned char byte)
{
    return is_identifier_begin(byte) || is_digit(byte);
}

static bool is_binary_selector(unsigned char byte)
{
    switch (byte) {
    case '!': case '%': case '&': case '*': case '+': case ',': case '-':
    case '/': case '<': case '=': case '>': case '?': case '@': case '\\':
    case '|': case '~':
        return true;
    default:
        return false;
    }
}

static size_t utf8_sequence_length(unsigned char lead)
{
    if (lead < 0x80u) return 1u;
    if (lead >= 0xC2u && lead <= 0xDFu) return 2u;
    if (lead >= 0xE0u && lead <= 0xEFu) return 3u;
    if (lead >= 0xF0u && lead <= 0xF4u) return 4u;
    return 0u;
}

static bool is_utf8_continuation(unsigned char byte)
{
    return (byte & 0xC0u) == 0x80u;
}

static bool utf8_sequence_is_canonical(const st_lexer_t *lexer, size_t length)
{
    unsigned char lead = byte_at(lexer, 0u);
    unsigned char second;
    size_t index;

    if (length == 1u) {
        return lead < 0x80u;
    }
    if (length < 2u || length > lexer->source_length - lexer->offset) {
        return false;
    }
    for (index = 1u; index < length; index++) {
        if (!is_utf8_continuation(byte_at(lexer, index))) {
            return false;
        }
    }

    second = byte_at(lexer, 1u);
    /* These bounds simultaneously enforce shortest-form encoding, exclude
     * UTF-16 surrogates, and cap Unicode at U+10FFFF. */
    if (lead == 0xE0u && second < 0xA0u) return false;
    if (lead == 0xEDu && second > 0x9Fu) return false;
    if (lead == 0xF0u && second < 0x90u) return false;
    if (lead == 0xF4u && second > 0x8Fu) return false;
    return true;
}

static bool append_consumed(st_lexer_t *lexer, st_token_t *token)
{
    return token_append(lexer, token, consume_byte(lexer));
}

/* Returns false only for a hard lexer failure.  Unterminated comments are a
 * normal token so the parser can issue a source-level diagnostic. */
static bool skip_separators(st_lexer_t *lexer, st_token_t *token,
                            bool *separated, bool *emitted_error)
{
    *emitted_error = false;
    for (;;) {
        while (is_space(byte_at(lexer, 0))) {
            *separated = true;
            (void)consume_byte(lexer);
        }
        if (byte_at(lexer, 0) != '"') {
            return true;
        }

        {
            bool comment_closed = false;
            *separated = true;
            token_reset(token);
            token->span.begin = current_position(lexer);
            (void)consume_byte(lexer);

            while (lexer->offset < lexer->source_length) {
                unsigned char byte = consume_byte(lexer);
                if (byte != '"') continue;
                if (byte_at(lexer, 0) == '"') {
                    (void)consume_byte(lexer);
                    continue;
                }
                comment_closed = true;
                token_reset(token);
                break;
            }
            if (!comment_closed) {
                size_t index = token->span.begin.offset + 1u;
                token->kind = ST_TOKEN_UNTERMINATED_COMMENT;
                /* Closed comments never touch token storage.  Only an actual
                 * diagnostic token pays the cost of materialising/unescaping
                 * its body. */
                while (index < lexer->source_length) {
                    unsigned char byte = lexer->source[index++];
                    if (byte == '"' && index < lexer->source_length
                            && lexer->source[index] == '"') {
                        index++;
                    }
                    if (!token_append(lexer, token, byte)) return false;
                }
                token->span.end = current_position(lexer);
                token->separated = true;
                *emitted_error = true;
                return true;
            }
        }
    }
}

static bool scan_string(st_lexer_t *lexer, st_token_t *token)
{
    (void)consume_byte(lexer); /* opening quote */
    token->kind = ST_TOKEN_UNTERMINATED_STRING;
    while (lexer->offset < lexer->source_length) {
        unsigned char byte = consume_byte(lexer);
        if (byte != '\'') {
            if (!token_append(lexer, token, byte)) {
                return false;
            }
            continue;
        }
        if (byte_at(lexer, 0) == '\'') {
            (void)consume_byte(lexer);
            if (!token_append(lexer, token, '\'')) {
                return false;
            }
            continue;
        }
        token->kind = ST_TOKEN_STRING;
        return true;
    }
    return true;
}

static bool scan_number(st_lexer_t *lexer, st_token_t *token)
{
    token->kind = ST_TOKEN_NUMBER;
    while (is_digit(byte_at(lexer, 0))) {
        if (!append_consumed(lexer, token)) {
            return false;
        }
    }

    if (byte_at(lexer, 0) == 'r') {
        if (!append_consumed(lexer, token)) {
            return false;
        }
        if (byte_at(lexer, 0) == '-') {
            if (!append_consumed(lexer, token)) {
                return false;
            }
        }
        /* Consume the complete candidate spelling.  The parser validates every
         * digit against the radix, so `2r102` becomes one precise invalid
         * literal instead of a valid `2r10` followed by an unrelated `2`. */
        while (is_identifier_continue(byte_at(lexer, 0))) {
            if (!append_consumed(lexer, token)) {
                return false;
            }
        }
        return true;
    }

    if (byte_at(lexer, 0) == '.' && is_digit(byte_at(lexer, 1))) {
        if (!append_consumed(lexer, token)) {
            return false;
        }
        while (is_digit(byte_at(lexer, 0))) {
            if (!append_consumed(lexer, token)) {
                return false;
            }
        }
    }

    if (byte_at(lexer, 0) == 'e' || byte_at(lexer, 0) == 'E') {
        size_t digit_offset = 1u;
        if (byte_at(lexer, digit_offset) == '+'
                || byte_at(lexer, digit_offset) == '-') {
            digit_offset++;
        }
        if (is_digit(byte_at(lexer, digit_offset))) {
            if (!append_consumed(lexer, token)) {
                return false;
            }
            if (byte_at(lexer, 0) == '+' || byte_at(lexer, 0) == '-') {
                if (!append_consumed(lexer, token)) {
                    return false;
                }
            }
            while (is_digit(byte_at(lexer, 0))) {
                if (!append_consumed(lexer, token)) {
                    return false;
                }
            }
        }
    } else if (byte_at(lexer, 0) == 's'
            && !is_identifier_begin(byte_at(lexer, 1))) {
        if (!append_consumed(lexer, token)) {
            return false;
        }
        while (is_digit(byte_at(lexer, 0))) {
            if (!append_consumed(lexer, token)) {
                return false;
            }
        }
    }
    return true;
}

static bool scan_token(st_lexer_t *lexer, st_token_t *token)
{
    bool separated = false;
    bool emitted_error = false;
    unsigned char byte;

    token_reset(token);
    if (!skip_separators(lexer, token, &separated, &emitted_error)) {
        return false;
    }
    if (emitted_error) {
        return true;
    }

    token->separated = separated;
    token->span.begin = current_position(lexer);
    byte = byte_at(lexer, 0);

    if (at_end(lexer)) {
        token->kind = ST_TOKEN_EOF;
        token->span.end = current_position(lexer);
        return true;
    }

    switch (byte) {
    case '[':
        token->kind = ST_TOKEN_LEFT_BRACKET;
        if (!append_consumed(lexer, token)) return false;
        break;
    case ']':
        token->kind = ST_TOKEN_RIGHT_BRACKET;
        if (!append_consumed(lexer, token)) return false;
        break;
    case '(':
        token->kind = ST_TOKEN_LEFT_PAREN;
        if (!append_consumed(lexer, token)) return false;
        break;
    case ')':
        token->kind = ST_TOKEN_RIGHT_PAREN;
        if (!append_consumed(lexer, token)) return false;
        break;
    case '.':
        token->kind = ST_TOKEN_PERIOD;
        if (!append_consumed(lexer, token)) return false;
        break;
    case ':':
        if (byte_at(lexer, 1) == '=') {
            token->kind = ST_TOKEN_ASSIGN;
            if (!append_consumed(lexer, token)
                    || !append_consumed(lexer, token)) return false;
        } else {
            token->kind = ST_TOKEN_COLON;
            if (!append_consumed(lexer, token)) return false;
        }
        break;
    case ';':
        token->kind = ST_TOKEN_SEMICOLON;
        if (!append_consumed(lexer, token)) return false;
        break;
    case '^':
        token->kind = ST_TOKEN_RETURN;
        if (!append_consumed(lexer, token)) return false;
        break;
    case '$':
        token->kind = ST_TOKEN_CHARACTER;
        if (!append_consumed(lexer, token)) return false;
        if (lexer->offset == lexer->source_length) {
            token->kind = ST_TOKEN_UNTERMINATED_CHARACTER;
        } else {
            size_t length = utf8_sequence_length(byte_at(lexer, 0));
            size_t index;
            if (length == 0u || length > lexer->source_length - lexer->offset) {
                token->kind = ST_TOKEN_UNKNOWN;
                if (!append_consumed(lexer, token)) return false;
                break;
            }
            if (!utf8_sequence_is_canonical(lexer, length)) {
                token->kind = ST_TOKEN_UNKNOWN;
            }
            for (index = 0u; index < length; index++) {
                if (!append_consumed(lexer, token)) return false;
            }
        }
        break;
    case '#':
        if (byte_at(lexer, 1) == '(') {
            token->kind = ST_TOKEN_LITERAL_ARRAY_BEGIN;
            if (!append_consumed(lexer, token)
                    || !append_consumed(lexer, token)) return false;
        } else {
            token->kind = ST_TOKEN_SYMBOL_PREFIX;
            if (!append_consumed(lexer, token)) return false;
        }
        break;
    case '\'':
        if (!scan_string(lexer, token)) return false;
        break;
    default:
        if (is_digit(byte)) {
            if (!scan_number(lexer, token)) return false;
        } else if (is_identifier_begin(byte)) {
            do {
                if (!append_consumed(lexer, token)) return false;
            } while (is_identifier_continue(byte_at(lexer, 0)));
            if (byte_at(lexer, 0) == ':' && byte_at(lexer, 1) != '='
                    && byte_at(lexer, 1) != ':') {
                token->kind = ST_TOKEN_KEYWORD;
                if (!append_consumed(lexer, token)) return false;
            } else {
                token->kind = ST_TOKEN_IDENTIFIER;
            }
        } else if ((byte == '-' || byte == '<' || byte == '>')
                && !is_binary_selector(byte_at(lexer, 1))) {
            token->kind = byte == '-' ? ST_TOKEN_MINUS
                : (byte == '<' ? ST_TOKEN_LESS_THAN : ST_TOKEN_GREATER_THAN);
            if (!append_consumed(lexer, token)) return false;
        } else if (byte == '|' && byte_at(lexer, 1) != '|') {
            token->kind = ST_TOKEN_VERTICAL_BAR;
            if (!append_consumed(lexer, token)) return false;
        } else if (is_binary_selector(byte)) {
            token->kind = ST_TOKEN_BINARY_SELECTOR;
            do {
                if (!append_consumed(lexer, token)) return false;
            } while (is_binary_selector(byte_at(lexer, 0)));
        } else {
            token->kind = ST_TOKEN_UNKNOWN;
            if (!append_consumed(lexer, token)) return false;
        }
        break;
    }

    token->span.end = current_position(lexer);
    return true;
}

static bool initialize(st_lexer_t *lexer, const void *source, size_t length)
{
    size_t slot;

    if (lexer == NULL || (source == NULL && length != 0u)) {
        return false;
    }
    memset(lexer, 0, sizeof(*lexer));
    for (slot = 0; slot < ST_LEXER_TOKEN_SLOTS; slot++) {
        token_init(&lexer->tokens[slot]);
        lexer->token_serial[slot] = SIZE_MAX;
    }
    lexer->source = source;
    lexer->source_length = length;
    lexer->line = 1u;
    lexer->column = 1u;
    lexer->status = ST_LEXER_OK;
    if (!scan_token(lexer, &lexer->tokens[0])) {
        st_lexer_status_t status = lexer->status;
        st_lexer_destroy(lexer);
        lexer->status = status;
        return false;
    }
    lexer->token_serial[0] = 0u;
    lexer->produced = 1u;
    return true;
}

bool st_lexer_init_memory(st_lexer_t *lexer, const void *source, size_t length)
{
    if (lexer == NULL) {
        return false;
    }
    if (source == NULL && length != 0u) {
        memset(lexer, 0, sizeof(*lexer));
        lexer->status = ST_LEXER_ERR_INVALID_ARGUMENT;
        return false;
    }
    return initialize(lexer, source, length);
}

bool st_lexer_init_cstr(st_lexer_t *lexer, const char *source)
{
    if (lexer == NULL) {
        return false;
    }
    if (source == NULL) {
        memset(lexer, 0, sizeof(*lexer));
        lexer->status = ST_LEXER_ERR_INVALID_ARGUMENT;
        return false;
    }
    return initialize(lexer, source, strlen(source));
}

bool st_lexer_init_file(st_lexer_t *lexer, FILE *file)
{
    unsigned char *buffer = NULL;
    size_t length = 0u;
    size_t capacity = 0u;

    if (lexer == NULL) {
        return false;
    }
    if (file == NULL) {
        memset(lexer, 0, sizeof(*lexer));
        lexer->status = ST_LEXER_ERR_INVALID_ARGUMENT;
        return false;
    }

    for (;;) {
        size_t available;
        size_t count;
        if (length == capacity) {
            size_t next = capacity == 0u ? 4096u : capacity * 2u;
            unsigned char *grown;
            if (next < capacity) {
                free(buffer);
                memset(lexer, 0, sizeof(*lexer));
                lexer->status = ST_LEXER_ERR_OUT_OF_MEMORY;
                return false;
            }
            grown = realloc(buffer, next);
            if (grown == NULL) {
                free(buffer);
                memset(lexer, 0, sizeof(*lexer));
                lexer->status = ST_LEXER_ERR_OUT_OF_MEMORY;
                return false;
            }
            buffer = grown;
            capacity = next;
        }
        available = capacity - length;
        count = fread(buffer + length, 1u, available, file);
        length += count;
        if (count != available) {
            if (ferror(file)) {
                free(buffer);
                memset(lexer, 0, sizeof(*lexer));
                lexer->status = ST_LEXER_ERR_IO;
                return false;
            }
            break;
        }
    }

    if (!initialize(lexer, buffer, length)) {
        st_lexer_status_t status = lexer->status;
        st_lexer_destroy(lexer);
        free(buffer);
        lexer->status = status;
        return false;
    }
    lexer->owned_source = buffer;
    return true;
}

bool st_lexer_reinit_memory(st_lexer_t *lexer, const void *source,
                            size_t length)
{
    if (lexer == NULL) return false;
    st_lexer_destroy(lexer);
    return st_lexer_init_memory(lexer, source, length);
}

bool st_lexer_reinit_cstr(st_lexer_t *lexer, const char *source)
{
    if (lexer == NULL) return false;
    st_lexer_destroy(lexer);
    return st_lexer_init_cstr(lexer, source);
}

bool st_lexer_reinit_file(st_lexer_t *lexer, FILE *file)
{
    if (lexer == NULL) return false;
    st_lexer_destroy(lexer);
    return st_lexer_init_file(lexer, file);
}

void st_lexer_destroy(st_lexer_t *lexer)
{
    size_t slot;
    if (lexer == NULL) {
        return;
    }
    for (slot = 0; slot < ST_LEXER_TOKEN_SLOTS; slot++) {
        if (lexer->tokens[slot].text != lexer->tokens[slot].inline_text) {
            free(lexer->tokens[slot].text);
        }
    }
    free(lexer->owned_source);
    memset(lexer, 0, sizeof(*lexer));
}

st_lexer_status_t st_lexer_status(const st_lexer_t *lexer)
{
    return lexer == NULL ? ST_LEXER_ERR_INVALID_ARGUMENT : lexer->status;
}

const char *st_lexer_status_string(st_lexer_status_t status)
{
    switch (status) {
    case ST_LEXER_OK: return "ok";
    case ST_LEXER_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_LEXER_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_LEXER_ERR_IO: return "input/output error";
    case ST_LEXER_ERR_LOOKAHEAD: return "lookahead exceeds lexer window";
    default: return "invalid lexer status";
    }
}

static bool ensure_token(st_lexer_t *lexer, size_t serial)
{
    while (lexer->produced <= serial) {
        size_t slot = lexer->produced % ST_LEXER_TOKEN_SLOTS;
        if (lexer->produced != 0u) {
            size_t previous_slot = (lexer->produced - 1u)
                % ST_LEXER_TOKEN_SLOTS;
            if (lexer->tokens[previous_slot].kind == ST_TOKEN_EOF
                    && lexer->token_serial[previous_slot]
                        == lexer->produced - 1u) {
                return true;
            }
        }
        if (!scan_token(lexer, &lexer->tokens[slot])) {
            return false;
        }
        lexer->token_serial[slot] = lexer->produced;
        lexer->produced++;
    }
    return true;
}

const st_token_t *st_lexer_current(const st_lexer_t *lexer)
{
    size_t slot;
    if (lexer == NULL || lexer->status != ST_LEXER_OK
            || lexer->cursor >= lexer->produced) {
        return NULL;
    }
    slot = lexer->cursor % ST_LEXER_TOKEN_SLOTS;
    return lexer->token_serial[slot] == lexer->cursor
        ? &lexer->tokens[slot] : NULL;
}

bool st_lexer_advance(st_lexer_t *lexer, const st_token_t **token_out)
{
    const st_token_t *current_token;
    size_t next;
    if (token_out != NULL) {
        *token_out = NULL;
    }
    if (lexer == NULL || lexer->status != ST_LEXER_OK) {
        return false;
    }
    current_token = st_lexer_current(lexer);
    if (current_token == NULL) return false;
    if (current_token->kind == ST_TOKEN_EOF) {
        if (token_out != NULL) *token_out = current_token;
        return true;
    }
    next = lexer->cursor + 1u;
    if (next < lexer->cursor || !ensure_token(lexer, next)) {
        return false;
    }
    lexer->cursor = next;
    if (token_out != NULL) {
        *token_out = st_lexer_current(lexer);
    }
    return true;
}

bool st_lexer_peek(st_lexer_t *lexer, size_t distance,
                   const st_token_t **token_out)
{
    size_t serial;
    size_t slot;
    if (token_out != NULL) {
        *token_out = NULL;
    }
    if (lexer == NULL || token_out == NULL || lexer->status != ST_LEXER_OK) {
        return false;
    }
    if (distance > ST_LEXER_LOOKAHEAD) {
        lexer->status = ST_LEXER_ERR_LOOKAHEAD;
        return false;
    }
    serial = lexer->cursor + distance;
    if (serial < lexer->cursor || !ensure_token(lexer, serial)) {
        return false;
    }
    if (serial >= lexer->produced) {
        serial = lexer->produced - 1u;
    }
    slot = serial % ST_LEXER_TOKEN_SLOTS;
    if (lexer->token_serial[slot] != serial) {
        lexer->status = ST_LEXER_ERR_LOOKAHEAD;
        return false;
    }
    *token_out = &lexer->tokens[slot];
    return true;
}

const st_token_t *st_lexer_previous(const st_lexer_t *lexer)
{
    size_t serial;
    size_t slot;
    if (lexer == NULL || lexer->status != ST_LEXER_OK || lexer->cursor == 0u) {
        return NULL;
    }
    serial = lexer->cursor - 1u;
    slot = serial % ST_LEXER_TOKEN_SLOTS;
    return lexer->token_serial[slot] == serial ? &lexer->tokens[slot] : NULL;
}

bool st_token_is(const st_token_t *token, st_token_kind_t accepted_kinds)
{
    return token != NULL && (token->kind & accepted_kinds) != 0u;
}

const char *st_token_kind_name(st_token_kind_t kind)
{
    switch (kind) {
    case ST_TOKEN_NONE: return "none";
    case ST_TOKEN_UNKNOWN: return "unknown";
    case ST_TOKEN_NUMBER: return "number";
    case ST_TOKEN_IDENTIFIER: return "identifier";
    case ST_TOKEN_SYMBOL_PREFIX: return "symbol prefix";
    case ST_TOKEN_CHARACTER: return "character";
    case ST_TOKEN_STRING: return "string";
    case ST_TOKEN_BINARY_SELECTOR: return "binary selector";
    case ST_TOKEN_KEYWORD: return "keyword";
    case ST_TOKEN_ASSIGN: return "assignment";
    case ST_TOKEN_RETURN: return "return";
    case ST_TOKEN_PERIOD: return "period";
    case ST_TOKEN_LITERAL_ARRAY_BEGIN: return "literal array begin";
    case ST_TOKEN_LEFT_PAREN: return "left parenthesis";
    case ST_TOKEN_RIGHT_PAREN: return "right parenthesis";
    case ST_TOKEN_LEFT_BRACKET: return "left bracket";
    case ST_TOKEN_RIGHT_BRACKET: return "right bracket";
    case ST_TOKEN_EOF: return "end of input";
    case ST_TOKEN_COLON: return "colon";
    case ST_TOKEN_SEMICOLON: return "semicolon";
    case ST_TOKEN_VERTICAL_BAR: return "vertical bar";
    case ST_TOKEN_LESS_THAN: return "less than";
    case ST_TOKEN_GREATER_THAN: return "greater than";
    case ST_TOKEN_MINUS: return "minus";
    case ST_TOKEN_UNTERMINATED_STRING: return "unterminated string";
    case ST_TOKEN_UNTERMINATED_COMMENT: return "unterminated comment";
    case ST_TOKEN_UNTERMINATED_CHARACTER: return "unterminated character";
    default: return "token set";
    }
}
