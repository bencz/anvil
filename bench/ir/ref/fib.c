#include <stdint.h>

int32_t kfib(int32_t n)
{
    return n < 2 ? n : kfib(n - 1) + kfib(n - 2);
}
