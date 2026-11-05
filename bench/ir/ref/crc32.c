#include <stddef.h>
#include <stdint.h>

void kcrc_table(uint32_t *t)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int32_t k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ c >> 1 : c >> 1;
        t[i] = c;
    }
}

uint32_t kcrc32(uint32_t crc, const uint8_t *buf, size_t len, const uint32_t *t)
{
    for (size_t i = 0; i < len; i++)
        crc = t[(crc ^ buf[i]) & 255] ^ crc >> 8;
    return crc;
}
