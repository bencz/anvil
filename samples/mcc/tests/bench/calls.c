/* Small static helpers called from a hot loop: call overhead and what
 * inlining them exposes (folded constants, registers across the body). */
#include <stdio.h>

typedef struct { int x, y; } point_t;

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int dot(point_t a, point_t b) { return a.x * b.x + a.y * b.y; }
static point_t step(point_t p, int k) { point_t q = { clamp(p.x + k, -1000, 1000), clamp(p.y - k, -1000, 1000) }; return q; }
static unsigned mix(unsigned h, int v) { return (h ^ (unsigned)v) * 16777619u; }

int main(void)
{
    point_t p = { 3, -7 }, axis = { 5, 2 };
    unsigned h = 2166136261u;
    for (int i = 0; i < 60000000; i++) {
        p = step(p, (i & 15) - 7);
        h = mix(h, dot(p, axis));
    }
    printf("calls %u %d %d\n", h, p.x, p.y);
    return 0;
}
