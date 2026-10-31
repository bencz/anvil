/* x86 memory operands, common to the i386 and x86-64 backends. */
#ifndef ANVIL_X86_ADDRESS_H
#define ANVIL_X86_ADDRESS_H

#include <stdbool.h>
#include <stdint.h>

/* base + index * {1,2,4,8} + disp32 for every access width (an
 * anvil_mir_address_mode_fn). */
static inline bool x86_address_mode(void *user, unsigned scale, int64_t disp, uint16_t access_bits)
{
    (void)user;
    (void)access_bits;
    return (scale == 1 || scale == 2 || scale == 4 || scale == 8) && disp >= INT32_MIN && disp <= INT32_MAX;
}

#endif
