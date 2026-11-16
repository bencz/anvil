/*
 * ANVIL - x86 (32-bit) Backend Helper Functions
 *
 * Register name tables and type size/alignment helpers.
 */

#include "x86_32_internal.h"

const char *x86_reg32_names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};

const char *x86_reg16_names[8] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};

const char *x86_reg8_names[8] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"};

const char *x86_xmm_names[8] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};

bool x86_reg_has_byte(int phys_reg)
{
    return phys_reg == X86_EAX || phys_reg == X86_ECX || phys_reg == X86_EDX || phys_reg == X86_EBX;
}

const char *x86_byte_reg_name(int phys_reg)
{
    if (!x86_reg_has_byte(phys_reg))
        return NULL;
    return x86_reg8_names[phys_reg];
}

bool x86_type_is_float(anvil_type_t *type)
{
    if (!type)
        return false;
    return type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64;
}
