#include "gas32_internal.h"
#include "../x86_address.h"
#include "../../common/parallel_move.h"
#include "x86_gas.h"
#include "../../common/gnu_data.h"
#include "x86_sse_vector.h"
#include "anvil/anvil_analysis.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Position-dependent code addresses a symbol absolutely: the base of a
 * load or store becomes the displacement (x86_memory_operand). */
static bool x86_use_takes_symbol(void *ctx, size_t index, const anvil_mir_instr_info_t *info, size_t use, const char *symbol, int64_t offset)
{
    const x86_mir_emit_t *emit = ctx;
    (void)index;
    (void)symbol;
    return emit->reloc == ANVIL_RELOC_STATIC && offset >= INT32_MIN && offset <= INT32_MAX &&
           ((info->op == ANVIL_MIR_OP_LOAD && use == 0) || (info->op == ANVIL_MIR_OP_STORE && use == 1));
}

/* GPRs the emitters below write before reading all of an instruction's
 * operands: CL for a variable shift count. */
static uint64_t x86_printer_gpr_scratch(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index,
                                        const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def = info->def != ANVIL_MIR_NO_VREG ? anvil_mir_get_vreg_info(mir, info->def) : NULL;
    int64_t count;
    switch (info->op) {
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
        return def && def->reg_class == ANVIL_MIR_REG_GPR && !x86_gas_facts_imm(facts, true, anvil_mir_get_instr_use(mir, index, 1), &count)
                   ? UINT64_C(1) << X86_ECX
                   : 0;
    default:
        return 0;
    }
}

/* GPRs they overwrite only after reading the operands: EDX:EAX for
 * division and the high half of a product, EAX for a dynamic allocation. */
static uint64_t x86_printer_gpr_clobbers(const anvil_mir_instr_info_t *info)
{
    const uint64_t eax = UINT64_C(1) << X86_EAX, edx = UINT64_C(1) << X86_EDX;
    switch (info->op) {
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD:
        return eax | edx;
    case ANVIL_MIR_OP_SCALAR:
        return info->ir_op == ANVIL_OP_UMULH || info->ir_op == ANVIL_OP_SMULH ? eax | edx : 0;
    case ANVIL_MIR_OP_DYN_ALLOCA:
        return eax;
    default:
        return 0;
    }
}

/* XMM registers of the shared SSE printer: XMM4, never allocated, and
 * XMM7, the scratch that reductions and accumulations declare
 * (x86_annotate_printer_registers). */
#define X86_SSE_TEMP 4
#define X86_SSE_TEMP2 7

bool x86_annotate_printer_registers(anvil_mir_func_t *mir)
{
    anvil_mir_isel_facts_t facts;
    if (!anvil_mir_isel_facts_build(&facts, mir, NULL, NULL, NULL))
        return false;
    bool ok = true;
    for (size_t index = 0; ok && index < anvil_mir_num_instrs(mir); index++) {
        anvil_mir_instr_info_t info;
        ok = anvil_mir_get_instr_info(mir, index, &info);
        uint64_t clobbers = ok ? x86_printer_gpr_clobbers(&info) : 0;
        /* Reductions and accumulations fold through a second XMM temp. */
        bool second_temp = ok && info.op == ANVIL_MIR_OP_VECTOR && (anvil_op_is_reduction(info.ir_op) || info.widening);
        ok = ok && (!second_temp || anvil_mir_set_instr_scratch(mir, index, ANVIL_MIR_REG_FPR, UINT64_C(1) << X86_SSE_TEMP2)) &&
             anvil_mir_set_instr_scratch(mir, index, ANVIL_MIR_REG_GPR, x86_printer_gpr_scratch(mir, &facts, index, &info)) &&
             (!clobbers || anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, info.clobbers[ANVIL_MIR_REG_GPR] | clobbers));
    }
    anvil_mir_isel_facts_destroy(&facts);
    return ok;
}

static void x86_emit_parallel_move(x86_mir_emit_t *emit, const int *dst, const int *src, size_t count)
{
    static const anvil_parallel_move_ops_t ops = { x86_gas_parallel_move, x86_gas_parallel_swap, -1 };
    if (!anvil_parallel_move(&ops, emit, dst, src, count))
        emit->base.failed = true;
}

static bool x86_scan_outgoing_stack_args(x86_mir_emit_t *emit)
{
    int outgoing_size = 0;
    if (!anvil_mir_outgoing_stack_bytes(emit->base.mir, 4, &outgoing_size))
        return false;
    emit->base.outgoing_size = anvil_align_int(outgoing_size, 16);
    return true;
}

static bool x86_func_returns_fp(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info))
            return false;
        if (info.op != ANVIL_MIR_OP_RET || info.num_uses == 0)
            continue;
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(mir, i, 0);
        const anvil_mir_vreg_info_t *rinfo = anvil_mir_get_vreg_info(mir, ret);
        if (rinfo && rinfo->reg_class == ANVIL_MIR_REG_FPR)
            return true;
    }
    return false;
}

static bool x86_prepare_frame(x86_mir_emit_t *emit)
{
    for (size_t i = 0; i < 8; i++) {
        emit->gpr_save_offsets[i] = -1;
    }

    if (!x86_scan_outgoing_stack_args(emit))
        return false;
    emit->fp_returns = x86_func_returns_fp(emit->base.mir);

    int offset = 0;
    emit->frame_pointer = x86_keeps_frame_pointer(emit->base.mir);
    /* Without a frame pointer the saves are pushes, the first into EBP's
     * would-be slot (offset 0). */
    int first_save = emit->frame_pointer ? 4 : 0;
    emit->num_pushes = 0;
    static const int callee_saved[] = {X86_EBX, X86_ESI, X86_EDI, X86_EBP};
    for (size_t c = 0; c < sizeof(callee_saved) / sizeof(callee_saved[0]); c++) {
        int reg = callee_saved[c];
        if (!anvil_mir_isel_writes(emit->base.mir, &emit->base.isel, ANVIL_MIR_REG_GPR, reg))
            continue;
        emit->gpr_save_offsets[reg] = offset = first_save + 4 * emit->num_pushes;
        emit->num_pushes++;
    }

    /* Calls (and runtime helpers) need the stack aligned; floating point
     * moves values through two scratch words. */
    bool calls = false, floating = false;
    for (size_t i = 0; i < anvil_mir_num_instrs(emit->base.mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->base.mir, i, &info))
            return false;
        calls |= info.op == ANVIL_MIR_OP_CALL || info.clobbers[ANVIL_MIR_REG_GPR] != 0;
    }
    for (size_t i = 0; i < anvil_mir_num_vregs(emit->base.mir); i++) {
        const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->base.mir, (anvil_mir_vreg_t)i);
        floating |= info && info->reg_class == ANVIL_MIR_REG_FPR;
    }
    emit->scratch_a_off = emit->scratch_b_off = -1;
    if (floating || emit->fp_returns) {
        offset += 4;
        emit->scratch_a_off = offset;
        offset += 4;
        emit->scratch_b_off = offset;
    }

    int fp_bytes = emit->fp->frame_bytes(emit);
    if (fp_bytes > 0) {
        offset = anvil_align_int(offset, 8) + fp_bytes;
        emit->fp_area_off = offset;
    }

    emit->base.num_spill_offsets = anvil_mir_num_spills(emit->base.mir);
    emit->base.num_frame_slot_offsets = anvil_mir_num_frame_slots(emit->base.mir);
    if (emit->base.num_frame_slot_offsets > 0) {
        emit->base.frame_slot_offsets = calloc(emit->base.num_frame_slot_offsets, sizeof(*emit->base.frame_slot_offsets));
        if (!emit->base.frame_slot_offsets)
            return false;
    }

    /* Addressed downward from EBP. */
    if (!anvil_mir_layout_frame_slots(emit->base.mir, offset, 4, (uint32_t)emit->plat->stack_alignment, true, emit->base.frame_slot_offsets, &offset))
        return false;

    if (emit->base.num_spill_offsets > 0) {
        emit->base.spill_offsets = calloc(emit->base.num_spill_offsets, sizeof(*emit->base.spill_offsets));
        if (!emit->base.spill_offsets)
            return false;
    }

    if (!anvil_mir_layout_spill_slots(emit->base.mir, offset, 4, 4, true, emit->base.spill_offsets, &offset))
        return false;

    offset += emit->base.outgoing_size;
    /* Entry leaves ESP 4 bytes below an aligned boundary and the EBP push
     * (or its slot) another 4: the frame restores the alignment for calls.
     * A function calling nothing with no aligned locals needs no
     * alignment, and without saves or a frame no EBP slot either. */
    int align = emit->plat->stack_alignment;
    if (emit->frame_pointer || calls || emit->base.num_frame_slot_offsets > 0)
        emit->base.frame_size = anvil_align_int(offset + 8, align) - 8;
    else
        emit->base.frame_size = offset == 0 && emit->num_pushes == 0 ? -4 : offset;
    emit->base.frame_reg = emit->frame_pointer ? "ebp" : "esp";
    emit->base.frame_bias = emit->frame_pointer ? 0 : emit->base.frame_size;
    return true;
}

