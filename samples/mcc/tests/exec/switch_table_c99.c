/* Switches that only choose values become table lookups: holes and values
 * past the range take the default, a default that is code keeps its own
 * path, two values chosen per case use two tables, and negative or
 * signed-narrow selectors index from the lowest case. */
#include <stdio.h>

static int holes(int x)
{
    switch (x) {
    case 3: return 30; case 4: return 40; case 6: return 60; case 7: return 70;
    case 9: return 90; case 10: return 100; case 12: return 120;
    default: return -1;
    }
}

static int code_default(int x, int *side)
{
    int r;
    switch (x) {
    case 0: r = 5; break;
    case 1: r = 9; break;
    case 2: r = 2; break;
    case 3: r = 7; break;
    case 4: r = 1; break;
    default: *side += x; r = x * 3; break;
    }
    return r + 1;
}

static int two_values(int x, int *second)
{
    int a, b;
    switch (x) {
    case 10: a = 1; b = 100; break;
    case 11: a = 2; b = 200; break;
    case 12: a = 3; b = 300; break;
    case 13: a = 4; b = 400; break;
    case 15: a = 5; b = 500; break;
    default: a = 0; b = -7; break;
    }
    *second = b;
    return a;
}

static int negative(int x)
{
    switch (x) {
    case -3: return 1; case -2: return 2; case -1: return 3; case 0: return 4; case 1: return 5; case 2: return 6;
    default: return 0;
    }
}

static int narrow(signed char c)
{
    switch (c) {
    case -128: return 1; case -127: return 2; case -126: return 3; case -125: return 4; case 127: return 9;
    default: return 0;
    }
}

int main(void)
{
    unsigned long check = 0;
    int side = 0;
    for (int x = -300; x < 300; x++) {
        int second = 0;
        check = check * 31 + (unsigned long)(holes(x) + 2);
        check = check * 31 + (unsigned long)(code_default(x, &side) + 1000);
        check = check * 31 + (unsigned long)two_values(x, &second);
        check = check * 31 + (unsigned long)(second + 10);
        check = check * 31 + (unsigned long)negative(x);
        check = check * 31 + (unsigned long)narrow((signed char)x);
    }
    printf("switch table %lu %d\n", check, side);
    return 0;
}
