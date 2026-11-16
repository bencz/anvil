/* Elements reached through constant subscripts of nested arrays and of
 * structs in arrays, global and local, mixed with variable subscripts and
 * with a local whose address escapes: one address per access (a
 * displacement from the object, a frame operand on x86), the C values on
 * every target. */
#include <stdio.h>

struct cell {
    short tag;
    int value[3];
};

static int cube[2][3][4];
static struct cell cells[4];

static int sum_cube(void)
{
    return cube[0][1][1] + cube[0][2][3] + cube[1][1][2] + cube[1][2][0];
}

static int sum_cells(int k)
{
    return cells[1].value[2] + cells[3].tag + cells[k & 3].value[1] + cells[2].value[k % 3];
}

static void fill(int *p, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = p[i] * 3 + i;
}

static int local_grid(int k)
{
    int grid[3][4];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            grid[i][j] = i * 10 + j + k;
    fill(&grid[1][0], 4);
    return grid[0][3] + grid[1][2] + grid[2][1] + grid[k % 3][k % 4];
}

int main(void)
{
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 4; k++)
                cube[i][j][k] = i * 100 + j * 10 + k;
    for (int i = 0; i < 4; i++) {
        cells[i].tag = (short)(i - 2);
        for (int j = 0; j < 3; j++)
            cells[i].value[j] = i * 7 + j;
    }
    printf("%d\n", sum_cube());
    for (int k = 0; k < 6; k++)
        printf("%d %d\n", sum_cells(k), local_grid(k));
    return 0;
}
