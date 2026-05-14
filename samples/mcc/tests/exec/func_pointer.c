/* Pointer-to-function dispatch table. */

int putchar(int c);

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }

typedef int (*binop)(int, int);

int main(void) {
    binop ops[3];
    ops[0] = add;
    ops[1] = sub;
    ops[2] = mul;

    int r0 = ops[0](3, 4);   /* 7 */
    int r1 = ops[1](10, 4);  /* 6 */
    int r2 = ops[2](5, 6);   /* 30 — two digits */

    putchar('0' + r0);
    putchar('0' + r1);
    putchar('0' + (r2 / 10));
    putchar('0' + (r2 % 10));
    putchar('\n');
    return 0;
}
