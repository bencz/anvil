/*
 * ANVIL - x86 (32-bit) Backend Helper Functions
 *
 * Register name tables and type size/alignment helpers.
 */

#include "x86_32_internal.h"

bool x86_reg_has_byte(int phys_reg)
{
    return phys_reg == X86_EAX || phys_reg == X86_ECX || phys_reg == X86_EDX || phys_reg == X86_EBX;
}

bool x86_type_is_float(anvil_type_t *type)
{
    if (!type)
        return false;
    return type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64;
}