static void x86_emit_func_header(x86_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->base.mir);
    const char *prefix = emit->base.symbol_prefix;
    const anvil_obj_format_t *format = emit->plat->format;
    anvil_linkage_t linkage = anvil_mir_func_linkage(emit->base.mir);
    char symbol[512];
    snprintf(symbol, sizeof(symbol), "%s%s", prefix, name);
    if (emit->plat->is_macho) {
        anvil_strbuf_append(&emit->base.code, format->text_section);
        anvil_obj_emit_binding(&emit->base.code, format, linkage, symbol);
        anvil_strbuf_append(&emit->base.code, "\t.p2align 4\n");
        anvil_strbuf_appendf(&emit->base.code, "%s%s:\n", prefix, name);
    } else if (emit->plat->is_coff) {
        char decorated[512];
        if (!emit->source_func ||
            !x86_decorated_symbol(emit->plat, emit->desc, emit->source_func->type, name, decorated, sizeof(decorated)))
            snprintf(decorated, sizeof(decorated), "%s%s", prefix, name);
        anvil_strbuf_append(&emit->base.code, format->text_section);
        anvil_obj_emit_binding(&emit->base.code, format, linkage, decorated);
        anvil_strbuf_appendf(&emit->base.code, "\t.def %s; .scl %d; .type 32; .endef\n", decorated, linkage == ANVIL_LINK_INTERNAL ? 3 : 2);
        anvil_strbuf_appendf(&emit->base.code, "%s:\n", decorated);
    } else {
        anvil_strbuf_append(&emit->base.code, format->text_section);
        anvil_obj_emit_binding(&emit->base.code, format, linkage, symbol);
        anvil_strbuf_appendf(&emit->base.code, "\t.type %s%s, @function\n", prefix, name);
        anvil_strbuf_appendf(&emit->base.code, "%s%s:\n", prefix, name);
    }
}

static void x86_emit_frame_entry(x86_mir_emit_t *emit);

static void x86_emit_prologue(x86_mir_emit_t *emit)
{
    bool cfi = emit->plat->format->cfi;
    if (cfi)
        anvil_strbuf_append(&emit->base.code, "\t.cfi_startproc\n");
    if (!emit->base.wrapped)
        x86_emit_frame_entry(emit);
}

/* The frame: pushes or EBP, locals (at the entry, or at the frame block). */
static void x86_emit_frame_entry(x86_mir_emit_t *emit)
{
    bool cfi = emit->plat->format->cfi;
    if (emit->frame_pointer) {
        anvil_strbuf_append(&emit->base.code, "\tpushl %ebp\n");
        if (cfi)
            anvil_strbuf_append(&emit->base.code, "\t.cfi_def_cfa_offset 8\n\t.cfi_offset %ebp, -8\n");
        anvil_strbuf_append(&emit->base.code, "\tmovl %esp, %ebp\n");
        if (cfi)
            anvil_strbuf_append(&emit->base.code, "\t.cfi_def_cfa_register %ebp\n");
        if (emit->base.frame_size > 0)
            anvil_strbuf_appendf(&emit->base.code, "\tsubl $%d, %%esp\n", emit->base.frame_size);
    } else {
        /* The same layout from ESP: the saves pushed, from EBP's would-be
         * slot down, then the rest. */
        for (int pushed = 0; pushed < emit->num_pushes; pushed++) {
            for (int reg = 0; reg < 8; reg++) {
                if (emit->gpr_save_offsets[reg] != 4 * pushed)
                    continue;
                anvil_strbuf_appendf(&emit->base.code, "\tpushl %%%s\n", x86_gas_gpr(reg, 4));
                if (cfi)
                    anvil_strbuf_appendf(&emit->base.code, "\t.cfi_def_cfa_offset %d\n\t.cfi_offset %%%s, %d\n", 8 + 4 * pushed, x86_gas_gpr(reg, 4),
                                         -emit->gpr_save_offsets[reg] - 8);
            }
        }
        int rest = emit->base.frame_size + 4 - 4 * emit->num_pushes;
        if (rest > 0) {
            anvil_strbuf_appendf(&emit->base.code, "\tsubl $%d, %%esp\n", rest);
            if (cfi)
                anvil_strbuf_appendf(&emit->base.code, "\t.cfi_def_cfa_offset %d\n", emit->base.frame_size + 8);
        }
        return;
    }

    for (int reg = 0; reg < 8; reg++) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->base.code, "\tmovl %%%s, %s\n", x86_gas_gpr(reg, 4), x86_gas_frame(&emit->base, -(emit->gpr_save_offsets[reg])));
            if (cfi)
                anvil_strbuf_appendf(&emit->base.code, "\t.cfi_offset %%%s, %d\n", x86_gas_gpr(reg, 4), -emit->gpr_save_offsets[reg] - 8);
        }
    }
}

/* `pop_bytes`: what the function drops from the caller's stack, as the
 * lowering recorded on RET. */
static void x86_emit_epilogue(x86_mir_emit_t *emit, int64_t pop_bytes)
{
    bool cfi = emit->plat->format->cfi;
    if (cfi)
        anvil_strbuf_append(&emit->base.code, "\t.cfi_remember_state\n");
    if (emit->frame_pointer) {
        for (int reg = 7; reg >= 0; reg--) {
            if (emit->gpr_save_offsets[reg] >= 0)
                anvil_strbuf_appendf(&emit->base.code, "\tmovl %s, %%%s\n", x86_gas_frame(&emit->base, -(emit->gpr_save_offsets[reg])), x86_gas_gpr(reg, 4));
        }
        anvil_strbuf_append(&emit->base.code, "\tmovl %ebp, %esp\n");
        anvil_strbuf_append(&emit->base.code, "\tpopl %ebp\n");
    } else {
        int rest = emit->base.frame_size + 4 - 4 * emit->num_pushes;
        if (rest > 0)
            anvil_strbuf_appendf(&emit->base.code, "\taddl $%d, %%esp\n", rest);
        for (int popped = emit->num_pushes - 1; popped >= 0; popped--) {
            for (int reg = 0; reg < 8; reg++) {
                if (emit->gpr_save_offsets[reg] == 4 * popped)
                    anvil_strbuf_appendf(&emit->base.code, "\tpopl %%%s\n", x86_gas_gpr(reg, 4));
            }
        }
    }
    if (cfi)
        anvil_strbuf_append(&emit->base.code, "\t.cfi_def_cfa %esp, 4\n");
    if (pop_bytes > 0) {
        anvil_strbuf_appendf(&emit->base.code, "\tret $%lld\n", (long long)pop_bytes);
    } else {
        anvil_strbuf_append(&emit->base.code, "\tret\n");
    }
    if (cfi)
        anvil_strbuf_append(&emit->base.code, "\t.cfi_restore_state\n");
}

