/*
 * Cross-Standard Test: C23 features compiled with -std=c11
 * Expected: Warnings/errors for C23 features used in C11 mode
 * 
 * Run with: ./mcc -std=c11 -fsyntax-only tests/cross/c23_in_c11.c
 * Expected output: Multiple warnings about C23 extensions
 */

/* C23: true/false as keywords - in C11 they require stdbool.h */
int flag1 = true;   /* Should warn - true is not a keyword in C11 */
int flag2 = false;  /* Should warn - false is not a keyword in C11 */

/* C23: nullptr - should warn/error */
int *null_ptr = nullptr;

/* C23: bool as keyword - should warn */
bool b1 = 1;  /* In C11, bool requires stdbool.h */

/* C23: typeof - should warn */
int x = 10;
typeof(x) y = 20;

/* C23: typeof_unqual - should warn */
const int cx = 100;
typeof_unqual(cx) mutable_copy = cx;

/* C23: [[attributes]] - should warn */
[[deprecated]] int old_func(void);
[[nodiscard]] int must_check(void);
[[maybe_unused]] static int unused_var = 0;

/* C23: Binary literals - should warn */
int binary = 0b1010;

/* C23: Digit separators - should warn */
int million = 1'000'000;

/* C23: u8 character literals - should warn */
unsigned char u8char = u8'A';

/* C23: static_assert without message - should warn */
static_assert(sizeof(int) >= 4);

int main(void)
{
    return 0;
}
