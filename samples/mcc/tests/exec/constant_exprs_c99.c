/* Test: integer constant expressions evaluated at parse time: enumerator
 * values, array bounds (variables, typedefs, struct members) built from
 * enumerators, sizeof and arithmetic, and multi-dimensional struct members. */
#include <stdio.h>
enum { A = 1 << 3, B = A * 2, C, D = sizeof(long) > 4 ? 64 : 32, E = (B - C) % 5 };
enum { R = 2, COLS = 3 };
struct rec { int a; char buf[2 * A + 1]; int b; };
struct grid { int tag; int m[R][COLS]; char name[COLS * 2 + 1]; double w[2][2][3]; };
typedef double vec_t[R + 3];
int main(void)
{
    char raw[sizeof(struct rec)];
    int table[A][R * COLS];
    vec_t v = { 1, 2, 3, 4, 5 };
    struct rec r = { 7, "hi", 9 };
    struct grid g = { 1, { { 1, 2, 3 }, { 4, 5, 6 } }, "abc", { { { 1, 2, 3 } }, [1][1][2] = 9 } };
    int s = 0;
    for (int i = 0; i < R; i++) for (int j = 0; j < COLS; j++) s += g.m[i][j] * (i * 10 + j + 1);
    table[A - 1][R * COLS - 1] = 5;
    raw[sizeof raw - 1] = 1;
    printf("%d %d %d %d %d\n", A, B, C, D, E);
    printf("%d %d %d %d %d %d %.0f\n", (int)sizeof raw, (int)sizeof table, (int)sizeof(struct rec),
           (int)sizeof v, r.b, (int)sizeof r.buf, v[4]);
    printf("%d %d %d %d %d %s %.0f %.0f %d\n", (int)sizeof g, (int)sizeof g.m, (int)sizeof g.m[0], g.m[1][0], s,
           g.name, g.w[0][0][2], g.w[1][1][2], (int)((char *)&g.w - (char *)&g));
    return table[A - 1][R * COLS - 1] + raw[sizeof raw - 1];
}
