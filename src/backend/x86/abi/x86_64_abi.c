#include "../x86_64_internal.h"
#include "../../common/abi_layout.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int x64_sysv_int_args[] = {7, 6, 2, 1, 8, 9};
static const int x64_sysv_fp_args[] = {0, 1, 2, 3, 4, 5, 6, 7};
/* Pools list volatile registers first, those the printer takes as
 * per-instruction scratch (RCX, RDX:RAX) after the others; spill reloads
 * use the scratch lists, part of the pools (anvil_regalloc_allocate). */
static const int x64_sysv_alloc_gpr[] = {8, 9, 6, 7, 10, 11, 2, 1, X64_RAX, 3, 12, 13, 14, 15};
static const int x64_sysv_alloc_fpr[] = {0, 1, 2, 3, 4, 5, 6, 7, 11, 12, 13, 8, 9, 10};
static const int x64_sysv_scratch_gpr[] = {11, 10, X64_RAX};
static const int x64_sysv_scratch_fpr[] = {8, 9, 10};

static const int x64_win64_int_args[] = {1, 2, 8, 9};
static const int x64_win64_fp_args[] = {0, 1, 2, 3};
static const int x64_win64_alloc_gpr[] = {8, 9, 10, 11, 2, 1, X64_RAX, 3, 7, 6, 12, 13, 14, 15};
static const int x64_win64_alloc_fpr[] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const int x64_win64_scratch_gpr[] = {11, 10, X64_RAX};
static const int x64_win64_scratch_fpr[] = {13, 14, 15};

