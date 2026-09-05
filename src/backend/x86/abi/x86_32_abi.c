#include "../x86_32_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int x86_fastcall_int_args[] = {X86_ECX, X86_EDX};
static const int x86_alloc_gpr[] = {X86_EBX, X86_ESI, X86_EDI};
static const int x86_scratch_gpr[] = {X86_EAX, X86_ECX, X86_EDX};
static const int x86_scratch_fpr[] = {5, 6, 7};

static const anvil_x86_cc_desc_t x86_cc_descs[] = {
    {
        .cc = ANVIL_CC_CDECL,
        .name = "cdecl",
        .callee_cleans_stack = false,
        .num_reg_int_args = 0,
        .reg_int_args = NULL,
        .decor = X86_DECOR_NONE,
        .int_ret_reg = X86_EAX,
        .int_ret_hi_reg = X86_EDX,
        .alloc_gpr_regs = x86_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x86_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = NULL,
        .num_alloc_fpr_regs = 0,
        .scratch_gpr_regs = x86_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x86_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x86_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x86_scratch_fpr) / sizeof(int)),
    },
    {
        .cc = ANVIL_CC_STDCALL,
        .name = "stdcall",
        .callee_cleans_stack = true,
        .num_reg_int_args = 0,
        .reg_int_args = NULL,
        .decor = X86_DECOR_STDCALL,
        .int_ret_reg = X86_EAX,
        .int_ret_hi_reg = X86_EDX,
        .alloc_gpr_regs = x86_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x86_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = NULL,
        .num_alloc_fpr_regs = 0,
        .scratch_gpr_regs = x86_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x86_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x86_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x86_scratch_fpr) / sizeof(int)),
    },
    {
        .cc = ANVIL_CC_FASTCALL,
        .name = "fastcall",
        .callee_cleans_stack = true,
        .num_reg_int_args = 2,
        .reg_int_args = x86_fastcall_int_args,
        .decor = X86_DECOR_FASTCALL,
        .int_ret_reg = X86_EAX,
        .int_ret_hi_reg = X86_EDX,
        .alloc_gpr_regs = x86_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x86_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = NULL,
        .num_alloc_fpr_regs = 0,
        .scratch_gpr_regs = x86_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x86_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x86_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x86_scratch_fpr) / sizeof(int)),
    },
};

static const anvil_x86_plat_desc_t x86_plat_descs[] = {
    {.platform = X86_PLAT_ELF, .sym_prefix = "", .is_macho = false, .is_coff = false},
    {.platform = X86_PLAT_MACHO, .sym_prefix = "_", .is_macho = true, .is_coff = false},
    {.platform = X86_PLAT_COFF, .sym_prefix = "_", .is_macho = false, .is_coff = true},
};

const anvil_x86_cc_desc_t *anvil_x86_get_cc_desc(anvil_cc_t cc)
{
    for (size_t i = 0; i < sizeof(x86_cc_descs) / sizeof(x86_cc_descs[0]); i++) {
        if (x86_cc_descs[i].cc == cc)
            return &x86_cc_descs[i];
    }
    return NULL;
}

const anvil_x86_plat_desc_t *anvil_x86_get_plat_desc(anvil_abi_t abi)
{
    if (abi == ANVIL_ABI_DARWIN)
        return &x86_plat_descs[1];
    if (abi == ANVIL_ABI_WIN64)
        return &x86_plat_descs[2];
    return &x86_plat_descs[0];
}

static bool x86_type_is_i64(anvil_type_t *type)
{
    return type && (type->kind == ANVIL_TYPE_I64 || type->kind == ANVIL_TYPE_U64);
}

static bool x86_type_is_small_aggregate_pair(anvil_type_t *type)
{
    if (!type)
        return false;
    if (type->kind != ANVIL_TYPE_STRUCT && type->kind != ANVIL_TYPE_ARRAY) {
        return false;
    }
    size_t size = anvil_type_size(type);
    return size > 4 && size <= 8;
}

bool x86_needs_pair(anvil_type_t *type)
{
    return x86_type_is_i64(type) || x86_type_is_small_aggregate_pair(type);
}

int64_t x86_stack_arg_slot_size(anvil_type_t *type)
{
    int64_t size = type ? x86_type_size(type) : 4;
    if (size <= 0)
        size = 4;
    return (size + 3) & ~INT64_C(3);
}