/* The epilogue, or before the frame block (nothing saved) only the
 * return. */
static void x86_emit_return(x86_mir_emit_t *emit, int64_t pop_bytes)
{
    if (emit->base.wrapped && emit->base.block < anvil_mir_frame_block(emit->base.mir)) {
        if (pop_bytes > 0)
            anvil_strbuf_appendf(&emit->base.code, "\tret $%lld\n", (long long)pop_bytes);
        else
            anvil_strbuf_append(&emit->base.code, "\tret\n");
        return;
    }
    x86_emit_epilogue(emit, pop_bytes);
}

/* EAX <- lo, EDX <- hi as one parallel move; the sources may sit in
 * either destination. */
static void x86_emit_result_pair_move(x86_mir_emit_t *emit, int lo_phys, int hi_phys, int lo_reg, int hi_reg)
{
    int dst[2], src[2];
    size_t count = 0;
    if (lo_phys >= 0) { dst[count] = lo_reg; src[count++] = lo_phys; }
    if (hi_phys >= 0) { dst[count] = hi_reg; src[count++] = hi_phys; }
    x86_emit_parallel_move(emit, dst, src, count);
}

/* The reverse: def <- EAX, hi_def <- EDX right after a call. */
static void x86_emit_call_pair_move(x86_mir_emit_t *emit, int def_phys, int hi_phys, int lo_reg, int hi_reg)
{
    int dst[2], src[2];
    size_t count = 0;
    if (def_phys >= 0) { dst[count] = def_phys; src[count++] = lo_reg; }
    if (hi_phys >= 0) { dst[count] = hi_phys; src[count++] = hi_reg; }
    x86_emit_parallel_move(emit, dst, src, count);
}

static void x86_emit_copy(x86_mir_emit_t *emit, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = x86_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = x86_vreg_info_checked(emit, src);
    if (!dst_info || !src_info || emit->base.failed)
        return;

    if (dst_info->reg_class == ANVIL_MIR_REG_FPR || src_info->reg_class == ANVIL_MIR_REG_FPR) {
        emit->fp->copy(emit, dst, src);
        return;
    }
    int dst_phys = x86_gas_phys_of(&emit->base, dst);
    int src_phys = x86_gas_phys_of(&emit->base, src);
    if (emit->base.failed || dst_phys == src_phys)
        return;
    int size = x86_size_bytes(dst_info->size_bits);
    if (size < 4)
        size = 4;
    anvil_strbuf_appendf(&emit->base.code, "\tmov%s %%%s, %%%s\n", x86_gas_size_suffix(size), x86_gas_gpr(src_phys, size), x86_gas_gpr(dst_phys, size));
}

static void x86_emit_mov(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info || !anvil_mir_isel_materialize(&emit->base.isel, info->def))
        return;
    int dst_phys = x86_gas_phys_of(&emit->base, info->def);
    if (emit->base.failed)
        return;

    int64_t imm = info->has_imm ? info->imm : 0;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        emit->fp->constant(emit, info->def, (uint64_t)imm);
        return;
    }

    /* xor for zero: the flags are dead here (a compare fused with its
     * branch is adjacent to it). */
    const char *dst = x86_gas_gpr(dst_phys, 4);
    if ((int32_t)imm == 0)
        anvil_strbuf_appendf(&emit->base.code, "\txorl %%%s, %%%s\n", dst, dst);
    else
        anvil_strbuf_appendf(&emit->base.code, "\tmovl $%lld, %%%s\n", (long long)(int32_t)imm, dst);
}

/* Integer arithmetic.  ADD..XOR and MUL work on the full registers (the
 * low bits of a narrow result are the same); shifts use the operand width.
 * Variable shift counts go through CL, the instruction's printer scratch
 * (no operand or result is there); division uses EDX:EAX, which only
 * clobbers. */
/* Division in EDX:EAX. */
static void x86_gpr_divide(void *user, size_t index, const anvil_mir_instr_info_t *info, int dst_phys, int a_phys, int b_phys, int size)
{
    x86_mir_emit_t *emit = user;
    (void)index;
    (void)size;
    const char *a = x86_gas_gpr(a_phys, 4), *b = x86_gas_gpr(b_phys, 4), *dst = x86_gas_gpr(dst_phys, 4);
    switch (info->op) {
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD: {
        bool is_signed = info->op == ANVIL_MIR_OP_SDIV || info->op == ANVIL_MIR_OP_SMOD;
        bool is_mod = info->op == ANVIL_MIR_OP_SMOD || info->op == ANVIL_MIR_OP_UMOD;
        /* A divisor in EAX or EDX, which the dividend overwrites, divides
         * from a stack copy. */
        bool divisor_in_pair = b_phys == X86_EAX || b_phys == X86_EDX;
        if (divisor_in_pair)
            anvil_strbuf_appendf(&emit->base.code, "\tpushl %%%s\n", b);
        if (a_phys != X86_EAX)
            anvil_strbuf_appendf(&emit->base.code, "\tmovl %%%s, %%eax\n", a);
        anvil_strbuf_append(&emit->base.code, is_signed ? "\tcltd\n" : "\txorl %edx, %edx\n");
        if (divisor_in_pair)
            anvil_strbuf_appendf(&emit->base.code, "\t%sl (%%esp)\n\taddl $4, %%esp\n", is_signed ? "idiv" : "div");
        else
            anvil_strbuf_appendf(&emit->base.code, "\t%sl %%%s\n", is_signed ? "idiv" : "div", b);
        int result = is_mod ? X86_EDX : X86_EAX;
        if (dst_phys != result)
            anvil_strbuf_appendf(&emit->base.code, "\tmovl %%%s, %%%s\n", x86_gas_gpr(result, 4), dst);
        break;
    }
    default:
        emit->base.failed = true;
        break;
    }
}

/* A lane-wise operation (SSE2 CPUs), through the printer x86-64 shares. */
static void x86_emit_vector(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    x86_sse_printer_t printer = {
        .code = &emit->base.code,
        .mir = emit->base.mir,
        .num_xmm = X86_NUM_FPR,
        .temp = x86_gas_xmm(X86_SSE_TEMP),
        .temp2 = x86_gas_xmm(X86_SSE_TEMP2),
    };
    x86_sse_emit_vector(&printer, anvil_vector_isa_for_mir(x86_sse_isa, x86_sse_isa_count, emit->base.mir, instr_index, info, emit->features),
                        instr_index, info);
    emit->base.failed |= printer.failed;
}

/* A scalar operation the table prices: an FP unit instruction, or the high
 * half of a product (EDX:EAX, which the instruction clobbers). */
static void x86_emit_scalar(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    const anvil_vector_isa_t *isa = anvil_vector_isa_for_mir(x86_scalar_isa, x86_scalar_isa_count, emit->base.mir, instr_index, info, emit->features);
    anvil_mir_vreg_t a = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
    if (!isa || a == ANVIL_MIR_NO_VREG) {
        emit->base.failed = true;
        return;
    }
    if (info->ir_op != ANVIL_OP_UMULH && info->ir_op != ANVIL_OP_SMULH) {
        emit->fp->apply(emit, isa->mnemonic, info->def, a);
        return;
    }
    anvil_mir_vreg_t b = anvil_mir_get_instr_use(emit->base.mir, instr_index, 1);
    int a_phys = x86_gas_phys_of(&emit->base, a);
    int b_phys = b == ANVIL_MIR_NO_VREG ? -1 : x86_gas_phys_of(&emit->base, b);
    int dst_phys = x86_gas_phys_of(&emit->base, info->def);
    if (b_phys < 0 || emit->base.failed) {
        emit->base.failed = true;
        return;
    }
    /* The product commutes: a multiplier already in EAX is the one kept
     * there (MUL reads the other before it writes EDX). */
    if (b_phys == X86_EAX) {
        b_phys = a_phys;
        a_phys = X86_EAX;
    }
    if (a_phys != X86_EAX)
        anvil_strbuf_appendf(&emit->base.code, "\tmovl %%%s, %%eax\n", x86_gas_gpr(a_phys, 4));
    anvil_strbuf_appendf(&emit->base.code, "\t%s %%%s\n", isa->mnemonic, x86_gas_gpr(b_phys, 4));
    if (dst_phys != X86_EDX)
        anvil_strbuf_appendf(&emit->base.code, "\tmovl %%edx, %%%s\n", x86_gas_gpr(dst_phys, 4));
}


