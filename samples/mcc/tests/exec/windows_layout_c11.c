#include <stddef.h>
#include <stdint.h>

int main(void)
{
#ifdef _WIN64
    int items[3];

    _Static_assert(sizeof(long) == 4, "Windows long is 32 bits");
    _Static_assert(sizeof(void *) == 8, "Windows x64 pointers are 64 bits");
    _Static_assert(sizeof(size_t) == 8, "size_t must hold a pointer-sized count");
    _Static_assert(sizeof(ptrdiff_t) == 8, "ptrdiff_t must hold pointer differences");
    _Static_assert(sizeof(uintptr_t) == 8, "uintptr_t must hold pointers");
    _Static_assert(sizeof(wchar_t) == 2, "Windows wchar_t is 16 bits");

    if (!_Generic(sizeof(int), size_t: 1, default: 0))
        return 1;

    if (!_Generic(&items[2] - &items[0], ptrdiff_t: 1, default: 0))
        return 2;

    if (!_Generic(_Alignof(int), size_t: 1, default: 0))
        return 3;
#endif

    return 0;
}
