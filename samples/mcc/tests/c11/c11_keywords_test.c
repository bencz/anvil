/* Test C11 keywords error messages in C89 mode */

int main(void) {
    /* These should give clear error messages in C89 mode */
    _Static_assert(1, "test");
    _Alignof(int);
    return 0;
}
