#include <stdio.h>
#include <string.h>

/* The # operator spells its argument as a string literal (escaping the
 * quotes and backslashes of literals); a string literal is an array of its
 * characters and the terminator, so sizeof counts them. */
#define STR(x) #x
#define XSTR(x) STR(x)
#define VALUE 42

static const char table[sizeof("hello")] = "hi";

int main(void)
{
    printf("[%s] [%s] [%s] [%s] [%s]\n", STR(u3), STR(a + b), STR("q\n"), STR('"'), XSTR(VALUE));
    printf("%d %d\n", (int)strlen(STR(abc)), (int)strlen(STR("x\\y")));
    printf("%d %d %d %d %d\n", (int)sizeof("abc"), (int)sizeof(STR(abc)), (int)sizeof("ab" "cd"), (int)sizeof("a\n\0b"), (int)sizeof table);
    return 0;
}
