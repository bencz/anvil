/*
 * Cross-Standard Test: C23 features compiled with -std=c89
 * Expected: Warnings/errors for C23 features used in C89 mode
 * 
 * Run with: ./mcc -std=c89 -fsyntax-only tests/cross/c23_in_c89.c
 * Expected output: Multiple warnings/errors about C23 extensions
 */

/* C23: [[attributes]] - should warn */
[[deprecated]] int old_func(void);

/* C23: bool as keyword - should warn */
bool flag = 1;

/* C23: nullptr - should warn/error */
void *null_ptr = nullptr;

/* C23: Binary literals - should warn */
int binary = 0b1010;

/* C23: Digit separators - should warn */
int million = 1'000'000;

/* C23: typeof - should warn */
int x = 10;
typeof(x) y = 20;

int main(void)
{
    return 0;
}