static void x86_emit_unary(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->base.failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info || emit->base.failed)
        return;

    if (info->op == ANVIL_MIR_OP_NEG) {
        if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
            emit->fp->unary(emit, info->op, info->def, src);
        } else {
            int size = x86_size_bytes(def_info->size_bits);
            if (size < 4)
                size = 4;
            const char *suf = x86_gas_size_suffix(size);
            const char *dst = x86_gas_gpr(x86_gas_phys_of(&emit->base, info->def), size);
            const char *s = x86_gas_gpr(x86_gas_phys_of(&emit->base, src), size);
            if (emit->base.failed)
                return;
            anvil_strbuf_appendf(&emit->base.code, "\tmov%s %%%s, %%%s\n", suf, s, dst);
            anvil_strbuf_appendf(&emit->base.code, "\tneg%s %%%s\n", suf, dst);
        }
    } else if (info->op == ANVIL_MIR_OP_FABS && def_info->reg_class == ANVIL_MIR_REG_FPR) {
        emit->fp->unary(emit, info->op, info->def, src);
    } else if (info->op == ANVIL_MIR_OP_NOT && def_info->reg_class == ANVIL_MIR_REG_GPR) {
        int size = x86_size_bytes(def_info->size_bits);
        if (size < 4)
            size = 4;
        const char *suf = x86_gas_size_suffix(size);
        const char *dst = x86_gas_gpr(x86_gas_phys_of(&emit->base, info->def), size);
        const char *s = x86_gas_gpr(x86_gas_phys_of(&emit->base, src), size);
        if (emit->base.failed)
            return;
        anvil_strbuf_appendf(&emit->base.code, "\tmov%s %%%s, %%%s\n", suf, s, dst);
        anvil_strbuf_appendf(&emit->base.code, "\tnot%s %%%s\n", suf, dst);
    } else {
        emit->base.failed = true;
    }
}

static void x86_emit_gpr_extend(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info, bool sign_extend)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->base.failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = x86_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = x86_vreg_info_checked(emit, src);
    int dst_phys = x86_gas_phys_of(&emit->base, info->def);
    int src_phys = x86_gas_phys_of(&emit->base, src);
    if (!dst_info || !src_info || emit->base.failed)
        return;
    if (dst_info->reg_class != ANVIL_MIR_REG_GPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        emit->base.failed = true;
        return;
    }

    int src_size = x86_size_bytes(src_info->size_bits);
    int dst_size = x86_size_bytes(dst_info->size_bits);
    if (dst_size < src_size)
        dst_size = src_size;
    if (dst_size < 4)
        dst_size = 4;

    /* A 32-bit source, a narrow load of the same signedness (which already
     * extended the value, x86_emit_load), or a value already zero above
     * its width. */
    if (src_size >= 4 || (src_info->is_signed == sign_extend && anvil_mir_isel_def_op(&emit->base.isel, src) == ANVIL_MIR_OP_LOAD) ||
        (!sign_extend && anvil_mir_isel_zero_extended(&emit->base.isel, src))) {
        if (src_phys != dst_phys)
            anvil_strbuf_appendf(&emit->base.code, "\tmovl %%%s, %%%s\n", x86_gas_gpr(src_phys, 4), x86_gas_gpr(dst_phys, 4));
        return;
    }

    const char *zs = sign_extend ? "movs" : "movz";
    if (src_size == 1 && !x86_reg_has_byte(src_phys)) {
        emit->base.failed = true;
        return;
    }
    const char *suffix = src_size == 1 ? "bl" : "wl";
    anvil_strbuf_appendf(&emit->base.code, "\t%s%s %%%s, %%%s\n", zs, suffix, x86_gas_gpr(src_phys, src_size), x86_gas_gpr(dst_phys, 4));
}

static void x86_emit_cast(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->base.failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = x86_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = x86_vreg_info_checked(emit, src);
    if (!dst_info || !src_info)
        return;

    switch (info->op) {
    case ANVIL_MIR_OP_ZEXT:
        x86_emit_gpr_extend(emit, instr_index, info, false);
        break;
    case ANVIL_MIR_OP_SEXT:
        x86_emit_gpr_extend(emit, instr_index, info, true);
        break;
    case ANVIL_MIR_OP_TRUNC:
    case ANVIL_MIR_OP_BITCAST:
        x86_emit_copy(emit, info->def, src);
        break;
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP:
        if (dst_info->reg_class != ANVIL_MIR_REG_FPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
            emit->base.failed = true;
            return;
        }
        emit->fp->convert(emit, info->op, info->def, src);
        break;
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI:
        if (dst_info->reg_class != ANVIL_MIR_REG_GPR || src_info->reg_class != ANVIL_MIR_REG_FPR) {
            emit->base.failed = true;
            return;
        }
        emit->fp->convert(emit, info->op, info->def, src);
        break;
    case ANVIL_MIR_OP_FPEXT:
    case ANVIL_MIR_OP_FPTRUNC:
        if (dst_info->reg_class != ANVIL_MIR_REG_FPR || src_info->reg_class != ANVIL_MIR_REG_FPR) {
            emit->base.failed = true;
            return;
        }
        emit->fp->convert(emit, info->op, info->def, src);
        break;
    default:
        emit->base.failed = true;
        break;
    }
}

static void x86_emit_dyn_alloca(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm <= 0 || info->num_uses != 1) {
        emit->base.failed = true;
        return;
    }

    anvil_mir_vreg_t count = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
    if (count == ANVIL_MIR_NO_VREG) {
        emit->base.failed = true;
        return;
    }

    const char *count_reg = x86_gas_gpr(x86_gas_phys_of(&emit->base, count), 4);
    int dst_phys = x86_gas_phys_of(&emit->base, info->def);
    if (emit->base.failed)
        return;

    /* EAX is the instruction's clobber: the count is read first. */
    anvil_strbuf_appendf(&emit->base.code, "\tmovl %%%s, %%eax\n", count_reg);
    if (info->imm != 1) {
        anvil_strbuf_appendf(&emit->base.code, "\timull $%lld, %%eax, %%eax\n", (long long)info->imm);
    }
    anvil_strbuf_append(&emit->base.code, "\taddl $15, %eax\n");
    anvil_strbuf_append(&emit->base.code, "\tandl $-16, %eax\n");
    /* Relocate the outgoing area below each dynamic allocation; otherwise
     * CALL_STACK_ARG overwrites the buffer. */
    if (emit->base.outgoing_size)
        anvil_strbuf_appendf(&emit->base.code, "\taddl $%d, %%eax\n", emit->base.outgoing_size);
    anvil_strbuf_append(&emit->base.code, "\tsubl %eax, %esp\n");
    anvil_strbuf_appendf(&emit->base.code, "\tleal %d(%%esp), %%%s\n", emit->base.outgoing_size, x86_gas_gpr(dst_phys, 4));
}

void x86_emit_got_pointer(x86_mir_emit_t *emit, const char *reg)
{
    const char *function = anvil_mir_func_name(emit->base.mir);
    unsigned label = emit->pic_labels++;
    anvil_strbuf_appendf(&emit->base.code, "\tcall .L%s_pic%u\n.L%s_pic%u:\n\tpopl %%%s\n", function, label, function, label, reg);
    anvil_strbuf_appendf(&emit->base.code, "\taddl $_GLOBAL_OFFSET_TABLE_+(.-.L%s_pic%u), %%%s\n", function, label, reg);
}

