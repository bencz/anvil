/* Memory cells a loop reaches through an invariant address, which the
 * compiler may keep in registers: a global accumulator, Brainfuck-style
 * cells tested by the loop (x[i] moved into x[i - 1]), a loop that runs
 * zero times (no store may appear), and loops where a call or another
 * pointer may touch the cell (kept in memory). */
#include <stdio.h>

static int total;
static int seen;
static int cells[64];

static void observe(void) { seen += total; }

static int accumulate(int n)
{
    for (int i = 0; i < n; i++)
        total += i;
    return total;
}

static int move_cell(int at)
{
    while (cells[at]) {
        cells[at - 1]++;
        cells[at]--;
    }
    return cells[at - 1];
}

static int with_call(int n)
{
    for (int i = 0; i < n; i++) {
        total += i;
        observe();
    }
    return seen;
}

static int through(int *other, int n)
{
    for (int i = 0; i < n; i++) {
        total += i;
        *other += total;
    }
    return *other;
}

int main(void)
{
    int a = accumulate(100);
    int zero = accumulate(0);
    cells[10] = 25;
    cells[9] = 4;
    int moved = move_cell(10);
    cells[20] = 0;
    int idle = move_cell(20);
    int called = with_call(10);
    int aliased = through(&total, 5);
    int other = 7;
    int separate = through(&other, 5);
    printf("%d %d %d %d %d %d %d %d %d\n", a, zero, moved, cells[10], idle, called, aliased, separate, total);
    return 0;
}
