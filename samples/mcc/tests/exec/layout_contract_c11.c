#include <stddef.h>

#ifndef EXPECT_PROBE_SIZE
#define EXPECT_PROBE_SIZE 24
#define EXPECT_PROBE_ALIGN 8
#define EXPECT_LL_OFFSET 8
#define EXPECT_DOUBLE_OFFSET 16
#define EXPECT_ARRAY_SIZE 16
#define EXPECT_PTR_OFFSET 8
#endif

struct Probe {
    char tag;
    long long wide;
    double real;
};

union Storage {
    char bytes[9];
    int word;
};

struct ArrayLayout {
    char tag;
    short lanes[3];
    void *ptr;
};

enum ContractEnum {
    CONTRACT_ZERO,
    CONTRACT_MAX = 2147483647
};

_Static_assert(sizeof(struct Probe) == EXPECT_PROBE_SIZE,
               "Probe target size mismatch");
_Static_assert(_Alignof(struct Probe) == EXPECT_PROBE_ALIGN,
               "Probe target alignment mismatch");
_Static_assert(sizeof(union Storage) == 12,
               "union max-size/tail-padding mismatch");
_Static_assert(_Alignof(union Storage) == 4,
               "union alignment mismatch");
_Static_assert(sizeof(struct ArrayLayout) == EXPECT_ARRAY_SIZE,
               "array/nested target layout mismatch");
_Static_assert(sizeof(((struct ArrayLayout *)0)->lanes) == 6,
               "member array size mismatch");
_Static_assert(offsetof(struct Probe, wide) == EXPECT_LL_OFFSET,
               "offsetof wide mismatch");
_Static_assert(offsetof(struct Probe, real) == EXPECT_DOUBLE_OFFSET,
               "offsetof real mismatch");
_Static_assert(offsetof(struct ArrayLayout, ptr) == EXPECT_PTR_OFFSET,
               "offsetof pointer mismatch");
_Static_assert(sizeof(enum ContractEnum) == 4,
               "enum must use target int storage");
_Static_assert(_Alignof(enum ContractEnum) == 4,
               "enum must use target int ABI alignment");

int main(void)
{
    if (offsetof(struct Probe, tag) != 0) return 1;
    if (offsetof(struct Probe, wide) != EXPECT_LL_OFFSET) return 2;
    if (offsetof(struct Probe, real) != EXPECT_DOUBLE_OFFSET) return 3;
    if (offsetof(struct ArrayLayout, lanes) != 2) return 4;
    if (offsetof(struct ArrayLayout, ptr) != EXPECT_PTR_OFFSET) return 5;
    return 0;
}
