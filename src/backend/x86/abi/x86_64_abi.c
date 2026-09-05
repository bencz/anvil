#include "../x86_64_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int x64_sysv_int_args[] = {7, 6, 2, 1, 8, 9};
static const int x64_sysv_fp_args[] = {0, 1, 2, 3, 4, 5, 6, 7};
static const int x64_sysv_alloc_gpr[] = {8, 9, 6, 7, 3, 12, 13, 14};
static const int x64_sysv_alloc_fpr[] = {0, 1, 2, 3, 4, 5, 6, 7, 11, 12, 13, 14};
static const int x64_sysv_scratch_gpr[] = {10, 11, 15, X64_RAX};
static const int x64_sysv_scratch_fpr[] = {8, 9, 10};

static const int x64_win64_int_args[] = {1, 2, 8, 9};
static const int x64_win64_fp_args[] = {0, 1, 2, 3};
static const int x64_win64_alloc_gpr[] = {8, 9, 3, 7, 6, 12, 13, 14};
static const int x64_win64_alloc_fpr[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12};
static const int x64_win64_scratch_gpr[] = {10, 11, 15, X64_RAX};
static const int x64_win64_scratch_fpr[] = {13, 14, 15};

static const anvil_x64_abi_desc_t x64_abi_descs[] = {
    {
        .abi = ANVIL_ABI_SYSV,
        .name = "sysv",
        .sym_prefix = "",
        .positional_args = false,
        .is_darwin = false,
        .is_win64 = false,
        .shadow_space = 0,
        .int_arg_regs = x64_sysv_int_args,
        .num_int_arg_regs = (int)(sizeof(x64_sysv_int_args) / sizeof(int)),
        .fp_arg_regs = x64_sysv_fp_args,
        .num_fp_arg_regs = (int)(sizeof(x64_sysv_fp_args) / sizeof(int)),
        .int_ret_reg = X64_RAX,
        .int_ret_hi_reg = X64_RDX,
        .fp_ret_reg = 0,
        .indirect_call_reg = X64_R11,
        .call_gpr_clobbers = UINT64_C(0x0fc7),
        .call_fpr_clobbers = UINT64_C(0xffff),
        .alloc_gpr_regs = x64_sysv_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x64_sysv_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = x64_sysv_alloc_fpr,
        .num_alloc_fpr_regs = (int)(sizeof(x64_sysv_alloc_fpr) / sizeof(int)),
        .scratch_gpr_regs = x64_sysv_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x64_sysv_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x64_sysv_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x64_sysv_scratch_fpr) / sizeof(int)),
    },
    {
        .abi = ANVIL_ABI_DARWIN,
        .name = "darwin",
        .sym_prefix = "_",
        .positional_args = false,
        .is_darwin = true,
        .is_win64 = false,
        .shadow_space = 0,
        .int_arg_regs = x64_sysv_int_args,
        .num_int_arg_regs = (int)(sizeof(x64_sysv_int_args) / sizeof(int)),
        .fp_arg_regs = x64_sysv_fp_args,
        .num_fp_arg_regs = (int)(sizeof(x64_sysv_fp_args) / sizeof(int)),
        .int_ret_reg = X64_RAX,
        .int_ret_hi_reg = X64_RDX,
        .fp_ret_reg = 0,
        .indirect_call_reg = X64_R11,
        .call_gpr_clobbers = UINT64_C(0x0fc7),
        .call_fpr_clobbers = UINT64_C(0xffff),
        .alloc_gpr_regs = x64_sysv_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x64_sysv_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = x64_sysv_alloc_fpr,
        .num_alloc_fpr_regs = (int)(sizeof(x64_sysv_alloc_fpr) / sizeof(int)),
        .scratch_gpr_regs = x64_sysv_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x64_sysv_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x64_sysv_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x64_sysv_scratch_fpr) / sizeof(int)),
    },
    {
        .abi = ANVIL_ABI_WIN64,
        .name = "win64",
        .sym_prefix = "",
        .positional_args = true,
        .is_darwin = false,
        .is_win64 = true,
        .shadow_space = 32,
        .int_arg_regs = x64_win64_int_args,
        .num_int_arg_regs = (int)(sizeof(x64_win64_int_args) / sizeof(int)),
        .fp_arg_regs = x64_win64_fp_args,
        .num_fp_arg_regs = (int)(sizeof(x64_win64_fp_args) / sizeof(int)),
        .int_ret_reg = X64_RAX,
        .int_ret_hi_reg = -1,
        .fp_ret_reg = 0,
        .indirect_call_reg = X64_R11,
        .call_gpr_clobbers = UINT64_C(0x0f07),
        .call_fpr_clobbers = UINT64_C(0x003f),
        .alloc_gpr_regs = x64_win64_alloc_gpr,
        .num_alloc_gpr_regs = (int)(sizeof(x64_win64_alloc_gpr) / sizeof(int)),
        .alloc_fpr_regs = x64_win64_alloc_fpr,
        .num_alloc_fpr_regs = (int)(sizeof(x64_win64_alloc_fpr) / sizeof(int)),
        .scratch_gpr_regs = x64_win64_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x64_win64_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x64_win64_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x64_win64_scratch_fpr) / sizeof(int)),
    },
};

const anvil_x64_abi_desc_t *anvil_x64_get_abi_desc(anvil_abi_t abi)
{
    if (abi == ANVIL_ABI_DEFAULT)
        abi = ANVIL_ABI_SYSV;
    for (size_t i = 0; i < sizeof(x64_abi_descs) / sizeof(x64_abi_descs[0]); i++) {
        if (x64_abi_descs[i].abi == abi)
            return &x64_abi_descs[i];
    }
    return NULL;
}

anvil_error_t x64_classify_abi_value(anvil_backend_t *be, anvil_type_t *type, bool is_return, anvil_abi_value_plan_t *plan)
{
    (void)is_return;
    if (be->ctx->abi != ANVIL_ABI_WIN64 || type->kind != ANVIL_TYPE_STRUCT || !anvil_sem_type_is_sized(type))
        return ANVIL_ERR_INVALID_TYPE;

    size_t size = anvil_type_size(type);
    if (!size)
        return ANVIL_ERR_INVALID_TYPE;

    if (size == 1 || size == 2 || size == 4 || size == 8) {
        plan->kind = ANVIL_ABI_VALUE_INTEGER;
        switch (size) {
        case 1:
            plan->transport_type = anvil_type_u8(be->ctx);
            break;
        case 2:
            plan->transport_type = anvil_type_u16(be->ctx);
            break;
        case 4:
            plan->transport_type = anvil_type_u32(be->ctx);
            break;
        case 8:
            plan->transport_type = anvil_type_u64(be->ctx);
            break;
        }
        plan->temporary_alignment = anvil_type_align(type);
    } else {
        plan->kind = ANVIL_ABI_VALUE_INDIRECT;
        plan->transport_type = anvil_type_ptr(be->ctx, type);
        plan->temporary_alignment = 16;
    }

    return plan->transport_type ? ANVIL_OK : ANVIL_ERR_NOMEM;
}
