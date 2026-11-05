#include <stdint.h>

int32_t ksieve(uint8_t *composite, int32_t limit)
{
    int32_t count = 0;
    for (int32_t i = 2; i <= limit; i++) {
        if (composite[i])
            continue;
        count++;
        if (i < 46341)
            for (int32_t j = i * i; j <= limit; j += i)
                composite[j] = 1;
    }
    return count;
}
