#include "bench.h"

uint32_t klex(const uint8_t *s, size_t n);

/* Source-like text: identifiers, numbers, operators, brackets, spaces. */
static uint8_t text[BENCH_PICK(1 << 20, 1 << 14)];

int main(void)
{
    static const char sample[] = "int x1 = (Alpha + beta_7) * 42; { y = z / 3; }\n\tif (k != 0) Run(q);\n";
    for (size_t i = 0; i < sizeof(text); i++)
        text[i] = (uint8_t)sample[(i * 7 + i / 13) % (sizeof(sample) - 1)];
    uint32_t h = 0;
    for (int rep = 0; rep < BENCH_PICK(40, 4); rep++)
        h ^= klex(text, sizeof(text)) + (uint32_t)rep;
    printf("lexer %u\n", h);
    return 0;
}
