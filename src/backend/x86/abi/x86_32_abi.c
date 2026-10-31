#include "../x86_32_internal.h"
#include "../../common/abi_layout.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int x86_fastcall_int_args[] = {X86_ECX, X86_EDX};
/* Caller-saved registers first: a value that crosses no call takes one
 * before the callee-saved EBX/ESI/EDI, which cost a save and a restore.
 * The spill reserve is the pool's caller-saved part; XMM4 stays out of
 * both, the FP units' own temporary.  (On the x87 unit an FPR number
 * names a home slot in the frame.) */
static const int x86_alloc_gpr[] = {X86_ECX, X86_EDX, X86_EAX, X86_EBX, X86_ESI, X86_EDI};
static const int x86_scratch_gpr[] = {X86_EAX, X86_ECX, X86_EDX};
static const int x86_alloc_fpr[] = {0, 1, 2, 3, 5, 6, 7};
static const int x86_scratch_fpr[] = {5, 6, 7};
/* Every i386 C convention: EAX, ECX, EDX and all XMM registers (x87 home
 * slots survive, but are treated alike). */
#define X86_CALL_GPR_CLOBBERS ((UINT64_C(1) << X86_EAX) | (UINT64_C(1) << X86_ECX) | (UINT64_C(1) << X86_EDX))
#define X86_CALL_FPR_CLOBBERS UINT64_C(0xff)

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
        .alloc_fpr_regs = x86_alloc_fpr,
        .num_alloc_fpr_regs = (int)(sizeof(x86_alloc_fpr) / sizeof(int)),
        .scratch_gpr_regs = x86_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x86_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x86_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x86_scratch_fpr) / sizeof(int)),
        .call_gpr_clobbers = X86_CALL_GPR_CLOBBERS,
        .call_fpr_clobbers = X86_CALL_FPR_CLOBBERS,
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
        .alloc_fpr_regs = x86_alloc_fpr,
        .num_alloc_fpr_regs = (int)(sizeof(x86_alloc_fpr) / sizeof(int)),
        .scratch_gpr_regs = x86_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x86_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x86_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x86_scratch_fpr) / sizeof(int)),
        .call_gpr_clobbers = X86_CALL_GPR_CLOBBERS,
        .call_fpr_clobbers = X86_CALL_FPR_CLOBBERS,
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
        .alloc_fpr_regs = x86_alloc_fpr,
        .num_alloc_fpr_regs = (int)(sizeof(x86_alloc_fpr) / sizeof(int)),
        .scratch_gpr_regs = x86_scratch_gpr,
        .num_scratch_gpr_regs = (int)(sizeof(x86_scratch_gpr) / sizeof(int)),
        .scratch_fpr_regs = x86_scratch_fpr,
        .num_scratch_fpr_regs = (int)(sizeof(x86_scratch_fpr) / sizeof(int)),
        .call_gpr_clobbers = X86_CALL_GPR_CLOBBERS,
        .call_fpr_clobbers = X86_CALL_FPR_CLOBBERS,
    },
};

/* Aggregate results as clang and GCC implement them: the i386 SysV psABI
 * returns every aggregate in memory; Darwin and MinGW return small ones in
 * registers.  (MSVC would return float/double members in EAX:EDX.) */
static const anvil_x86_plat_desc_t x86_plat_descs[] = {
    {.platform = X86_PLAT_ELF, .sym_prefix = "", .is_macho = false, .is_coff = false,
     .small_aggregates_in_regs = false, .float_aggregates_in_x87 = false, .callee_pops_sret = true, .stack_alignment = 16, .format = &anvil_obj_format_elf},
    {.platform = X86_PLAT_MACHO, .sym_prefix = "_", .is_macho = true, .is_coff = false,
     .small_aggregates_in_regs = true, .float_aggregates_in_x87 = true, .callee_pops_sret = true, .stack_alignment = 16, .format = &anvil_obj_format_macho},
    {.platform = X86_PLAT_COFF, .sym_prefix = "_", .is_macho = false, .is_coff = true,
     .small_aggregates_in_regs = true, .float_aggregates_in_x87 = true, .callee_pops_sret = false, .stack_alignment = 16, .format = &anvil_obj_format_coff},
};

