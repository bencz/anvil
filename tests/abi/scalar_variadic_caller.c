#include "scalar_variadic.h"
#include <stdio.h>
#include <stddef.h>

static int check_cursor_objects(int count, ...)
{
    va_list source;
    va_list copies[3];
    va_start(source, count);
    int correct = 1;
    for (int index = 0; index < 3; index++)
    {
        va_copy(copies[index], source);
        correct &= abi_cursor_next(&source) == 11 * (index + 1);
    }

    for (int index = 0; index < 3; index++)
    {
        correct &= abi_cursor_next(&copies[index]) == 11 * (index + 1);
        va_end(copies[index]);
    }

    va_end(source);
    return correct;
}

int main(void)
{
    if (abi_cursor_size() != sizeof(va_list) || abi_cursor_offset() != offsetof(abi_cursor_record, cursor) ||
        abi_cursor_record_size() != sizeof(abi_cursor_record) || !check_cursor_objects(3, 11, 22, 33, 44, 55, 66))
    {
        fprintf(stderr, "native va_list layout or independent copy mismatch\n");
        return 1;
    }

    double sum = abi_variadic_sum(12, 0.25, 1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5, 6, 6.5,
                                 7, 7.5, 8, 8.5, 9, 9.5, 10, 10.5, 11, 11.5, 12, 12.5);
    double exhausted = abi_variadic_exhausted(1, 2, 3, 4, 5, 6, 7, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 11, 12.5, 13);
    int length = abi_variadic_forward("%d %.2f %s\n", 42, 1.25, "abi");
    if (sum != 162.25 || exhausted != 114.0 || length != 12)
    {
        fprintf(stderr, "variadic ABI mismatch: sum=%.2f exhausted=%.2f length=%d\n", sum, exhausted, length);
        return 1;
    }

    return 0;
}
