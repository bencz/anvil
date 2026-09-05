#include <stdio.h>

extern int shared_value;

static int read_shared(void)
{
    return shared_value;
}

int shared_value = 37;
extern int shared_value;
int shared_value;

int main(void)
{
    if (read_shared() != 37)
        return 1;

    shared_value = 42;
    if (read_shared() != 42)
        return 2;

    puts("extern declarations preserve storage ownership");
    return 0;
}
