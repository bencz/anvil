#include <stdarg.h>
#include <stdio.h>

static int copies(int count, ...)
{
    va_list source;
    va_list saved[3];
    va_start(source, count);
    for (int index = 0; index < 3; index++)
    {
        va_copy(saved[index], source);
        va_arg(source, int);
    }

    int result = 0;
    for (int index = 0; index < 3; index++)
    {
        result = result * 100 + va_arg(saved[index], int);
        va_end(saved[index]);
    }

    va_end(source);
    return result;
}

int main(void)
{
    int result = copies(3, 11, 22, 33, 44, 55, 66);
    printf("%d\n", result);
    return result != 112233;
}
