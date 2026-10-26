/* Test: static storage: block-scope statics keep their value and their
 * initializer, file-scope statics have internal linkage, static functions
 * may be used before their definition, and global structs are addressable
 * member by member. */
#include <stdio.h>

struct pair { int a, b; };
struct pair g_pair = { 3, 4 };
static int g_hidden = 17;
static int forward(int);

static int counter(void)
{
    static int calls = 40;
    static int history[3] = { 1 };
    static struct pair last = { 5, 6 };
    calls++;
    history[0]++;
    last.a += last.b;
    return calls * 1000 + history[0] * 100 + last.a;
}

int main(void)
{
    counter();
    counter();
    g_pair.b += g_hidden;
    printf("%d %d %d %d\n", counter(), g_pair.a * 100 + g_pair.b,
           forward(5), g_hidden);
    return 0;
}

static int forward(int x) { return x * g_hidden; }
