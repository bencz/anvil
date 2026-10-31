#include "../systemz_internal.h"

void systemz_uppercase(char *dest, const char *src, size_t max_len)
{
    size_t i = 0;
    if (!dest || max_len == 0)
        return;
    if (!src)
        src = "ANON";
    for (; i + 1 < max_len && src[i]; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - ('a' - 'A'));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            dest[i] = c;
        } else {
            dest[i] = '_';
        }
    }
    dest[i] = '\0';
}

/* A lowercase C-style name of at most 8 characters, which uppercases to a
 * valid ordinary symbol no other mapped name can produce. */
static bool systemz_plain_name(const char *src, size_t length)
{
    if (length == 0 || length > 8 || (src[0] >= '0' && src[0] <= '9'))
        return false;
    for (size_t i = 0; i < length; i++) {
        char c = src[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

/* Every source symbol maps to one HLASM name, the same in every module:
 * plain names uppercase, any other name (mixed case, other characters,
 * longer than an OBJ external's 8 characters) keeps up to three leading
 * characters and gets '@' and a hash of the full name. '@' never occurs in
 * a plain name, and internal labels use '#', so the classes cannot
 * collide. */
void systemz_symbol_name(char *dest, size_t max_len, const char *src)
{
    if (!dest || max_len < 9)
        return;
    if (!src || !src[0])
        src = "anon";
    size_t length = strlen(src);
    if (systemz_plain_name(src, length)) {
        systemz_uppercase(dest, src, max_len);
        return;
    }

    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++)
        hash = (hash ^ (unsigned char)src[i]) * UINT64_C(1099511628211);

    static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
    size_t out = 0;
    for (size_t i = 0; i < length && out < 3; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - ('a' - 'A'));
        bool letter = c >= 'A' && c <= 'Z';
        if (letter || (out > 0 && ((c >= '0' && c <= '9') || c == '_')))
            dest[out++] = c;
        else if (out == 0)
            break;
    }
    dest[out++] = '@';
    while (out < 8) {
        dest[out++] = digits[hash & 31];
        hash >>= 5;
    }
    dest[out] = '\0';
}

bool systemz_symbol_is_renamed(const char *src)
{
    return !src || !systemz_plain_name(src, strlen(src));
}

/* `ENTRY`/`EXTRN` for a source symbol; a renamed symbol also gets an ALIAS
 * so the binder resolves it by its source spelling. */
void systemz_emit_external(anvil_strbuf_t *out, const char *directive, const char *src)
{
    char name[16];
    systemz_symbol_name(name, sizeof(name), src);
    anvil_strbuf_appendf(out, "         %-5s %s\n", directive, name);
    if (!src || !systemz_symbol_is_renamed(src) || strchr(src, '\'') || strchr(src, '&'))
        return;

    /* Statements end in column 71; a longer one continues with a mark in
     * column 72 and resumes in column 16. */
    char line[512];
    int length = snprintf(line, sizeof(line), "%-8s ALIAS C'%s'", name, src);
    if (length < 0 || (size_t)length >= sizeof(line))
        return;
    size_t done = (size_t)length <= 71 ? (size_t)length : 71;
    anvil_strbuf_appendf(out, "%.*s%s\n", (int)done, line, (size_t)length > done ? "X" : "");
    while (done < (size_t)length) {
        size_t chunk = (size_t)length - done > 56 ? 56 : (size_t)length - done;
        anvil_strbuf_appendf(out, "               %.*s%s\n", (int)chunk, line + done, done + chunk < (size_t)length ? "X" : "");
        done += chunk;
    }
}
