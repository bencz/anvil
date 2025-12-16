/*
 * Utility functions - another compilation unit
 * Tests multi-file compilation support
 */

int abs_val(int x)
{
    if (x < 0) {
        return -x;
    }
    return x;
}

int max(int a, int b)
{
    if (a > b) {
        return a;
    }
    return b;
}

int min(int a, int b)
{
    if (a < b) {
        return a;
    }
    return b;
}
