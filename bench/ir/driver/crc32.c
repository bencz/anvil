#include "bench.h"

void kcrc_table(uint32_t *t);
uint32_t kcrc32(uint32_t crc, const uint8_t *buf, size_t len, const uint32_t *t);

static uint8_t buffer[BENCH_PICK(1 << 20, 1 << 14)];
static uint32_t table[256];

int main(void)
{
    for (size_t i = 0; i < sizeof(buffer); i++)
        buffer[i] = (uint8_t)(i * 31 + (i >> 7));
    uint32_t crc = 0xFFFFFFFFu;
    /* Built once: the measured work is the table-driven loop (a compiler
     * may evaluate the whole table at compile time). */
    kcrc_table(table);
    for (int rep = 0; rep < BENCH_PICK(60, 4); rep++)
        crc = kcrc32(crc, buffer, sizeof(buffer), table);
    printf("crc32 %08x %08x\n", crc ^ 0xFFFFFFFFu, table[255]);
    return 0;
}
