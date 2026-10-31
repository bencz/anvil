/* Table-driven CRC-32 plus a bitwise variant: shifts, xor, table loads. */
#include <stdio.h>

static unsigned table[256];
static unsigned char buffer[1 << 20];

int main(void)
{
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    for (unsigned i = 0; i < sizeof(buffer); i++)
        buffer[i] = (unsigned char)(i * 31u + (i >> 7));
    unsigned crc = 0xffffffffu;
    for (int rep = 0; rep < 40; rep++)
        for (unsigned i = 0; i < sizeof(buffer); i++)
            crc = table[(crc ^ buffer[i]) & 0xffu] ^ (crc >> 8);
    unsigned slow = 0xffffffffu;
    for (unsigned i = 0; i < sizeof(buffer) / 4; i++) {
        slow ^= buffer[i];
        for (int k = 0; k < 8; k++)
            slow = (slow >> 1) ^ (0xedb88320u & (0u - (slow & 1u)));
    }
    printf("crc32 %08x %08x\n", crc ^ 0xffffffffu, slow ^ 0xffffffffu);
    return 0;
}