static void x86_emit_symbol_addr(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->symbol || !info->symbol[0]) {
        emit->base.failed = true;
        return;
    }
    if (!anvil_mir_isel_materialize(&emit->base.isel, info->def))
        return;

    int dst_phys = x86_gas_phys_of(&emit->base, info->def);
    if (emit->base.failed)
        return;

    const char *prefix = x86_gas_symbol_ref_prefix(&emit->base, info->symbol);
    const char *dst = x86_gas_gpr(dst_phys, 4);
    if (emit->reloc != ANVIL_RELOC_STATIC) {
        /* From the GOT, or GOT-relative for this module's own symbols. */
        x86_emit_got_pointer(emit, dst);
        if (info->symbol_got) {
            anvil_strbuf_appendf(&emit->base.code, "\tmovl %s%s@GOT(%%%s), %%%s\n", prefix, info->symbol, dst, dst);
            if (info->has_imm && info->imm != 0)
                anvil_strbuf_appendf(&emit->base.code, "\taddl $%lld, %%%s\n", (long long)info->imm, dst);
        } else {
            anvil_strbuf_appendf(&emit->base.code, "\tleal %s%s@GOTOFF", prefix, info->symbol);
            if (info->has_imm && info->imm != 0)
                anvil_strbuf_appendf(&emit->base.code, "%+lld", (long long)info->imm);
            anvil_strbuf_appendf(&emit->base.code, "(%%%s), %%%s\n", dst, dst);
        }
        return;
    }
    anvil_strbuf_appendf(&emit->base.code, "\tmovl $%s%s", prefix, info->symbol);
    if (info->has_imm && info->imm != 0)
        anvil_strbuf_appendf(&emit->base.code, "%+lld", (long long)info->imm);
    anvil_strbuf_appendf(&emit->base.code, ", %%%s\n", dst);
}

/* The FP unit's register-memory moves, for the shared spill and
 * stack-argument printers. */
static void x86_fp_load(void *gas, anvil_mir_vreg_t vreg, const char *mem)
{
    x86_mir_emit_t *emit = gas;
    emit->fp->load(emit, vreg, mem);
}

static void x86_fp_store(void *gas, anvil_mir_vreg_t vreg, const char *mem)
{
    x86_mir_emit_t *emit = gas;
    emit->fp->store(emit, vreg, mem);
}

static void x86_fp_compare(void *gas, anvil_mir_vreg_t a, anvil_mir_vreg_t b)
{
    x86_mir_emit_t *emit = gas;
    emit->fp->compare(emit, a, b);
}

static void x86_fp_binary(void *gas, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    x86_mir_emit_t *emit = gas;
    emit->fp->binary(emit, op, def, lhs, rhs);
}

/* The i386 mode of the shared printer (`user`: the platform, NULL for the
 * registerless marking). */
static void x86_mode(x86_gas_t *gas, const void *user)
{
    const anvil_x86_plat_desc_t *plat = user;
    gas->pointer_bytes = 4;
    gas->symbol_prefix = plat ? plat->sym_prefix : "";
    gas->wraps_imm32 = true;
    gas->fp_memory_operands = false;
    gas->stack_reg = "esp";
    gas->min_gpr_move = 4;
    gas->min_operation_bytes = 4;
    gas->has_byte = x86_reg_has_byte;
    gas->fp_load = x86_fp_load;
    gas->fp_store = x86_fp_store;
    gas->fp_compare = x86_fp_compare;
    gas->fp_binary = x86_fp_binary;
    gas->gpr_divide = x86_gpr_divide;
    gas->local_label_prefix = plat ? plat->format->local_label_prefix : "";
}

bool anvil_x86_mark_registerless(anvil_mir_func_t *mir)
{
    return x86_gas_mark_registerless(mir, x86_mode, NULL);
}

static void x86_emit_incoming_stack_arg(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm < 0 || info->imm > INT32_MAX - 32) {
        emit->base.failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    int def_phys = x86_gas_phys_of(&emit->base, info->def);
    if (!def_info || emit->base.failed)
        return;

    int size = x86_size_bytes(def_info->size_bits);
    int frame_offset = 8 + (int)info->imm;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        char mem[32];
        snprintf(mem, sizeof(mem), "%s", x86_gas_frame(&emit->base, (int)(frame_offset)));
        emit->fp->load(emit, info->def, mem);
        return;
    }

    const char *op;
    if (def_info->is_signed && size == 1)
        op = "movsbl";
    else if (def_info->is_signed && size == 2)
        op = "movswl";
    else if (!def_info->is_signed && size == 1)
        op = "movzbl";
    else if (!def_info->is_signed && size == 2)
        op = "movzwl";
    else
        op = "movl";
    anvil_strbuf_appendf(&emit->base.code, "\t%s %s, %%%s\n", op, x86_gas_frame(&emit->base, (int)(frame_offset)), x86_gas_gpr(def_phys, 4));
}

static void x86_emit_call(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    const anvil_x86_cc_desc_t *call_desc = anvil_x86_get_cc_desc(info->call_cc);
    if (!call_desc) {
        emit->base.failed = true;
        return;
    }
    bool direct = info->symbol && info->symbol[0];
    size_t arg_start = direct ? 0 : 1;

    /* Register arguments move in parallel: an argument may sit in another
     * argument's register.  An indirect target in one of those registers
     * moves along into EAX (no convention passes an argument there, and the
     * call clobbers it). */
    int move_src[ANVIL_PARALLEL_MOVE_MAX], move_dst[ANVIL_PARALLEL_MOVE_MAX];
    size_t num_moves = 0;
    for (size_t u = arg_start; u < info->num_uses; u++) {
        size_t idx = u - arg_start;
        if ((int)idx >= call_desc->num_reg_int_args || num_moves == ANVIL_PARALLEL_MOVE_MAX - 1)
            break;
        move_src[num_moves] = x86_gas_phys_of(&emit->base, anvil_mir_get_instr_use(emit->base.mir, instr_index, u));
        move_dst[num_moves++] = call_desc->reg_int_args[idx];
        if (emit->base.failed)
            return;
    }
    int target_phys = direct ? -1 : x86_gas_phys_of(&emit->base, anvil_mir_get_instr_use(emit->base.mir, instr_index, 0));
    for (size_t m = 0; target_phys >= 0 && m < num_moves; m++) {
        if (move_dst[m] == target_phys) {
            move_src[num_moves] = target_phys;
            move_dst[num_moves++] = X86_EAX;
            target_phys = X86_EAX;
            break;
        }
    }
    if (emit->base.failed)
        return;
    x86_emit_parallel_move(emit, move_dst, move_src, num_moves);

    if (direct) {
        /* Lowering already spelled COFF stdcall/fastcall names in full. */
        bool decorated = emit->plat->is_coff && call_desc->decor != X86_DECOR_NONE;
        const char *prefix = decorated ? "" : x86_gas_symbol_ref_prefix(&emit->base, info->symbol);
        if (info->symbol_got && emit->reloc != ANVIL_RELOC_STATIC) {
            /* Through the GOT entry (EAX carries no argument here). */
            x86_emit_got_pointer(emit, "eax");
            anvil_strbuf_appendf(&emit->base.code, "\tcall *%s%s@GOT(%%eax)\n", prefix, info->symbol);
        } else {
            anvil_strbuf_appendf(&emit->base.code, "\tcall %s%s\n", prefix, info->symbol);
        }
    } else {
        anvil_strbuf_appendf(&emit->base.code, "\tcall *%%%s\n", x86_gas_gpr(target_phys, 4));
    }

    /* A callee that dropped stack bytes leaves ESP above the outgoing
     * argument area the frame reserved. */
    if (info->has_imm && info->imm > 0)
        anvil_strbuf_appendf(&emit->base.code, "\tsubl $%lld, %%esp\n", (long long)info->imm);

    /* A 32-bit CALL_RESULT right after the call takes EDX; both moves
     * happen together. */
    int def_phys = info->def != ANVIL_MIR_NO_VREG ? x86_gas_phys_of(&emit->base, info->def) : -1;
    int hi_phys = -1;
    anvil_mir_instr_info_t next;
    if (anvil_mir_get_instr_info(emit->base.mir, instr_index + 1, &next) && next.op == ANVIL_MIR_OP_CALL_RESULT &&
        next.block == info->block) {
        const anvil_mir_vreg_info_t *next_info = x86_vreg_info_checked(emit, next.def);
        if (next_info && next_info->reg_class == ANVIL_MIR_REG_GPR) {
            hi_phys = x86_gas_phys_of(&emit->base, next.def);
            emit->call_hi_moved = true;
        }
    }
    if (!emit->base.failed)
        x86_emit_call_pair_move(emit, def_phys, hi_phys, call_desc->int_ret_reg, call_desc->int_ret_hi_reg);
}

