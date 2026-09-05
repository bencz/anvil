#include <stdio.h>

int main(void)
{
    int count = fprintf(stdout, "standard output %d\n", 42);
    int error = fprintf(stderr, "standard error %d\n", 7);
    if (fflush(stdout) || fflush(stderr))
        return 1;

    return count == 19 && error == 17 ? 0 : 1;
}
