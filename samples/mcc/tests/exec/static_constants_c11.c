/* Test: address constants (functions, &object, &a[i], &s.m, p +/- k, casts,
 * string literal offsets) and unions initialized through any member, in
 * file-scope and block-scope static storage. */
#include <stdio.h>
#include <string.h>

typedef int (*binop_t)(int, int);
static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
int mul(int a, int b) { return a * b; }

struct point { int x, y; };
struct shape { const char *name; struct point corners[3]; int sides; };

int g_values[6] = { 10, 20, 30, 40, 50, 60 };
struct shape g_tri = { "tri", { { 0, 0 }, { 4, 0 }, { 0, 3 } }, 3 };
static double g_scale = 2.5;

/* Address constants. */
int *p_first = g_values;
int *p_third = &g_values[2];
int *p_last = g_values + 5;
int *p_back = &g_values[5] - 2;
int *p_y = &g_tri.corners[1].y;
struct point *p_corner = &g_tri.corners[2];
const char *p_name_tail = "hello" + 2;
void *p_void = &g_scale;
char *p_bytes = (char *)&g_values[1];
double *p_scale = &g_scale;
binop_t ops[] = { add, &sub, mul, 0 };
struct { const char *label; binop_t fn; int *data; } table[] = {
    { "add", add, &g_values[0] }, { "sub", sub, g_values + 1 }, { "mul", &mul, 0 }
};
int **pp = &p_third;

/* Unions initialized through any member. */
union num { char c; short s; int i; double d; float f[2]; };
union num u_char = { 'A' };
union num u_int = { .i = 0x01020304 };
union num u_float = { .f = { 1.5f, -2.25f } };
union num u_double = { .d = 3.75 };
union tagged { struct { unsigned char tag; unsigned char data[7]; } b; long long whole; };
union tagged u_tagged = { .b = { 9, { 1, 2, 3 } } };
struct holder { int before; union num n; int after; } holder = { 1, { .s = -2 }, 3 };

static int use_static_locals(void)
{
    static int hits;
    static int *self = &hits;
    static const char *msgs[] = { "zero", "one", "two" };
    static const char **cursor = &msgs[1];
    (*self)++;
    return hits * 100 + (int)strlen(*cursor) + (int)(cursor - msgs);
}

int main(void)
{
    printf("%d %d %d %d %d %d\n", *p_first, *p_third, *p_last, *p_back, *p_y, p_corner->y);
    printf("%s %.2f %d %d\n", p_name_tail, *(double *)p_void, p_bytes[0] == ((char *)g_values)[4], (int)(*p_scale * 2));
    for (int i = 0; ops[i]; i++) printf("op%d=%d ", i, ops[i](7, 3));
    printf("\n");
    for (int i = 0; i < 3; i++)
        printf("%s:%d:%d ", table[i].label, table[i].fn(5, 2), table[i].data ? *table[i].data : -1);
    printf("\n**pp=%d\n", **pp);
    printf("u_char=%d u_int=%d u_float=%.2f,%.2f u_double=%.2f\n",
           u_char.c, u_int.i, u_float.f[0], u_float.f[1], u_double.d);
    unsigned char raw[sizeof(union num)];
    memcpy(raw, &u_int, sizeof raw);
    printf("u_int bytes %d %d %d %d\n", raw[0], raw[1], raw[2], raw[3]);
    printf("u_tagged %d %d %d %lld\n", u_tagged.b.tag, u_tagged.b.data[2], u_tagged.b.data[3], u_tagged.whole);
    printf("holder %d %d %d\n", holder.before, holder.n.s, holder.after);
    use_static_locals();
    printf("statics %d\n", use_static_locals());
    return 0;
}
