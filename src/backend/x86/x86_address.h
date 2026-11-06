/* x86 memory operands, common to the i386 and x86-64 backends. */
#ifndef ANVIL_X86_ADDRESS_H
#define ANVIL_X86_ADDRESS_H

#include "anvil/anvil.h"
#include "anvil/anvil_machine.h"

#include <stdbool.h>
#include <stdint.h>

/* base + index * {1,2,4,8} + disp32 for every access width, the index as
 * wide as the address (an anvil_mir_address_mode_fn). */
static inline bool x86_address_mode(void *user, unsigned scale, int64_t disp, uint16_t access_bits, anvil_mir_index_extend_t extend)
{
    (void)user;
    (void)access_bits;
    return extend == ANVIL_MIR_INDEX_PLAIN && (scale == 1 || scale == 2 || scale == 4 || scale == 8) && disp >= INT32_MIN && disp <= INT32_MAX;
}

/* anvil_backend_ops_t.indexed_displacement: any disp32, any width. */
static inline bool x86_indexed_displacement(anvil_backend_t *be, int64_t displacement, size_t access_bytes)
{
    (void)be;
    (void)access_bytes;
    return x86_address_mode(NULL, 1, displacement, 0, ANVIL_MIR_INDEX_PLAIN);
}

#endif