static void x86_emit_ret(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (info->num_uses == 1) {
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
        const anvil_mir_vreg_info_t *ret_info = x86_vreg_info_checked(emit, ret);
        int ret_phys = x86_gas_phys_of(&emit->base, ret);
        if (!ret_info || emit->base.failed)
            return;

        if (ret_info->reg_class == ANVIL_MIR_REG_FPR) {
            emit->fp->push_result(emit, ret);
        } else {
            x86_emit_result_pair_move(emit, ret_phys, emit->ret_hi_phys, emit->desc->int_ret_reg, emit->desc->int_ret_hi_reg);
        }
    }
    emit->ret_hi_phys = -1;
    if (!emit->base.failed)
        x86_emit_return(emit, info->has_imm ? info->imm : 0);
}

static void x86_emit_other(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (info->def == ANVIL_MIR_NO_VREG) {
        return;
    }
    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info || emit->base.failed)
        return;

    int phys = x86_gas_phys_of(&emit->base, info->def);
    if (emit->base.failed)
        return;

    if (def_info->reg_class == ANVIL_MIR_REG_GPR) {
        if (emit->call_hi_moved) {
            emit->call_hi_moved = false;
            return;
        }
        if (phys != emit->desc->int_ret_hi_reg) {
            anvil_strbuf_appendf(&emit->base.code, "\tmovl %%%s, %%%s\n", x86_gas_gpr(emit->desc->int_ret_hi_reg, 4), x86_gas_gpr(phys, 4));
        }
        return;
    }

    emit->fp->pop_result(emit, info->def);
}

/* Atomics: pointer in EDX, operand in ECX, accumulator EAX (CMPXCHG:
 * expected in EAX, desired in ECX); the old value comes back in EAX.
 * Lowering admits CMPXCHG/XADD only on i486 and later models. */
static void x86_emit_atomic(x86_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info)
{
    if (info->ir_op == ANVIL_OP_ATOMIC_FENCE) {
        if (info->atomic.order == ANVIL_ORDER_SEQ_CST)
            anvil_strbuf_append(&emit->base.code, (emit->features & ANVIL_FEATURE_X86_SSE2) ? "\tmfence\n" : "\tlock orl $0, (%esp)\n");
        return;
    }

    anvil_mir_vreg_t value = info->def != ANVIL_MIR_NO_VREG ? info->def : anvil_mir_get_instr_use(emit->base.mir, index, 1);
    const anvil_mir_vreg_info_t *type = x86_vreg_info_checked(emit, value);
    if (!type || emit->base.failed)
        return;

    int dst[3] = {X86_EDX, X86_ECX, X86_EAX};
    if (info->ir_op == ANVIL_OP_ATOMIC_CMPXCHG) {
        dst[1] = X86_EAX;
        dst[2] = X86_ECX;
    }
    int src[3];
    size_t count = info->num_uses < 3 ? info->num_uses : 3;
    for (size_t operand = 0; operand < count; operand++)
        src[operand] = x86_gas_phys_of(&emit->base, anvil_mir_get_instr_use(emit->base.mir, index, operand));
    if (emit->base.failed)
        return;
    x86_emit_parallel_move(emit, dst, src, count);

    int size = x86_size_bytes(type->size_bits);
    const char *suffix = x86_gas_size_suffix(size);
    const char *accumulator = x86_gas_gpr(X86_EAX, size);
    const char *operand = x86_gas_gpr(X86_ECX, size);
    switch (info->ir_op) {
    case ANVIL_OP_ATOMIC_LOAD:
        anvil_strbuf_appendf(&emit->base.code, "\tmov%s (%%edx), %%%s\n", suffix, accumulator);
        break;
    case ANVIL_OP_ATOMIC_STORE:
        anvil_strbuf_appendf(&emit->base.code, "\t%s%s %%%s, (%%edx)\n", info->atomic.order == ANVIL_ORDER_SEQ_CST ? "xchg" : "mov", suffix, operand);
        return;
    case ANVIL_OP_ATOMIC_CMPXCHG:
        anvil_strbuf_appendf(&emit->base.code, "\tlock cmpxchg%s %%%s, (%%edx)\n", suffix, operand);
        break;
    case ANVIL_OP_ATOMIC_RMW:
        if (info->atomic.rmw <= ANVIL_ATOMIC_SUB) {
            if (info->atomic.rmw == ANVIL_ATOMIC_SUB)
                anvil_strbuf_appendf(&emit->base.code, "\tneg%s %%%s\n", suffix, operand);
            const char *operation = info->atomic.rmw == ANVIL_ATOMIC_EXCHANGE ? "xchg" : "lock xadd";
            anvil_strbuf_appendf(&emit->base.code, "\t%s%s %%%s, (%%edx)\n\tmovl %%ecx, %%eax\n", operation, suffix, operand);
        } else {
            /* EBX (callee-saved, allocatable) is borrowed around the loop. */
            const char *operation = info->atomic.rmw == ANVIL_ATOMIC_AND ? "and" : (info->atomic.rmw == ANVIL_ATOMIC_OR ? "or" : "xor");
            const char *temporary = x86_gas_gpr(X86_EBX, size);
            const char *prefix = emit->plat->format->local_label_prefix;
            const char *function = anvil_mir_func_name(emit->base.mir);
            anvil_strbuf_appendf(&emit->base.code, "\tpushl %%ebx\n\tmov%s (%%edx), %%%s\n%s%s_atomic_%zu:\n", suffix, accumulator, prefix, function, index);
            anvil_strbuf_appendf(&emit->base.code, "\tmovl %%eax, %%ebx\n\t%s%s %%%s, %%%s\n", operation, suffix, operand, temporary);
            anvil_strbuf_appendf(&emit->base.code, "\tlock cmpxchg%s %%%s, (%%edx)\n\tjne %s%s_atomic_%zu\n\tpopl %%ebx\n", suffix, temporary, prefix, function, index);
        }
        break;
    default:
        emit->base.failed = true;
        return;
    }

    /* Narrow results are kept extended, as loads leave them. */
    if (info->def != ANVIL_MIR_NO_VREG) {
        int def_phys = x86_gas_phys_of(&emit->base, info->def);
        if (emit->base.failed)
            return;
        if (size < 4)
            anvil_strbuf_appendf(&emit->base.code, "\tmov%c%cl %%%s, %%%s\n", type->is_signed ? 's' : 'z', size == 1 ? 'b' : 'w',
                                 accumulator, x86_gas_gpr(def_phys, 4));
        else if (def_phys != X86_EAX)
            anvil_strbuf_appendf(&emit->base.code, "\tmovl %%eax, %%%s\n", x86_gas_gpr(def_phys, 4));
    }
}

