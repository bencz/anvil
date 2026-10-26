/* Test: brace initializers (C11 6.7.9): nesting, brace elision, designators,
 * overrides that reset subobjects, unions, anonymous members, inferred array
 * sizes, compound literals and static storage. */
#include <stdio.h>
struct In { int a, b; };
struct Out { char tag[6]; struct In in[2]; double d; int tail; };
union U { int i; char c[4]; };
struct WithU { int k; union U u; };
struct Anon { int x; struct { int y, z; }; int w; };

struct Out g_out = { "glob", { {1, 2}, {3} }, 2.5, 9 };
struct Out g_elide = { "ab", 10, 20, 30, 40, 1.0, 7 };
int g_arr[] = { [3] = 4, 5, [0] = 1 };
struct In g_ins[] = { {1, 2}, [2] = {5, 6} };
static int g_static = 17;
double g_d[3] = { 1, 2 };
const char *g_names[] = { "x", "yy", 0 };
union U g_u = { 0x01020304 };
struct Anon g_anon = { 1, { 2, 3 }, 4 };
struct Anon g_anon_d = { .z = 9, .x = 1 };

static int sum_in(const struct In *p, int n) { int s = 0, i; for (i = 0; i < n; i++) s += p[i].a * 10 + p[i].b; return s; }

int counter(void) { static struct In st = { 5, 6 }; st.a++; return st.a * 100 + st.b; }

int main(void)
{
    struct Out o = { "loc", { {1, 2}, {3} }, 2.5, 9 };
    struct Out e = { "ab", 10, 20, 30, 40, 1.0, 7 };
    struct Out d = { .in[1].b = 8, .tag = "z", .tail = 3, .in = { {4} } };
    int arr[] = { [3] = 4, 5, [0] = 1 };
    int over[4] = { 1, 2, 3, 4, [1] = 9 };
    struct In ins[3] = { [1] = { .b = 7 }, { 8 } };
    union U u = { .c = { 1, 2 } };
    struct WithU wu = { 1, { 77 } };
    struct Anon an = { 1, 2, 3, 4 };
    struct Anon and = { .y = 5, 6, 7 };
    char str[8] = "hi";
    char strb[] = { "brace" };
    int matrix[2][3] = { 1, 2, 3, 4 };
    int *cl = (int[]){ 4, 5, 6 };
    struct In *cp = &(struct In){ .b = 3 };
    int i;
    printf("o %s %d %d %d %d %g %d\n", o.tag, o.in[0].a, o.in[0].b, o.in[1].a, o.in[1].b, o.d, o.tail);
    printf("e %s %d %d %d %d %g %d\n", e.tag, e.in[0].a, e.in[0].b, e.in[1].a, e.in[1].b, e.d, e.tail);
    printf("d %s %d %d %d %d %g %d\n", d.tag, d.in[0].a, d.in[0].b, d.in[1].a, d.in[1].b, d.d, d.tail);
    printf("arr %d", (int)(sizeof arr / sizeof arr[0])); for (i = 0; i < 5; i++) printf(" %d", arr[i]); printf("\n");
    printf("over %d %d %d %d\n", over[0], over[1], over[2], over[3]);
    printf("ins %d\n", sum_in(ins, 3));
    printf("u %d %d %d %d\n", u.c[0], u.c[1], u.c[2], u.c[3]);
    printf("wu %d %d\n", wu.k, wu.u.i);
    printf("an %d %d %d %d / %d %d %d %d\n", an.x, an.y, an.z, an.w, and.x, and.y, and.z, and.w);
    printf("str %s %d %d | %s %d\n", str, str[2], str[7], strb, (int)sizeof strb);
    printf("matrix"); for (i = 0; i < 6; i++) printf(" %d", matrix[i / 3][i % 3]); printf("\n");
    printf("cl %d %d %d cp %d %d\n", cl[0], cl[1], cl[2], cp->a, cp->b);
    printf("g_out %s %d %d %d %d %g %d\n", g_out.tag, g_out.in[0].a, g_out.in[0].b, g_out.in[1].a, g_out.in[1].b, g_out.d, g_out.tail);
    printf("g_elide %s %d %d %d %d %g %d\n", g_elide.tag, g_elide.in[0].a, g_elide.in[0].b, g_elide.in[1].a, g_elide.in[1].b, g_elide.d, g_elide.tail);
    printf("g_arr %d", (int)(sizeof g_arr / sizeof g_arr[0])); for (i = 0; i < 5; i++) printf(" %d", g_arr[i]); printf("\n");
    printf("g_ins %d %d\n", (int)(sizeof g_ins / sizeof g_ins[0]), sum_in(g_ins, 3));
    printf("g_static %d g_d %g %g %g\n", g_static, g_d[0], g_d[1], g_d[2]);
    printf("g_names %s %s %d\n", g_names[0], g_names[1], g_names[2] == 0);
    printf("g_u %d\n", g_u.i);
    printf("g_anon %d %d %d %d / %d %d %d %d\n", g_anon.x, g_anon.y, g_anon.z, g_anon.w, g_anon_d.x, g_anon_d.y, g_anon_d.z, g_anon_d.w);
    counter(); counter();
    printf("counter %d\n", counter());
    return 0;
}
