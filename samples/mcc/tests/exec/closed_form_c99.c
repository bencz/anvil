/* Loops that count to zero and only add invariants, which the compiler may
 * run in closed form: a counter going down or up, values and cells that
 * grow by an invariant, two updates of one cell, a loop that never runs,
 * an unsigned counter that wraps on its way to zero, and the counter and
 * the values read after the loop. */
#include <stdio.h>

static int tape[16];

static int drain(int at, int step)
{
    while (tape[at]) {
        tape[at - 1] += step;
        tape[at]--;
    }
    return tape[at - 1];
}

static long sums(int c, int k, int *cell, int *twice)
{
    long total = 5;
    while (c != 0) {
        c--;
        total += k;
        *cell += 3;
        *twice += 1;
        *twice += 2;
    }
    return total * 1000 + c;
}

static unsigned up(int start)
{
    unsigned left = 0;
    int c = start;
    while (c != 0) {
        c++;
        left += 7;
    }
    return left;
}

static unsigned wraps(unsigned c)
{
    unsigned n = 0;
    while (c != 0) {
        c++;
        n += 1;
    }
    return n;
}

int main(void)
{
    tape[5] = 40;
    tape[4] = 2;
    int moved = drain(5, 3);
    tape[8] = 0;
    tape[7] = 9;
    int idle = drain(8, 5);
    int cell = 1, twice = 0;
    long s = sums(12, -4, &cell, &twice);
    long none = sums(0, 9, &cell, &twice);
    printf("%d %d %d %d %ld %ld %d %d %u %u\n", moved, tape[5], idle, tape[8], s, none, cell, twice, up(-25), wraps(4294967290u));
    return 0;
}