static void x86_emit_instr(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (!anvil_mir_isel_registerless_ok(emit->base.mir, &emit->base.isel, instr_index, info)) {
        emit->base.failed = true;
        return;
    }
    if (instr_index == emit->base.adc_skip) {
        emit->base.adc_skip = SIZE_MAX;
        return;
    }
    if (instr_index == emit->base.adc_at) {
        /* mov keeps the carry. */
        emit->base.adc_at = SIZE_MAX;
        int to = x86_gas_phys_of(&emit->base, info->def), from = x86_gas_phys_of(&emit->base, emit->base.adc_other);
        if (emit->base.failed || to < 0 || from < 0) {
            emit->base.failed = true;
            return;
        }
        if (to != from)
            anvil_strbuf_appendf(&emit->base.code, "\tmovl %%%s, %%%s\n", x86_gas_gpr(from, 4), x86_gas_gpr(to, 4));
        anvil_strbuf_appendf(&emit->base.code, "\tadcl $0, %%%s\n", x86_gas_gpr(to, 4));
        return;
    }
    switch (info->op) {
    case ANVIL_MIR_OP_ATOMIC:
        x86_emit_atomic(emit, instr_index, info);
        return;
    case ANVIL_MIR_OP_MOV:
        x86_emit_mov(emit, info);
        break;
    case ANVIL_MIR_OP_COPY: {
        anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
        if (src == ANVIL_MIR_NO_VREG) {
            emit->base.failed = true;
            break;
        }
        x86_emit_copy(emit, info->def, src);
        break;
    }
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_MUL:
    case ANVIL_MIR_OP_DIV:
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_FDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD:
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR:
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
        x86_gas_emit_binary(&emit->base, instr_index, info);
        break;
    case ANVIL_MIR_OP_NEG:
    case ANVIL_MIR_OP_NOT:
    case ANVIL_MIR_OP_FABS:
        x86_emit_unary(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SCALAR:
        x86_emit_scalar(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_VECTOR:
        x86_emit_vector(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_ZEXT:
    case ANVIL_MIR_OP_SEXT:
    case ANVIL_MIR_OP_TRUNC:
    case ANVIL_MIR_OP_BITCAST:
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP:
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI:
    case ANVIL_MIR_OP_FPEXT:
    case ANVIL_MIR_OP_FPTRUNC:
        x86_emit_cast(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_FCMP:
    case ANVIL_MIR_OP_CMP:
    case ANVIL_MIR_OP_CMP_EQ:
    case ANVIL_MIR_OP_CMP_NE:
    case ANVIL_MIR_OP_CMP_LT:
    case ANVIL_MIR_OP_CMP_LE:
    case ANVIL_MIR_OP_CMP_GT:
    case ANVIL_MIR_OP_CMP_GE:
    case ANVIL_MIR_OP_CMP_ULT:
    case ANVIL_MIR_OP_CMP_ULE:
    case ANVIL_MIR_OP_CMP_UGT:
    case ANVIL_MIR_OP_CMP_UGE:
        x86_gas_emit_cmp(&emit->base, instr_index, info);
        break;
    case ANVIL_MIR_OP_SYMBOL_ADDR:
        x86_emit_symbol_addr(emit, info);
        break;
    case ANVIL_MIR_OP_LOAD:
        x86_gas_emit_load(&emit->base, instr_index, info);
        break;
    case ANVIL_MIR_OP_STORE:
        x86_gas_emit_store(&emit->base, instr_index, info);
        break;
    case ANVIL_MIR_OP_FRAME_ADDR:
        x86_gas_emit_frame_addr(&emit->base, info);
        break;
    case ANVIL_MIR_OP_DYN_ALLOCA:
        x86_emit_dyn_alloca(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        x86_emit_incoming_stack_arg(emit, info);
        break;
    case ANVIL_MIR_OP_VA_START:
        /* Address of the first unnamed stack argument. */
        anvil_strbuf_appendf(&emit->base.code, "\tleal %s, %%%s\n", x86_gas_frame(&emit->base, (int)((8 + info->imm))),
                             x86_gas_gpr(x86_gas_phys_of(&emit->base, info->def), 4));
        break;
    case ANVIL_MIR_OP_CALL:
        x86_emit_call(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        x86_gas_emit_call_stack_arg(&emit->base, instr_index, info);
        break;
    case ANVIL_MIR_OP_BR:
        emit->base.failed |= !x86_gas_emit_jump(&emit->base.code, emit->plat->format->local_label_prefix, emit->base.mir, &emit->base.layout, emit->base.block, info->true_block);
        break;
    case ANVIL_MIR_OP_SELECT:
        x86_gas_emit_select(&emit->base, instr_index, info);
        break;
    case ANVIL_MIR_OP_BR_COND: {
        const char *cc = emit->base.fused_jcc;
        emit->base.fused_jcc = NULL;
        if (!cc) {
            anvil_mir_vreg_t cond = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
            const anvil_mir_vreg_info_t *cond_info = cond == ANVIL_MIR_NO_VREG ? NULL : x86_vreg_info_checked(emit, cond);
            int cond_phys = cond_info ? x86_gas_phys_of(&emit->base, cond) : -1;
            if (!cond_info || emit->base.failed) {
                emit->base.failed = true;
                break;
            }
            /* A boolean tests in its byte register. */
            int size = x86_reg_has_byte(cond_phys) ? x86_size_bytes(cond_info->size_bits) : 4;
            const char *reg = x86_gas_gpr(cond_phys, size);
            anvil_strbuf_appendf(&emit->base.code, "\ttest%s %%%s, %%%s\n", x86_gas_size_suffix(size), reg, reg);
            cc = "ne";
        }
        emit->base.failed |= !x86_gas_emit_cond_branch(&emit->base.code, emit->plat->format->local_label_prefix, emit->base.mir, &emit->base.layout, emit->base.block, cc,
                                                  info->true_block, info->false_block);
        break;
    }
    case ANVIL_MIR_OP_RET:
        x86_emit_ret(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SPILL_LOAD:
        x86_gas_emit_spill_load(&emit->base, info);
        break;
    case ANVIL_MIR_OP_SPILL_STORE:
        x86_gas_emit_spill_store(&emit->base, instr_index, info);
        break;
    case ANVIL_MIR_OP_RET_VALUE_PART:
        /* Moved together with RET's value. */
        emit->ret_hi_phys = x86_gas_phys_of(&emit->base, anvil_mir_get_instr_use(emit->base.mir, instr_index, 0));
        break;
    case ANVIL_MIR_OP_CALL_RESULT:
        x86_emit_other(emit, info);
        break;
    case ANVIL_MIR_OP_KEEPALIVE:
        break;
    default:
        emit->base.failed = true;
        break;
    }
}

static void x86_emit_rodata(x86_mir_emit_t *emit)
{
    const anvil_obj_format_t *format = emit->plat->format;
    const char *name = anvil_mir_func_name(emit->base.mir);
    if (!anvil_gnu_emit_mir_strings(&emit->base.code, emit->base.mir, format)) {
        emit->base.failed = true;
        return;
    }

    /* The floating literal pool. */
    if (emit->fp_pool_count) {
        anvil_strbuf_append(&emit->base.code, format->rodata_section);
        anvil_obj_emit_align(&emit->base.code, 8);
    }
    for (size_t i = 0; i < emit->fp_pool_count; i++) {
        uint64_t bits = emit->fp_pool[i].bits;
        anvil_strbuf_appendf(&emit->base.code, "%s%s_fpc%zu:\n\t.long %u", format->local_label_prefix, name, i, (uint32_t)bits);
        if (emit->fp_pool[i].size > 4)
            anvil_strbuf_appendf(&emit->base.code, ", %u", (uint32_t)(bits >> 32));
        anvil_strbuf_append(&emit->base.code, "\n");
    }

    /* Sign masks, per function so several functions can use them. */
    if (emit->base.emitted_fneg_mask || emit->base.emitted_fabs_mask) {
        anvil_strbuf_append(&emit->base.code, format->literal16_section);
        anvil_obj_emit_align(&emit->base.code, 16);
    }
    if (emit->base.emitted_fneg_mask) {
        anvil_strbuf_appendf(&emit->base.code, "%s%s_fneg_mask32:\n\t.long 0x80000000, 0, 0, 0\n", emit->plat->format->local_label_prefix, name);
        anvil_strbuf_appendf(&emit->base.code, "%s%s_fneg_mask64:\n\t.long 0, 0x80000000, 0, 0\n", emit->plat->format->local_label_prefix, name);
    }
    if (emit->base.emitted_fabs_mask) {
        anvil_strbuf_appendf(&emit->base.code, "%s%s_fabs_mask32:\n\t.long 0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff\n", emit->plat->format->local_label_prefix, name);
        anvil_strbuf_appendf(&emit->base.code, "%s%s_fabs_mask64:\n\t.long 0xffffffff, 0x7fffffff, 0xffffffff, 0xffffffff\n", emit->plat->format->local_label_prefix, name);
    }
}

bool anvil_x86_emit_mir_features(const anvil_mir_func_t *mir, anvil_func_t *func, anvil_abi_t abi, anvil_syntax_t syntax,
                                 anvil_cpu_features_t features, char **output, size_t *len)
{
    return x86_emit_mir_reloc(mir, func, abi, syntax, features, ANVIL_RELOC_STATIC, output, len);
}

bool x86_emit_mir_reloc(const anvil_mir_func_t *mir, anvil_func_t *func, anvil_abi_t abi, anvil_syntax_t syntax,
                        anvil_cpu_features_t features, anvil_reloc_model_t reloc, char **output, size_t *len)
{
    if (!mir || !output)
        return false;
    *output = NULL;
    if (len)
        *len = 0;
    if (!anvil_x86_verify_mir_legal(mir, NULL, 0))
        return false;

    if (func && (!func->type || func->type->kind != ANVIL_TYPE_FUNC))
        return false;
    const anvil_x86_cc_desc_t *desc = anvil_x86_get_cc_desc(func ? func->type->data.func.cc : ANVIL_CC_CDECL);
    const anvil_x86_plat_desc_t *plat = anvil_x86_get_plat_desc(abi);
    if (!desc || !plat)
        return false;

    x86_mir_emit_t emit;
    memset(&emit, 0, sizeof(emit));
    emit.base.adc_skip = emit.base.adc_at = emit.base.test_at = SIZE_MAX;
    emit.base.fold_target = SIZE_MAX;
    emit.base.mir = mir;
    emit.source_func = func;
    emit.desc = desc;
    emit.plat = plat;
    emit.base.syntax = syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : syntax;
    emit.features = features;
    emit.reloc = reloc == ANVIL_RELOC_DEFAULT || !plat->format->got_relocations ? ANVIL_RELOC_STATIC : reloc;
    /* Floating point in SSE2 registers where the CPU model has them,
     * otherwise on the x87 FPU. */
    emit.fp = (features & ANVIL_FEATURE_X86_SSE2) ? &x86_fp_sse2_unit : &x86_fp_x87_unit;
    x86_mode(&emit.base, plat);
    emit.ret_hi_phys = -1;
    anvil_strbuf_init(&emit.base.code);
    if (!emit.base.code.data)
        return false;

    /* The facts first: the frame saves the registers printed code writes. */
    if (!anvil_mir_isel_facts_build(&emit.base.isel, mir, x86_gas_use_takes_immediate, x86_use_takes_symbol, &emit)) {
        anvil_strbuf_destroy(&emit.base.code);
        return false;
    }

    if (!x86_prepare_frame(&emit) || !anvil_mir_layout_build(&emit.base.layout, mir)) {
        anvil_mir_layout_destroy(&emit.base.layout);
        anvil_mir_isel_facts_destroy(&emit.base.isel);
        anvil_strbuf_destroy(&emit.base.code);
        free(emit.base.spill_offsets);
        free(emit.base.frame_slot_offsets);
        return false;
    }

    x86_emit_func_header(&emit);
    /* A frame (EBP kept or anything pushed or allocated) set up at the
     * frame block when the blocks before it need none. */
    emit.base.wrapped = (emit.frame_pointer || emit.num_pushes > 0 || emit.base.frame_size + 4 - 4 * emit.num_pushes > 0) &&
                   anvil_mir_isel_frameless_prefix(mir, &emit.base.isel, anvil_mir_frame_block(mir));
    x86_emit_prologue(&emit);

    size_t num_blocks = anvil_mir_num_blocks(mir);
    size_t num_instrs = anvil_mir_num_instrs(mir);
    /* Loop headers start aligned (padding outside the loop is cheap). */
    bool *loop_headers = calloc(num_blocks ? num_blocks : 1, sizeof(*loop_headers));
    if (!loop_headers || !anvil_mir_loop_header_blocks(mir, loop_headers))
        emit.base.failed = true;
    for (size_t b = 0; b < num_blocks && !emit.base.failed && !emit.base.code.failed; b++) {
        if (emit.base.layout.skip[b])
            continue;  /* prints only a jump: branches go where it leads */
        if (loop_headers[b])
            anvil_strbuf_append(&emit.base.code, "\t.p2align 4\n");
        if (!x86_gas_emit_label(&emit.base.code, emit.plat->format->local_label_prefix, mir, (anvil_mir_block_t)b)) {
            emit.base.failed = true;
            break;
        }
        emit.base.block = (anvil_mir_block_t)b;
        if (emit.base.wrapped && b == anvil_mir_frame_block(mir))
            x86_emit_frame_entry(&emit);
        /* A block may be entered from elsewhere: no flags carry in. */
        anvil_print_flags_reset(&emit.base.flags);

        for (size_t i = 0; i < num_instrs && !emit.base.failed && !emit.base.code.failed; i++) {
            anvil_mir_instr_info_t info;
            if (!anvil_mir_get_instr_info(mir, i, &info)) {
                emit.base.failed = true;
                break;
            }
            if (info.block != (anvil_mir_block_t)b)
                continue;
            size_t printed = emit.base.code.len;
            x86_emit_instr(&emit, i, &info);
            anvil_print_flags_after(&emit.base.flags, mir, i, &info, emit.base.code.len != printed);
        }
    }

    if (!emit.base.failed && !emit.base.code.failed && emit.plat->format->cfi)
        anvil_strbuf_append(&emit.base.code, "\t.cfi_endproc\n");
    if (!emit.base.failed && !emit.base.code.failed && !emit.plat->is_macho && !emit.plat->is_coff) {
        const char *prefix = emit.base.symbol_prefix;
        const char *name = anvil_mir_func_name(mir);
        anvil_strbuf_appendf(&emit.base.code, "\t.size %s%s, .-%s%s\n", prefix, name, prefix, name);
    }
    if (!emit.base.failed && !emit.base.code.failed) {
        x86_emit_rodata(&emit);
    }

    free(loop_headers);
    free(emit.fp_pool);
    anvil_mir_isel_facts_destroy(&emit.base.isel);
    anvil_mir_layout_destroy(&emit.base.layout);
    free(emit.base.spill_offsets);
    free(emit.base.frame_slot_offsets);
    if (emit.base.failed || emit.base.code.failed) {
        anvil_strbuf_destroy(&emit.base.code);
        return false;
    }

    *output = anvil_strbuf_detach(&emit.base.code, len);
    return *output != NULL;
}

/* The entry points without a CPU model assume an SSE2 one (pentium4). */
bool anvil_x86_emit_mir_abi(const anvil_mir_func_t *mir, anvil_func_t *func, anvil_abi_t abi, anvil_syntax_t syntax, char **output, size_t *len)
{
    return anvil_x86_emit_mir_features(mir, func, abi, syntax, anvil_cpu_model_features(ANVIL_CPU_X86_PENTIUM4), output, len);
}

bool anvil_x86_emit_mir(const anvil_mir_func_t *mir, char **output, size_t *len)
{
    return anvil_x86_emit_mir_abi(mir, NULL, ANVIL_ABI_SYSV, ANVIL_SYNTAX_GAS, output, len);
}
