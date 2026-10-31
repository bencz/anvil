/*
 * ANVIL - ARM64 Darwin ABI: Apple's AAPCS64 variant with Mach-O.
 *
 * Differences from Linux (Apple, "Writing ARM64 code for Apple
 * platforms"): named stack arguments are packed at their natural size and
 * alignment, 8/16-bit integer arguments are extended to 32 bits by the
 * caller, 16-byte aligned composites need no even register pair, every
 * unnamed argument goes in a doubleword stack slot, and va_list is a plain
 * pointer into those slots.
 */

#include "../arm64_internal.h"
#include "../../common/obj_format.h"
#include "../../common/va_cursor.h"

#include <stdio.h>

/* ------------------------------------------------------------------
 * Argument passing.
 * ------------------------------------------------------------------ */

static arm64_arg_loc_t assign_scalar(const arm64_abi_desc_t *desc, arm64_arg_state_t *state,
                                     bool fp, unsigned size, bool unnamed)
{
    arm64_arg_loc_t loc = {-1, 0};
    if (unnamed) {
        state->stack = (state->stack + 7) & ~INT64_C(7);
        loc.offset = state->stack;
        state->stack += 8;
        return loc;
    }

    loc.reg = fp ? arm64_aapcs64_take_regs(&state->fpr, desc->num_fpr_args, 1)
                 : arm64_aapcs64_take_regs(&state->gpr, desc->num_gpr_args, 1);
    if (loc.reg < 0) {
        int64_t natural = size ? (int64_t)size : 8;
        state->stack = (state->stack + natural - 1) & ~(natural - 1);
        loc.offset = state->stack;
        state->stack += natural;
    }
    return loc;
}

/* Unnamed aggregates, like every unnamed argument, go to the stack. */
static arm64_arg_loc_t assign_aggregate(const arm64_abi_desc_t *desc, arm64_arg_state_t *state,
                                        const arm64_agg_plan_t *plan, bool unnamed)
{
    arm64_arg_loc_t loc = {-1, 0};
    if (unnamed) {
        loc.offset = arm64_aapcs64_aggregate_stack(state, plan);
        return loc;
    }
    loc.reg = plan->fp ? arm64_aapcs64_take_regs(&state->fpr, desc->num_fpr_args, plan->num_parts)
                       : arm64_aapcs64_take_regs(&state->gpr, desc->num_gpr_args, plan->num_parts);
    if (loc.reg < 0)
        loc.offset = arm64_aapcs64_aggregate_stack(state, plan);
    return loc;
}

/* ------------------------------------------------------------------
 * Variadic functions: va_start is the address of the first unnamed
 * doubleword slot, after the named stack arguments.
 * ------------------------------------------------------------------ */

static bool add_va_start(anvil_mir_func_t *mir, anvil_mir_vreg_t def, const arm64_arg_state_t *named)
{
    int64_t first_unnamed = (named->stack + 7) & ~INT64_C(7);
    return anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_VA_START, def, first_unnamed);
}

static void emit_va_start(anvil_strbuf_t *code, const char *dst, int frame_offset, const anvil_mir_instr_info_t *instr)
{
    (void)frame_offset;
    arm64_emit_mov_imm(code, "x16", instr->imm + 16);
    anvil_strbuf_appendf(code, "\tadd %s, x29, x16\n", dst);
}

/* Doubleword slots; composites over 16 bytes travel as a pointer to a
 * copy. */
static bool darwin_aggregate_by_value(size_t size)
{
    return size <= 16;
}

static anvil_value_t *build_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name)
{
    static const anvil_va_cursor_rules_t rules = {8, false, darwin_aggregate_by_value};
    return anvil_va_cursor_arg(be->ctx, &rules, storage, type, "Darwin ARM64", name);
}

static anvil_value_t *build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name)
{
    return anvil_va_cursor_copy(be->ctx, cursor, "Darwin ARM64", name);
}

static bool build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor)
{
    return anvil_va_cursor_copy_into(be->ctx, destination, cursor, "Darwin ARM64");
}

/* ------------------------------------------------------------------
 * Mach-O spelling.
 * ------------------------------------------------------------------ */

static void emit_function_header(anvil_strbuf_t *code, const char *symbol, anvil_linkage_t linkage)
{
    anvil_strbuf_append(code, "\t.section __TEXT,__text,regular,pure_instructions\n");
    anvil_obj_emit_binding(code, &anvil_obj_format_macho, linkage, symbol);
    anvil_strbuf_append(code, "\t.p2align 2\n");
    anvil_strbuf_appendf(code, "%s:\n", symbol);
}

static void emit_function_footer(anvil_strbuf_t *code, const char *symbol)
{
    (void)code;
    (void)symbol;
}

static void emit_symbol_address(anvil_strbuf_t *code, const char *dst, const char *symbol, int64_t addend)
{
    if (addend) {
        anvil_strbuf_appendf(code, "\tadrp %s, %s%+lld@PAGE\n", dst, symbol, (long long)addend);
        anvil_strbuf_appendf(code, "\tadd %s, %s, %s%+lld@PAGEOFF\n", dst, dst, symbol, (long long)addend);
    } else {
        anvil_strbuf_appendf(code, "\tadrp %s, %s@PAGE\n", dst, symbol);
        anvil_strbuf_appendf(code, "\tadd %s, %s, %s@PAGEOFF\n", dst, dst, symbol);
    }
}

static void emit_symbol_got(anvil_strbuf_t *code, const char *dst, const char *symbol, int64_t addend)
{
    anvil_strbuf_appendf(code, "\tadrp %s, %s@GOTPAGE\n\tldr %s, [%s, %s@GOTPAGEOFF]\n", dst, symbol, dst, dst, symbol);
    if (addend)
        arm64_emit_add_imm(code, dst, addend);
}

const arm64_abi_desc_t arm64_darwin_abi = {
    .abi = ANVIL_ABI_DARWIN,
    .name = "darwin",
    .num_gpr_args = 8,
    .num_fpr_args = 8,
    .indirect_result_reg = ARM64_X8,
    .indirect_call_reg = ARM64_X16,
    .caller_extends_small_ints = true,
    .classify_aggregate = arm64_aapcs64_classify_aggregate,
    .assign_scalar = assign_scalar,
    .assign_aggregate = assign_aggregate,
    .add_va_start = add_va_start,
    .emit_va_register_save = NULL,
    .emit_va_start = emit_va_start,
    .build_va_arg = build_va_arg,
    .build_va_copy = build_va_copy,
    .build_va_copy_into = build_va_copy_into,
    .symbol_prefix = "_",
    .module_header = "// Generated by ANVIL for ARM64 (AArch64) - macOS\n\t.build_version macos, 11, 0\n"
                     "\t.section __TEXT,__text,regular,pure_instructions\n\n",
    .cstring_section = "\t.section __TEXT,__cstring,cstring_literals",
    .emit_function_header = emit_function_header,
    .emit_function_footer = emit_function_footer,
    .emit_symbol_address = emit_symbol_address,
    .emit_symbol_got = emit_symbol_got,
    .reloc_models = (1u << ANVIL_RELOC_PIE) | (1u << ANVIL_RELOC_PIC),
    .default_reloc = ANVIL_RELOC_PIE,
    .format = &anvil_obj_format_macho,
    .result_extend_bits = 32,
};