static const anvil_x64_abi_desc_t x64_abi_descs[] = {
    {
        .abi = ANVIL_ABI_SYSV,
        .name = "sysv",
        .sym_prefix = "",
        .positional_args = false,
        .is_darwin = false,
        .is_win64 = false,
        .reloc_models = (1u << ANVIL_RELOC_STATIC) | (1u << ANVIL_RELOC_PIE) | (1u << ANVIL_RELOC_PIC),
        .default_reloc = ANVIL_RELOC_STATIC,
        .plt_suffix = "@PLT",
        .format = &anvil_obj_format_elf,
        .caller_extends_narrow_args = true,
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
        .sse_temps = {15, 14},
    },
    {
        .abi = ANVIL_ABI_DARWIN,
        .name = "darwin",
        .sym_prefix = "_",
        .positional_args = false,
        .is_darwin = true,
        .is_win64 = false,
        .reloc_models = (1u << ANVIL_RELOC_PIE) | (1u << ANVIL_RELOC_PIC),
        .default_reloc = ANVIL_RELOC_PIE,
        .plt_suffix = "",
        .format = &anvil_obj_format_macho,
        .caller_extends_narrow_args = true,
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
        .sse_temps = {15, 14},
    },
    {
        .abi = ANVIL_ABI_WIN64,
        .name = "win64",
        .sym_prefix = "",
        .positional_args = true,
        .is_darwin = false,
        .is_win64 = true,
        .reloc_models = 1u << ANVIL_RELOC_STATIC,
        .default_reloc = ANVIL_RELOC_STATIC,
        .plt_suffix = NULL,
        .format = &anvil_obj_format_coff,
        .caller_extends_narrow_args = true,
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
        .sse_temps = {5, 4},
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

/* SysV eightbyte classes; INTEGER dominates SSE when they share one. */
enum { X64_CLASS_NONE, X64_CLASS_SSE, X64_CLASS_INTEGER };

typedef struct {
    int classes[2];
} x64_class_state_t;

static bool x64_class_leaf(void *user, anvil_type_t *leaf, size_t offset)
{
    x64_class_state_t *state = user;
    size_t eightbyte = offset / 8;
    if (eightbyte > 1)
        return false;
    int cls = leaf->kind == ANVIL_TYPE_F32 || leaf->kind == ANVIL_TYPE_F64 ? X64_CLASS_SSE : X64_CLASS_INTEGER;
    if (cls > state->classes[eightbyte])
        state->classes[eightbyte] = cls;
    return true;
}

bool x64_classify_aggregate(const anvil_x64_abi_desc_t *desc, anvil_type_t *type, bool is_return, x64_agg_plan_t *plan)
{
    memset(plan, 0, sizeof(*plan));
    if (!desc || !type || (type->kind != ANVIL_TYPE_STRUCT && type->kind != ANVIL_TYPE_ARRAY) || !type->size)
        return false;
    plan->size = type->size;
    plan->align = type->align ? type->align : 1;

    if (desc->is_win64) {
        /* Objects of 1, 2, 4 or 8 bytes travel as an integer of that size;
         * everything else by reference (arguments) or hidden pointer. */
        if (type->size == 1 || type->size == 2 || type->size == 4 || type->size == 8) {
            plan->placement = X64_AGG_REGS;
            plan->parts[0] = (x64_agg_part_t){false, (unsigned)type->size, 0, 0};
            plan->num_parts = 1;
            plan->num_gpr = 1;
        } else {
            plan->placement = is_return ? X64_AGG_HIDDEN_POINTER : X64_AGG_REFERENCE;
        }
        return true;
    }

    /* SysV / Darwin: MEMORY for objects over two eightbytes or with
     * unaligned fields; otherwise one class per eightbyte. */
    x64_class_state_t state = {{X64_CLASS_NONE, X64_CLASS_NONE}};
    if (type->size > 16 || anvil_abi_has_unaligned_leaf(type) || !anvil_abi_visit_leaves(type, 0, x64_class_leaf, &state)) {
        plan->placement = is_return ? X64_AGG_HIDDEN_POINTER : X64_AGG_STACK;
        return true;
    }
    plan->placement = X64_AGG_REGS;
    plan->num_parts = type->size > 8 ? 2 : 1;
    for (unsigned i = 0; i < plan->num_parts; i++) {
        bool fp = state.classes[i] == X64_CLASS_SSE;
        size_t remaining = type->size - 8 * i;
        plan->parts[i] = (x64_agg_part_t){fp, (unsigned)(remaining < 8 ? remaining : 8), 8 * i, 0};
        if (fp)
            plan->num_fpr++;
        else
            plan->num_gpr++;
    }
    return true;
}

x64_arg_loc_t x64_assign_scalar(const anvil_x64_abi_desc_t *desc, x64_arg_state_t *state, bool fp)
{
    x64_arg_loc_t loc = {-1, -1, 0};
    size_t index = desc->positional_args ? state->position : fp ? state->fpr : state->gpr;
    if (fp && (int)index < desc->num_fp_arg_regs)
        loc.reg = desc->fp_arg_regs[index];
    else if (!fp && (int)index < desc->num_int_arg_regs)
        loc.reg = desc->int_arg_regs[index];
    if (fp && desc->positional_args && (int)index < desc->num_int_arg_regs)
        loc.shadow_reg = desc->int_arg_regs[index];

    if (fp)
        state->fpr++;
    else
        state->gpr++;
    state->position++;
    if (loc.reg < 0) {
        loc.offset = state->stack;
        state->stack += 8;
    }
    return loc;
}

x64_arg_loc_t x64_assign_aggregate(const anvil_x64_abi_desc_t *desc, x64_arg_state_t *state,
                                   const x64_agg_plan_t *plan, int regs[2])
{
    x64_arg_loc_t loc = {-1, -1, 0};
    bool fits = plan->placement == X64_AGG_REGS &&
        (desc->positional_args ? state->position + plan->num_parts <= (size_t)desc->num_int_arg_regs
                               : state->gpr + plan->num_gpr <= (size_t)desc->num_int_arg_regs &&
                                 state->fpr + plan->num_fpr <= (size_t)desc->num_fp_arg_regs);
    if (fits) {
        /* SysV takes all eightbytes in registers or none of them. */
        for (unsigned part = 0; part < plan->num_parts; part++) {
            bool fp = plan->parts[part].fp;
            size_t index = desc->positional_args ? state->position : fp ? state->fpr : state->gpr;
            regs[part] = fp ? desc->fp_arg_regs[index] : desc->int_arg_regs[index];
            if (fp)
                state->fpr++;
            else
                state->gpr++;
            state->position++;
        }
        loc.reg = regs[0];
        return loc;
    }

    /* Memory image in eightbytes, 16-aligned for 16-byte aligned types;
     * later arguments may still use registers. */
    if (plan->align >= 16)
        state->stack = (state->stack + 15) & ~INT64_C(15);
    loc.offset = state->stack;
    state->stack += (int64_t)((plan->size + 7) & ~(size_t)7);
    if (desc->positional_args)
        state->position++;
    return loc;
}
