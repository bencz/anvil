/* Test: labels (case and goto targets) nested inside statements that
 * sequential control flow never reaches. */
#include <stdio.h>
static int f(int x) {
    int r = 0;
    switch (x) {
    case 0: r = 1; break;
        if (x > 100) { case 1: r += 10; }
        while (r < 50) { case 2: r += 20; }
        break;
    default: r = -1;
    }
    return r;
}
static int g(int x) { int r = 0; goto skip; { int k = 5; r = k; L: r += 3; } skip: if (x) { x = 0; goto L; } return r; }
int main(void) { printf("%d %d %d %d %d %d\n", f(0), f(1), f(2), f(3), g(0), g(1)); return 0; }
