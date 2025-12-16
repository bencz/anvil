/* Test multi-line macros with line continuation */

#define MULTI_LINE_MACRO(a, b, c) \
    do { \
        int x = (a); \
        int y = (b); \
        int z = (c); \
    } while(0)

#define LONG_STRING "This is a very long string that \
continues on the next line"

int main(void) {
    MULTI_LINE_MACRO(1, 2, 3);
    const char *s = LONG_STRING;
    return 0;
}
