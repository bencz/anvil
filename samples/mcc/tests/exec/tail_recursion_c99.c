#include <stdio.h>

/* Recursive returns as loops: f(args) (gcd, void countdown), x + f(args)
 * (sumto, fib, a tree sum with two calls), and a function with a local
 * array that keeps its frames.  Every result must match. */
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static unsigned gcd(unsigned a, unsigned b) { return b == 0 ? a : gcd(b, a % b); }
static long sumto(long n) { if (n == 0) return 0; return n + sumto(n - 1); }
static int count = 0;
static void countdown(int n) { if (n <= 0) return; count += n; countdown(n - 1); }
static int local(int n) { int buf[4]; buf[n & 3] = n; if (n <= 0) return buf[0] * 0; return buf[n & 3] + local(n - 1); }
static int depth(const int *t, int i, int n) { if (i >= n) return 0; return t[i] + depth(t, 2 * i + 1, n) + depth(t, 2 * i + 2, n); }
int main(void) {
    int t[31]; for (int i = 0; i < 31; i++) t[i] = i * 3 - 7;
    countdown(100);
    printf("%d %u %ld %d %d %d\n", fib(30), gcd(1071u, 462u), sumto(100000), count, local(20), depth(t, 0, 31));
    return 0;
}
