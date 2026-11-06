/* Accesses at constant distances from one index (a[i - 1], a[i + 1]) with
 * int, unsigned and short indices, in loops whose body branches, and loops
 * that leave after one trip with values read after them: the shapes that
 * address a neighbour from the index's own address, count in a counter
 * register and thread a loop's back edge out. */
#include <stdio.h>

static int tape[64];

static void drain_all(int n)
{
    for (int k = 1; k < n; k++)
        if (tape[k]) {
            tape[k - 1] += 3 * tape[k];
            tape[k] = 0;
        }
}

static long smooth(const int *a, unsigned n)
{
    long total = 0;
    for (unsigned i = 1; i + 1 < n; i++)
        total += a[i - 1] - 2 * a[i] + a[i + 1];
    return total;
}

static int shorts(const int *a, short n)
{
    int best = 0;
    for (short i = 2; i < n; i++) {
        int d = a[i] - a[i - 2];
        if (d > best)
            best = d;
    }
    return best;
}

static int once(int c, int *out)
{
    int s = 10, n = c;
    while (n != 0) {
        s += n;
        *out = s;
        n = 0;
    }
    return s * 100 + n;
}

static int swap_until(const int *a, int limit)
{
    int x = 0, y = 1, i = 0;
    while (i < limit && a[i]) {
        int t = x;
        x = y;
        y = t + a[i];
        i++;
    }
    return x * 1000 + y;
}

int main(void)
{
    int data[40];
    for (int i = 0; i < 40; i++)
        data[i] = (i * 37 + 11) % 23 - 9;
    for (int i = 0; i < 64; i++)
        tape[i] = (i * 5) % 7;
    drain_all(64);
    unsigned h = 0;
    for (int i = 0; i < 64; i++)
        h = h * 31 + (unsigned)tape[i];
    int cell = 0;
    int first = once(5, &cell);
    int second = once(0, &cell);
    data[17] = 0;
    printf("%u %ld %d %d %d %d %d %d\n", h, smooth(data, 40), smooth(data, 1) == 0, shorts(data, 40), first, second, cell,
           swap_until(data, 40));
    return 0;
}
