#include "bench.h"

uint32_t khex(const uint8_t *s, size_t n, uint8_t *out);

static uint8_t data[BENCH_PICK(1 << 20, 1 << 14)];
static uint8_t text[2 * sizeof(data)];

int main(void)
{
    uint32_t seed = 12345;
    for (size_t i = 0; i < sizeof(data); i++) {
        seed = seed * 1103515245u + 12345u;
        data[i] = (uint8_t)(seed >> 16);
    }
    uint32_t h = 0;
    for (int rep = 0; rep < BENCH_PICK(40, 4); rep++)
        h ^= khex(data, sizeof(data), text) + (uint32_t)rep;
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof(text); i += 97)
        sum = sum * 33 + text[i];
    printf("hex %u %u\n", h, sum);
    return 0;
}
