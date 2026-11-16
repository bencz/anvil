/*
 * ANVIL - x86-64 Backend Helper Functions
 *
 * Register name tables and type size/alignment helpers.
 */

#include "x86_64_internal.h"

const char *x64_reg64_names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};

const char *x64_reg32_names[16] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};

const char *x64_reg16_names[16] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};

const char *x64_reg8_names[16] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};

const char *x64_xmm_names[16] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};

bool x64_type_is_float(anvil_type_t *type)
{
    if (!type)
        return false;
    return type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64;
}