const anvil_agg_target_t x86_agg_target = {4, false};

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

bool x86_classify_aggregate(const anvil_x86_plat_desc_t *plat, anvil_type_t *type, bool is_return,
                            x86_agg_plan_t *plan)
{
    memset(plan, 0, sizeof(*plan));
    if (!plat || !type || (type->kind != ANVIL_TYPE_STRUCT && type->kind != ANVIL_TYPE_ARRAY) || !type->size)
        return false;
    plan->size = type->size;
    plan->align = type->align ? type->align : 1;
    if (!is_return) {
        plan->placement = X86_AGG_STACK;
        return true;
    }

    plan->placement = X86_AGG_INDIRECT;
    if (!plat->small_aggregates_in_regs)
        return true;

    size_t count = 0;
    anvil_type_t *base = anvil_abi_homogeneous_float(type, &count);
    if (plat->float_aggregates_in_x87 && base && count == 1) {
        plan->placement = X86_AGG_REGS;
        plan->parts[0] = (anvil_agg_part_t){true, (unsigned)base->size, 0, 0};
        plan->num_parts = 1;
        return true;
    }
    if (type->size == 1 || type->size == 2 || type->size == 4 || type->size == 8) {
        plan->placement = X86_AGG_REGS;
        plan->num_parts = type->size > 4 ? 2 : 1;
        for (unsigned i = 0; i < plan->num_parts; i++)
            plan->parts[i] = anvil_agg_chunk(type->size, 4 * i, x86_agg_target);
    }
    return true;
}

int64_t x86_callee_pop_bytes(const anvil_x86_cc_desc_t *desc, const anvil_x86_plat_desc_t *plat,
                             int64_t stack_arg_bytes, bool hidden_result_pointer)
{
    if (desc->callee_cleans_stack)
        return stack_arg_bytes;
    return hidden_result_pointer && plat->callee_pops_sret ? 4 : 0;
}

/* COFF stdcall/fastcall names carry the bytes of the declared arguments:
 * stack copies of by-value aggregates at their rounded size, the hidden
 * result pointer as one word, and no bytes for a result returned in
 * registers. Callers and callees both derive it from the function type. */
static int64_t x86_decorated_argument_bytes(const anvil_x86_plat_desc_t *plat, anvil_type_t *fn_type)
{
    int64_t bytes = 0;
    for (size_t i = 0; i < fn_type->data.func.num_params; i++) {
        anvil_type_t *param = fn_type->data.func.params[i];
        anvil_param_attr_t attr = anvil_type_func_param_attr(fn_type, i);
        if (attr != ANVIL_PARAM_PLAIN && param && param->kind == ANVIL_TYPE_PTR) {
            x86_agg_plan_t plan;
            if (!x86_classify_aggregate(plat, param->data.pointee, attr == ANVIL_PARAM_SRET, &plan))
                return -1;
            if (plan.placement == X86_AGG_REGS)
                continue;
            if (plan.placement == X86_AGG_STACK) {
                bytes += (int64_t)((plan.size + 3) & ~(size_t)3);
                continue;
            }
        }
        bytes += x86_stack_arg_slot_size(param);
    }
    return bytes;
}

bool x86_decorated_symbol(const anvil_x86_plat_desc_t *plat, const anvil_x86_cc_desc_t *desc, anvil_type_t *fn_type,
                          const char *name, char *out, size_t out_size)
{
    if (!plat->is_coff || desc->decor == X86_DECOR_NONE || !fn_type || fn_type->kind != ANVIL_TYPE_FUNC)
        return false;

    int64_t bytes = x86_decorated_argument_bytes(plat, fn_type);
    if (bytes < 0)
        return false;

    int written = desc->decor == X86_DECOR_STDCALL ? snprintf(out, out_size, "_%s@%lld", name, (long long)bytes)
                                                   : snprintf(out, out_size, "@%s@%lld", name, (long long)bytes);
    return written > 0 && (size_t)written < out_size;
}
