#include "../x86_64_internal.h"
#include "../x86_address.h"
#include "anvil/anvil_analysis.h"
#include "../../common/mir_isel.h"
#include "../../common/parallel_move.h"
#include "x86_gas.h"
#include "../../common/gnu_data.h"
#include "../../common/print_flags.h"
#include "x86_sse_vector.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    x86_gas_t base;
    const anvil_x64_abi_desc_t *desc;
    int gpr_save_offsets[16];
    int fpr_save_offsets[16];
    bool has_frame;
} x64_mir_emit_t;


/* The GPRs the emitters below overwrite before they have read all of an
 * instruction's operands or written its result: CL for a variable count,
 * RDX:RAX for division and the high half of a product, and the temporaries
 * of conversions, floating constants and dynamic allocation. */
static uint64_t x64_printer_gpr_scratch(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, const anvil_x64_abi_desc_t *desc,
                                        size_t index, const anvil_mir_instr_info_t *info)
{
    const uint64_t rax = UINT64_C(1) << X64_RAX, rcx = UINT64_C(1) << X64_RCX, rdx = UINT64_C(1) << X64_RDX;
    const uint64_t r10 = UINT64_C(1) << X64_R10, r11 = UINT64_C(1) << X64_R11;
    const anvil_mir_vreg_info_t *def = info->def != ANVIL_MIR_NO_VREG ? anvil_mir_get_vreg_info(mir, info->def) : NULL;
    const anvil_mir_vreg_info_t *source = info->num_uses ? anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, 0)) : NULL;
    int64_t count;
    switch (info->op) {
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
        return def && def->reg_class == ANVIL_MIR_REG_GPR && !x86_gas_facts_imm(facts, false, anvil_mir_get_instr_use(mir, index, 1), &count) ? rcx : 0;
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD:
        return rax | rdx;
    case ANVIL_MIR_OP_SCALAR:
        if (info->ir_op == ANVIL_OP_ROTL)
            return x86_gas_facts_imm(facts, false, anvil_mir_get_instr_use(mir, index, 1), &count) ? 0 : rcx;
        return info->ir_op == ANVIL_OP_UMULH || info->ir_op == ANVIL_OP_SMULH ? rax | rdx : 0;
    case ANVIL_MIR_OP_UITOFP:
        return source && source->size_bits > 32 ? r11 | rax : r11;
    case ANVIL_MIR_OP_FPTOUI:
        return def && def->size_bits > 32 ? rax : 0;
    case ANVIL_MIR_OP_MOV:
        return def && def->reg_class == ANVIL_MIR_REG_FPR && info->has_imm && info->imm != 0 ? r11 : 0;
    case ANVIL_MIR_OP_DYN_ALLOCA:
        return desc->is_win64 ? r11 | rax | r10 : r11;
    default:
        return 0;
    }
}

bool x64_annotate_printer_scratch(anvil_mir_func_t *mir, const anvil_x64_abi_desc_t *desc)
{
    anvil_mir_isel_facts_t facts;
    if (!anvil_mir_isel_facts_build(&facts, mir, NULL, NULL, NULL))
        return false;
    bool ok = true;
    for (size_t index = 0; ok && index < anvil_mir_num_instrs(mir); index++) {
        anvil_mir_instr_info_t info;
        ok = anvil_mir_get_instr_info(mir, index, &info) &&
             anvil_mir_set_instr_scratch(mir, index, ANVIL_MIR_REG_GPR, x64_printer_gpr_scratch(mir, &facts, desc, index, &info));
    }
    anvil_mir_isel_facts_destroy(&facts);
    return ok;
}

static int x64_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0)
        return 8;
    int size = (int)((size_bits + 7) / 8);
    if (size <= 0)
        return 8;
    if (size > 16)
        return 16;
    return size;
}

static const anvil_mir_vreg_info_t *x64_vreg_info_checked(x64_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->base.mir, vreg);
    if (!info)
        emit->base.failed = true;
    return info;
}

static const anvil_regalloc_assignment_t *x64_assignment_checked(x64_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(emit->base.mir, vreg);
    if (!assignment || assignment->spilled || assignment->phys_reg < 0) {
        emit->base.failed = true;
        return NULL;
    }
    return assignment;
}

static const char *x64_reg_name(x64_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = x64_vreg_info_checked(emit, vreg);
    const anvil_regalloc_assignment_t *assignment = x64_assignment_checked(emit, vreg);
    if (!info || !assignment)
        return "?";

    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        if (assignment->phys_reg < 0 || assignment->phys_reg >= 16) {
            emit->base.failed = true;
            return "?";
        }
        return x86_gas_xmm(assignment->phys_reg);
    }

    int size = x64_size_bytes(info->size_bits);
    return x86_gas_gpr(assignment->phys_reg, size);
}

static bool x64_instr_has_call(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info))
            return true;
        if (info.op == ANVIL_MIR_OP_CALL)
            return true;
    }
    return false;
}

static bool x64_scan_outgoing_stack_args(x64_mir_emit_t *emit)
{
    int outgoing_size = 0;
    if (!anvil_mir_outgoing_stack_bytes(emit->base.mir, 8, &outgoing_size))
        return false;
    bool has_call = x64_instr_has_call(emit->base.mir);
    if (!has_call && outgoing_size > 0)
        return false;
    if (has_call) {
        if (outgoing_size > INT_MAX - emit->desc->shadow_space)
            return false;
        outgoing_size += emit->desc->shadow_space;
    }
    emit->base.outgoing_size = anvil_align_int(outgoing_size, 16);
    return true;
}

static bool x64_prepare_frame(x64_mir_emit_t *emit)
{
    for (size_t i = 0; i < 16; i++) {
        emit->gpr_save_offsets[i] = -1;
    }

    if (!x64_scan_outgoing_stack_args(emit))
        return false;

    int offset = 0;
    for (size_t i = 0; i < 16; i++)
        emit->fpr_save_offsets[i] = -1;

    static const int sysv_saved[] = {X64_RBX, X64_R12, X64_R13, X64_R14, X64_R15};
    static const int win64_saved[] = {X64_RBX, X64_RDI, X64_RSI, X64_R12, X64_R13, X64_R14, X64_R15};
    const int *callee_saved = emit->desc->is_win64 ? win64_saved : sysv_saved;
    size_t num_callee_saved = emit->desc->is_win64 ? sizeof(win64_saved) / sizeof(win64_saved[0]) : sizeof(sysv_saved) / sizeof(sysv_saved[0]);
    for (size_t c = 0; c < num_callee_saved; c++) {
        int reg = callee_saved[c];
        bool used = anvil_mir_isel_writes(emit->base.mir, &emit->base.isel, ANVIL_MIR_REG_GPR, reg);
        if (used) {
            offset += 8;
            emit->gpr_save_offsets[reg] = offset;
        }
    }

    if (emit->desc->is_win64) {
        for (int reg = 6; reg <= 15; reg++) {
            bool used = anvil_mir_isel_writes(emit->base.mir, &emit->base.isel, ANVIL_MIR_REG_FPR, reg);
            if (used) {
                offset = anvil_align_int(offset, 16);
                offset += 16;
                emit->fpr_save_offsets[reg] = offset;
            }
        }
    }

    emit->base.num_spill_offsets = anvil_mir_num_spills(emit->base.mir);
    emit->base.num_frame_slot_offsets = anvil_mir_num_frame_slots(emit->base.mir);
    if (emit->base.num_frame_slot_offsets > 0) {
        emit->base.frame_slot_offsets = calloc(emit->base.num_frame_slot_offsets, sizeof(*emit->base.frame_slot_offsets));
        if (!emit->base.frame_slot_offsets)
            return false;
    }

    /* Addressed downward from RBP. */
    if (!anvil_mir_layout_frame_slots(emit->base.mir, offset, 8, emit->desc->stack_alignment, true, emit->base.frame_slot_offsets, &offset))
        return false;

    if (emit->base.num_spill_offsets > 0) {
        emit->base.spill_offsets = calloc(emit->base.num_spill_offsets, sizeof(*emit->base.spill_offsets));
        if (!emit->base.spill_offsets)
            return false;
    }

    if (!anvil_mir_layout_spill_slots(emit->base.mir, offset, 8, 8, true, emit->base.spill_offsets, &offset))
        return false;

    offset += emit->base.outgoing_size;
    emit->base.frame_size = anvil_align_int(offset, 16);
    emit->base.frame_bias = emit->desc->is_win64 ? emit->base.frame_size : 0;
    /* A leaf with no frame bytes runs on the caller's frame (no RBP setup).
     * Win64 keeps its prologue: the unwind directives describe it. */
    emit->has_frame = emit->base.frame_size > 0 || emit->desc->is_win64 || anvil_mir_needs_frame_record(emit->base.mir);
    return true;
}

/* Frame offsets are measured down from the saved RBP slot. Win64 records a
 * frame pointer at the bottom of the fixed allocation so unwind can recover
 * RSP even after a dynamic alloca (SEH frame offsets are limited to 240). */
static int x64_frame_disp(const x64_mir_emit_t *emit, int offset)
{
    return emit->base.frame_bias - offset;
}

/* Probe every crossed guard page before moving RSP. Using inline probes
 * keeps generated GAS independent of MSVC/MinGW's different chkstk symbols.
 * R11 holds the size; RAX/R10 are reserved emitter temporaries. */
static void x64_emit_stack_probe(x64_mir_emit_t *emit, size_t id)
{
    const char *name = anvil_mir_func_name(emit->base.mir);
    anvil_strbuf_append(&emit->base.code, "\tmovq %rsp, %rax\n\tmovq %rsp, %r10\n\tsubq %r11, %r10\n");
    anvil_strbuf_appendf(&emit->base.code, "%s%s_probe_%zu_loop:\n", emit->desc->format->local_label_prefix, name, id);
    anvil_strbuf_append(&emit->base.code, "\tsubq $4096, %rax\n\tcmpq %r10, %rax\n");
    anvil_strbuf_appendf(&emit->base.code, "\tjb %s%s_probe_%zu_done\n", emit->desc->format->local_label_prefix, name, id);
    anvil_strbuf_append(&emit->base.code, "\ttestb $0, (%rax)\n");
    anvil_strbuf_appendf(&emit->base.code, "\tjmp %s%s_probe_%zu_loop\n", emit->desc->format->local_label_prefix, name, id);
    anvil_strbuf_appendf(&emit->base.code, "%s%s_probe_%zu_done:\n", emit->desc->format->local_label_prefix, name, id);
    anvil_strbuf_append(&emit->base.code, "\ttestb $0, (%r10)\n");
}

static void x64_emit_frame_entry(x64_mir_emit_t *emit);

static void x64_emit_prologue(x64_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->base.mir);
    const char *prefix = emit->base.symbol_prefix;
    char symbol[512];
    snprintf(symbol, sizeof(symbol), "%s%s", prefix, name);
    if (emit->desc->is_darwin) {
        anvil_strbuf_append(&emit->base.code, emit->desc->format->text_section);
        anvil_obj_emit_binding(&emit->base.code, emit->desc->format, anvil_mir_func_linkage(emit->base.mir), symbol);
        anvil_strbuf_append(&emit->base.code, "\t.p2align 4\n");
        anvil_strbuf_appendf(&emit->base.code, "%s%s:\n", prefix, name);
    } else if (emit->desc->is_win64) {
        anvil_strbuf_append(&emit->base.code, emit->desc->format->text_section);
        anvil_obj_emit_binding(&emit->base.code, emit->desc->format, anvil_mir_func_linkage(emit->base.mir), symbol);
        anvil_strbuf_appendf(&emit->base.code, "\t.def %s%s; .scl %d; .type 32; .endef\n", prefix, name,
                             anvil_mir_func_linkage(emit->base.mir) == ANVIL_LINK_INTERNAL ? 3 : 2);
        anvil_strbuf_appendf(&emit->base.code, "\t.seh_proc %s%s\n", prefix, name);
        anvil_strbuf_appendf(&emit->base.code, "%s%s:\n", prefix, name);
    } else {
        anvil_strbuf_append(&emit->base.code, emit->desc->format->text_section);
        anvil_obj_emit_binding(&emit->base.code, emit->desc->format, anvil_mir_func_linkage(emit->base.mir), symbol);
        anvil_strbuf_appendf(&emit->base.code, "\t.type %s%s, @function\n", prefix, name);
        anvil_strbuf_appendf(&emit->base.code, "%s%s:\n", prefix, name);
    }

    bool cfi = emit->desc->format->cfi;
    if (cfi)
        anvil_strbuf_append(&emit->base.code, "\t.cfi_startproc\n");
    if (!emit->has_frame || emit->base.wrapped)
        return;
    x64_emit_frame_entry(emit);
}

/* The frame: rbp, locals, saves and va_start's homing (at the entry, or at
 * the frame block). */
static void x64_emit_frame_entry(x64_mir_emit_t *emit)
{
    bool cfi = emit->desc->format->cfi;
    anvil_strbuf_append(&emit->base.code, "\tpushq %rbp\n");
    if (emit->desc->is_win64)
        anvil_strbuf_append(&emit->base.code, "\t.seh_pushreg %rbp\n");
    if (cfi)
        anvil_strbuf_append(&emit->base.code, "\t.cfi_def_cfa_offset 16\n\t.cfi_offset %rbp, -16\n");
    if (!emit->desc->is_win64)
        anvil_strbuf_append(&emit->base.code, "\tmovq %rsp, %rbp\n");
    if (cfi)
        anvil_strbuf_append(&emit->base.code, "\t.cfi_def_cfa_register %rbp\n");
    if (emit->base.frame_size > 0) {
        if (emit->desc->is_win64 && emit->base.frame_size >= 4096) {
            anvil_strbuf_appendf(&emit->base.code, "\tmovq $%d, %%r11\n", emit->base.frame_size);
            x64_emit_stack_probe(emit, SIZE_MAX);
        }
        anvil_strbuf_appendf(&emit->base.code, "\tsubq $%d, %%rsp\n", emit->base.frame_size);
        if (emit->desc->is_win64)
            anvil_strbuf_appendf(&emit->base.code, "\t.seh_stackalloc %d\n", emit->base.frame_size);
    }

    if (emit->desc->is_win64)
        anvil_strbuf_append(&emit->base.code, "\tmovq %rsp, %rbp\n\t.seh_setframe %rbp, 0\n");

    for (int reg = 0; reg < 16; reg++) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->base.code, "\tmovq %%%s, %d(%%rbp)\n", x86_gas_gpr(reg, 8), x64_frame_disp(emit, emit->gpr_save_offsets[reg]));
            if (cfi)
                anvil_strbuf_appendf(&emit->base.code, "\t.cfi_offset %%%s, %d\n", x86_gas_gpr(reg, 8), x64_frame_disp(emit, emit->gpr_save_offsets[reg]) - 16);
            if (emit->desc->is_win64)
                anvil_strbuf_appendf(&emit->base.code, "\t.seh_savereg %%%s, %d\n", x86_gas_gpr(reg, 8), emit->base.frame_size - emit->gpr_save_offsets[reg]);
        }
    }
    for (int reg = 6; reg < 16; reg++) {
        if (emit->fpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->base.code, "\tmovdqu %%xmm%d, %d(%%rbp)\n", reg, x64_frame_disp(emit, emit->fpr_save_offsets[reg]));
            anvil_strbuf_appendf(&emit->base.code, "\t.seh_savexmm %%xmm%d, %d\n", reg, emit->base.frame_size - emit->fpr_save_offsets[reg]);
        }
    }
    if (emit->desc->is_win64)
        anvil_strbuf_append(&emit->base.code, "\t.seh_endprologue\n");

    /* Home the incoming register bits before parameter copies or generated
     * instructions can overwrite them. Win64 variadic FP arguments also arrive
     * in these GPRs, so the cursor sees one contiguous sequence of 8-byte slots. */
    for (size_t index = 0; index < anvil_mir_num_instrs(emit->base.mir); index++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->base.mir, index, &info)) {
            emit->base.failed = true;
            return;
        }
        if (info.op != ANVIL_MIR_OP_VA_START)
            continue;

        if (!emit->desc->is_win64) {
            if (info.frame_slot < 0 || (size_t)info.frame_slot >= emit->base.num_frame_slot_offsets) {
                emit->base.failed = true;
                return;
            }

            int save = -emit->base.frame_slot_offsets[info.frame_slot] + 32;
            const char *registers[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
            for (int slot = 0; slot < 6; slot++)
                anvil_strbuf_appendf(&emit->base.code, "\tmovq %%%s, %d(%%rbp)\n", registers[slot], save + slot * 8);

            for (int slot = 0; slot < 8; slot++)
                anvil_strbuf_appendf(&emit->base.code, "\tmovups %%xmm%d, %d(%%rbp)\n", slot, save + 48 + slot * 16);

            continue;
        }

        if (!emit->desc->is_win64 || emit->base.frame_size > INT_MAX - 40) {
            emit->base.failed = true;
            return;
        }
        const char *registers[] = {"rcx", "rdx", "r8", "r9"};
        for (int slot = 0; slot < 4; slot++)
            anvil_strbuf_appendf(&emit->base.code, "\tmovq %%%s, %d(%%rbp)\n", registers[slot], emit->base.frame_size + 16 + slot * 8);

        break;
    }
}

static void x64_emit_epilogue(x64_mir_emit_t *emit)
{
    bool cfi = emit->desc->format->cfi;
    if (!emit->has_frame) {
        anvil_strbuf_append(&emit->base.code, "\tret\n");
        return;
    }
    if (cfi)
        anvil_strbuf_append(&emit->base.code, "\t.cfi_remember_state\n");
    for (int reg = 15; reg >= 6; reg--) {
        if (emit->fpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->base.code, "\tmovdqu %d(%%rbp), %%xmm%d\n", x64_frame_disp(emit, emit->fpr_save_offsets[reg]), reg);
        }
    }
    for (int reg = 15; reg >= 0; reg--) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->base.code, "\tmovq %d(%%rbp), %%%s\n", x64_frame_disp(emit, emit->gpr_save_offsets[reg]), x86_gas_gpr(reg, 8));
        }
    }
    if (emit->desc->is_win64)
        anvil_strbuf_appendf(&emit->base.code, "\tleaq %d(%%rbp), %%rsp\n", emit->base.frame_size);
    else
        anvil_strbuf_append(&emit->base.code, "\tmovq %rbp, %rsp\n");
    anvil_strbuf_append(&emit->base.code, "\tpopq %rbp\n");
    if (cfi)
        anvil_strbuf_append(&emit->base.code, "\t.cfi_def_cfa %rsp, 8\n");
    anvil_strbuf_append(&emit->base.code, "\tret\n");
    if (cfi)
        anvil_strbuf_append(&emit->base.code, "\t.cfi_restore_state\n");
}

/* The printer's XMM temporaries (see anvil_x64_abi_desc_t.sse_temps). */
static const char *x64_sse_temp(const x64_mir_emit_t *emit)
{
    return x86_gas_xmm(emit->desc->sse_temps[0]);
}

static const char *x64_sse_temp2(const x64_mir_emit_t *emit)
{
    return x86_gas_xmm(emit->desc->sse_temps[1]);
}

static const char *x64_fp_mov_op(int size)
{
    if (size == 16)
        return "movups";

    return size <= 4 ? "movss" : "movsd";
}

/* An FP register to or from memory (the shared spill and stack-argument
 * printers' hooks). */
static void x64_fp_move(x86_gas_t *gas, anvil_mir_vreg_t vreg, const char *mem, bool load)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(gas->mir, vreg);
    int phys = x86_gas_phys_of(gas, vreg);
    if (!info || gas->failed) {
        gas->failed = true;
        return;
    }
    const char *op = x64_fp_mov_op(x64_size_bytes(info->size_bits));
    if (load)
        anvil_strbuf_appendf(&gas->code, "\t%s %s, %%%s\n", op, mem, x86_gas_xmm(phys));
    else
        anvil_strbuf_appendf(&gas->code, "\t%s %%%s, %s\n", op, x86_gas_xmm(phys), mem);
}

static void x64_fp_compare(void *user, anvil_mir_vreg_t a, anvil_mir_vreg_t b)
{
    x86_gas_t *gas = user;
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(gas->mir, a);
    int a_phys = x86_gas_phys_of(gas, a), b_phys = x86_gas_phys_of(gas, b);
    if (!info || gas->failed) {
        gas->failed = true;
        return;
    }
    anvil_strbuf_appendf(&gas->code, "\t%s %%%s, %%%s\n", x64_size_bytes(info->size_bits) <= 4 ? "ucomiss" : "ucomisd", x86_gas_xmm(b_phys), x86_gas_xmm(a_phys));
}

static void x64_fp_load(void *gas, anvil_mir_vreg_t vreg, const char *mem)
{
    x64_fp_move(gas, vreg, mem, true);
}

static void x64_fp_store(void *gas, anvil_mir_vreg_t vreg, const char *mem)
{
    x64_fp_move(gas, vreg, mem, false);
}

/* Register to register: movss/movsd between registers merge into the
 * destination's low lane, so they depend on its old value (a loop-carried
 * chain when the register was last written in the previous iteration);
 * movaps writes it whole. */
static const char *x64_fp_copy_op(void)
{
    return "movaps";
}

/* FPR <- FPR, and the bit moves between an FPR and a GPR. */
static void x64_fp_copy(void *user, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    x64_mir_emit_t *emit = user;
    const anvil_mir_vreg_info_t *dst_info = x64_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = x64_vreg_info_checked(emit, src);
    int dst_phys = x86_gas_phys_of(&emit->base, dst);
    int src_phys = x86_gas_phys_of(&emit->base, src);
    if (!dst_info || !src_info || emit->base.failed)
        return;
    if (dst_info->reg_class == ANVIL_MIR_REG_FPR && src_info->reg_class == ANVIL_MIR_REG_FPR) {
        if (dst_phys != src_phys)
            anvil_strbuf_appendf(&emit->base.code, "\t%s %%%s, %%%s\n", x64_fp_copy_op(), x86_gas_xmm(src_phys), x86_gas_xmm(dst_phys));
    } else if (dst_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x64_size_bytes(dst_info->size_bits);
        anvil_strbuf_appendf(&emit->base.code, "\t%s %%%s, %%%s\n", size <= 4 ? "movd" : "movq", x86_gas_gpr(src_phys, size <= 4 ? 4 : 8), x86_gas_xmm(dst_phys));
    } else {
        int size = x64_size_bytes(src_info->size_bits);
        anvil_strbuf_appendf(&emit->base.code, "\t%s %%%s, %%%s\n", size <= 4 ? "movd" : "movq", x86_gas_xmm(src_phys), x86_gas_gpr(dst_phys, size <= 4 ? 4 : 8));
    }
}

/* An FP constant through R11 (the printer's temporary). */
static void x64_fp_constant(void *user, anvil_mir_vreg_t dst, uint64_t bits)
{
    x64_mir_emit_t *emit = user;
    const anvil_mir_vreg_info_t *info = x64_vreg_info_checked(emit, dst);
    int dst_phys = x86_gas_phys_of(&emit->base, dst);
    if (!info || emit->base.failed)
        return;
    const char *xmm = x86_gas_xmm(dst_phys);
    if (bits == 0)
        anvil_strbuf_appendf(&emit->base.code, "\txorps %%%s, %%%s\n", xmm, xmm);
    else if (x64_size_bytes(info->size_bits) <= 4)
        anvil_strbuf_appendf(&emit->base.code, "\tmovl $%u, %%r11d\n\tmovd %%r11d, %%%s\n", (uint32_t)bits, xmm);
    else
        anvil_strbuf_appendf(&emit->base.code, "\tmovabsq $%llu, %%r11\n\tmovq %%r11, %%%s\n", (unsigned long long)bits, xmm);
}

/* Division in RDX:RAX, which are this instruction's scratch: no operand
 * is there. */
static void x64_gpr_divide(void *user, size_t index, const anvil_mir_instr_info_t *info, int dst_phys, int a_phys, int b_phys, int size)
{
    x64_mir_emit_t *emit = user;
    (void)index;
    const char *suf = x86_gas_size_suffix(size);
    const char *rax = x86_gas_gpr(X64_RAX, size);
    const char *rdx = x86_gas_gpr(X64_RDX, size);
    const char *a = x86_gas_gpr(a_phys, size);
    const char *b = x86_gas_gpr(b_phys, size);
    const char *dst = x86_gas_gpr(dst_phys, size);
    switch (info->op) {
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD: {
        bool is_signed = info->op == ANVIL_MIR_OP_SDIV || info->op == ANVIL_MIR_OP_SMOD;
        bool is_mod = info->op == ANVIL_MIR_OP_SMOD || info->op == ANVIL_MIR_OP_UMOD;
        /* RDX:RAX are this instruction's scratch: no operand is there. */
        anvil_strbuf_appendf(&emit->base.code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
        if (size == 1) {
            if (is_signed) {
                anvil_strbuf_append(&emit->base.code, "\tcbtw\n");
                anvil_strbuf_appendf(&emit->base.code, "\tidivb %%%s\n", b);
            } else {
                anvil_strbuf_append(&emit->base.code, "\tmovzbw %al, %ax\n");
                anvil_strbuf_appendf(&emit->base.code, "\tdivb %%%s\n", b);
            }
            if (is_mod) {
                anvil_strbuf_append(&emit->base.code, "\tshrw $8, %ax\n");
            }
            anvil_strbuf_appendf(&emit->base.code, "\tmovb %%al, %%%s\n", dst);
        } else {
            if (is_signed) {
                const char *ext = size == 2 ? "\tcwtd\n" : size == 4 ? "\tcltd\n" : "\tcqto\n";
                anvil_strbuf_append(&emit->base.code, ext);
                anvil_strbuf_appendf(&emit->base.code, "\tidiv%s %%%s\n", suf, b);
            } else {
                anvil_strbuf_appendf(&emit->base.code, "\txorl %%edx, %%edx\n");
                anvil_strbuf_appendf(&emit->base.code, "\tdiv%s %%%s\n", suf, b);
            }
            anvil_strbuf_appendf(&emit->base.code, "\tmov%s %%%s, %%%s\n", suf, is_mod ? rdx : rax, dst);
        }
        break;
    }
    default:
        emit->base.failed = true;
        break;
    }
}

/* The shared SSE printer over this function's registers and temps. */
static x86_sse_printer_t x64_sse_printer(x64_mir_emit_t *emit)
{
    return (x86_sse_printer_t){
        .code = &emit->base.code,
        .mir = emit->base.mir,
        .num_xmm = X64_NUM_FPR,
        .gpr64 = true,
        .temp = x64_sse_temp(emit),
        .temp2 = x64_sse_temp2(emit),
    };
}

static void x64_emit_sse_destructive(x64_mir_emit_t *emit, const char *mnemonic, anvil_mir_vreg_t def, anvil_mir_vreg_t lhs,
                                     anvil_mir_vreg_t rhs, bool commutative)
{
    x86_sse_printer_t printer = x64_sse_printer(emit);
    x86_sse_emit_destructive(&printer, mnemonic, def, lhs, rhs, commutative);
    emit->base.failed |= printer.failed;
}

/* A lane-wise operation, or a scalar one the table prices: the general
 * register ones here, the SSE ones through the shared printer. */
static void x64_emit_vector(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    const anvil_vector_isa_t *isa =
        anvil_vector_isa_for_mir(x86_sse_isa, x86_sse_isa_count, emit->base.mir, instr_index, info, anvil_mir_func_cpu_features(emit->base.mir));
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
    anvil_mir_vreg_t rhs = info->num_uses > 1 ? anvil_mir_get_instr_use(emit->base.mir, instr_index, 1) : ANVIL_MIR_NO_VREG;
    if (!isa || lhs == ANVIL_MIR_NO_VREG) {
        emit->base.failed = true;
        return;
    }
    if (info->ir_op == ANVIL_OP_ROTL) {
        /* The count into CL first (dst may hold it), then rotate a copy. */
        int size = info->imm == 64 ? 8 : 4;
        const char *a = x86_gas_gpr(x86_gas_phys_of(&emit->base, lhs), size), *dst = x86_gas_gpr(x86_gas_phys_of(&emit->base, info->def), size);
        int64_t count;
        bool immediate = x86_gas_known_imm(&emit->base, rhs, &count);
        if (!immediate)
            anvil_strbuf_appendf(&emit->base.code, "\tmovl %%%s, %%ecx\n", x86_gas_gpr(x86_gas_phys_of(&emit->base, rhs), 4));
        if (emit->base.failed)
            return;
        if (strcmp(a, dst) != 0)
            anvil_strbuf_appendf(&emit->base.code, "\tmov%s %%%s, %%%s\n", x86_gas_size_suffix(size), a, dst);
        if (immediate)
            anvil_strbuf_appendf(&emit->base.code, "\t%s $%d, %%%s\n", isa->mnemonic, (int)(count & (size * 8 - 1)), dst);
        else
            anvil_strbuf_appendf(&emit->base.code, "\t%s %%cl, %%%s\n", isa->mnemonic, dst);
        return;
    }
    if (info->ir_op == ANVIL_OP_UMULH || info->ir_op == ANVIL_OP_SMULH) {
        /* RAX and RDX are emitter temporaries: a into RAX, the high half
         * from RDX. */
        int size = info->imm == 64 ? 8 : 4;
        const char *a = x86_gas_gpr(x86_gas_phys_of(&emit->base, lhs), size), *b = x86_gas_gpr(x86_gas_phys_of(&emit->base, rhs), size);
        const char *dst = x86_gas_gpr(x86_gas_phys_of(&emit->base, info->def), size);
        if (!emit->base.failed)
            anvil_strbuf_appendf(&emit->base.code, "\tmov%s %%%s, %%%s\n\t%s %%%s\n\tmov%s %%%s, %%%s\n", x86_gas_size_suffix(size), a,
                                 x86_gas_gpr(X64_RAX, size), isa->mnemonic, b, x86_gas_size_suffix(size), x86_gas_gpr(X64_RDX, size), dst);
        return;
    }
    x86_sse_printer_t printer = x64_sse_printer(emit);
    x86_sse_emit_vector(&printer, isa, instr_index, info);
    emit->base.failed |= printer.failed;
}

static void x64_fp_binary(void *user, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    x64_mir_emit_t *emit = user;
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->base.mir, def);
    bool single = info && x64_size_bytes(info->size_bits) <= 4;
    const char *mnemonic = op == ANVIL_MIR_OP_ADD ? (single ? "addss" : "addsd") : op == ANVIL_MIR_OP_SUB ? (single ? "subss" : "subsd")
                         : op == ANVIL_MIR_OP_MUL ? (single ? "mulss" : "mulsd") : op == ANVIL_MIR_OP_DIV || op == ANVIL_MIR_OP_FDIV ? (single ? "divss" : "divsd")
                                                                                                                                   : NULL;
    if (!info || !mnemonic) {
        emit->base.failed = true;
        return;
    }
    x64_emit_sse_destructive(emit, mnemonic, def, lhs, rhs, op == ANVIL_MIR_OP_ADD || op == ANVIL_MIR_OP_MUL);
}

static void x64_emit_cond_branch(x64_mir_emit_t *emit, const char *cc, anvil_mir_block_t true_block, anvil_mir_block_t false_block)
{
    emit->base.failed |= !x86_gas_emit_cond_branch(&emit->base.code, emit->desc->format->local_label_prefix, emit->base.mir, &emit->base.layout, emit->base.block, cc, true_block,
                                              false_block);
}

/* NEG and FABS: the sign bit flipped or cleared through the function's
 * mask constants. */
static void x64_fp_unary(void *user, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t src)
{
    x64_mir_emit_t *emit = user;
    const anvil_mir_vreg_info_t *info = x64_vreg_info_checked(emit, def);
    int dst_phys = x86_gas_phys_of(&emit->base, def), src_phys = x86_gas_phys_of(&emit->base, src);
    if (!info || emit->base.failed)
        return;
    bool single = x64_size_bytes(info->size_bits) <= 4, neg = op == ANVIL_MIR_OP_NEG;
    const char *dst = x86_gas_xmm(dst_phys);
    anvil_strbuf_appendf(&emit->base.code, "\t%s %%%s, %%%s\n", x64_fp_copy_op(), x86_gas_xmm(src_phys), dst);
    anvil_strbuf_appendf(&emit->base.code, "\t%s %s%s_%s_mask%s(%%rip), %%%s\n", neg ? (single ? "xorps" : "xorpd") : (single ? "andps" : "andpd"),
                         emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->base.mir), neg ? "fneg" : "fabs", single ? "32" : "64", dst);
    if (neg)
        emit->base.emitted_fneg_mask = true;
    else
        emit->base.emitted_fabs_mask = true;
}

static void x64_emit_cast(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->base.failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = x64_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = x64_vreg_info_checked(emit, src);
    if (!dst_info || !src_info)
        return;

    switch (info->op) {
    case ANVIL_MIR_OP_ZEXT:
        x86_gas_emit_gpr_extend(&emit->base, instr_index, info, false);
        break;
    case ANVIL_MIR_OP_SEXT:
        x86_gas_emit_gpr_extend(&emit->base, instr_index, info, true);
        break;
    case ANVIL_MIR_OP_TRUNC:
    case ANVIL_MIR_OP_BITCAST:
        x86_gas_emit_copy(&emit->base, info->def, src);
        break;
    case ANVIL_MIR_OP_SITOFP: {
        if (dst_info->reg_class != ANVIL_MIR_REG_FPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
            emit->base.failed = true;
            return;
        }
        int dst_size = x64_size_bytes(dst_info->size_bits);
        int src_size = x64_size_bytes(src_info->size_bits);
        if (src_size < 4)
            src_size = 4;
        const char *dst = x64_reg_name(emit, info->def);
        const char *s = x86_gas_gpr(x86_gas_phys_of(&emit->base, src), src_size);
        if (emit->base.failed)
            return;
        /* Conversions into an XMM register merge into its upper bits:
         * zeroing it first breaks the dependency on its old value. */
        anvil_strbuf_appendf(&emit->base.code, "\txorps %%%s, %%%s\n", dst, dst);
        anvil_strbuf_appendf(&emit->base.code, "\tcvtsi2s%s%s %%%s, %%%s\n", dst_size <= 4 ? "s" : "d", src_size <= 4 ? "l" : "q", s, dst);
        break;
    }
    case ANVIL_MIR_OP_UITOFP: {
        if (dst_info->reg_class != ANVIL_MIR_REG_FPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
            emit->base.failed = true;
            return;
        }
        int dst_size = x64_size_bytes(dst_info->size_bits);
        int src_size = x64_size_bytes(src_info->size_bits);
        const char *dst = x64_reg_name(emit, info->def);
        if (emit->base.failed)
            return;
        anvil_strbuf_appendf(&emit->base.code, "\txorps %%%s, %%%s\n", dst, dst);
        if (src_size <= 4) {
            const char *s = x86_gas_gpr(x86_gas_phys_of(&emit->base, src), 4);
            anvil_strbuf_appendf(&emit->base.code, "\tmovl %%%s, %%r11d\n", s);
            anvil_strbuf_appendf(&emit->base.code, "\tcvtsi2s%sq %%r11, %%%s\n", dst_size <= 4 ? "s" : "d", dst);
        } else {
            /* Values with bit 63 set convert at half scale, keeping the low
             * bit for rounding, and are then doubled. */
            const char *s = x86_gas_gpr(x86_gas_phys_of(&emit->base, src), 8);
            const char *sd = dst_size <= 4 ? "s" : "d";
            anvil_strbuf_appendf(&emit->base.code, "\ttestq %%%s, %%%s\n\tjs 1f\n", s, s);
            anvil_strbuf_appendf(&emit->base.code, "\tcvtsi2s%sq %%%s, %%%s\n\tjmp 2f\n", sd, s, dst);
            anvil_strbuf_appendf(&emit->base.code, "1:\tmovq %%%s, %%r11\n\tshrq %%r11\n", s);
            anvil_strbuf_appendf(&emit->base.code, "\tmovq %%%s, %%rax\n\tandl $1, %%eax\n\torq %%rax, %%r11\n", s);
            anvil_strbuf_appendf(&emit->base.code, "\tcvtsi2s%sq %%r11, %%%s\n\tadds%s %%%s, %%%s\n2:\n", sd, dst, sd, dst, dst);
        }
        break;
    }
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI: {
        if (dst_info->reg_class != ANVIL_MIR_REG_GPR || src_info->reg_class != ANVIL_MIR_REG_FPR) {
            emit->base.failed = true;
            return;
        }
        int src_size = x64_size_bytes(src_info->size_bits);
        int dst_size = x64_size_bytes(dst_info->size_bits);
        if (dst_size < 4)
            dst_size = 4;
        const char *s = x64_reg_name(emit, src);
        const char *sd = src_size <= 4 ? "s" : "d";
        bool is_unsigned = info->op == ANVIL_MIR_OP_FPTOUI;
        /* An unsigned 32-bit result is the low half of the 64-bit one. */
        const char *dst = x86_gas_gpr(x86_gas_phys_of(&emit->base, info->def), dst_size <= 4 && !is_unsigned ? 4 : 8);
        if (emit->base.failed)
            return;
        if (!is_unsigned || dst_size <= 4) {
            anvil_strbuf_appendf(&emit->base.code, "\tcvtts%s2si %%%s, %%%s\n", sd, s, dst);
            break;
        }
        /* From 2^63 up: convert x - 2^63 and set bit 63.  Both steps are
         * exact in that range, so the source is restored afterwards. */
        const char *limit = x64_sse_temp(emit);
        if (src_size <= 4)
            anvil_strbuf_appendf(&emit->base.code, "\tmovl $0x5f000000, %%eax\n\tmovd %%eax, %%%s\n", limit);
        else
            anvil_strbuf_appendf(&emit->base.code, "\tmovabsq $0x43e0000000000000, %%rax\n\tmovq %%rax, %%%s\n", limit);
        anvil_strbuf_appendf(&emit->base.code, "\tucomis%s %%%s, %%%s\n\tjae 1f\n", sd, limit, s);
        anvil_strbuf_appendf(&emit->base.code, "\tcvtts%s2si %%%s, %%%s\n\tjmp 2f\n", sd, s, dst);
        anvil_strbuf_appendf(&emit->base.code, "1:\tsubs%s %%%s, %%%s\n\tcvtts%s2si %%%s, %%%s\n", sd, limit, s, sd, s, dst);
        anvil_strbuf_appendf(&emit->base.code, "\tadds%s %%%s, %%%s\n\tbtcq $63, %%%s\n2:\n", sd, limit, s, dst);
        break;
    }
    case ANVIL_MIR_OP_FPEXT: {
        const char *dst = x64_reg_name(emit, info->def);
        const char *s = x64_reg_name(emit, src);
        if (emit->base.failed)
            return;
        if (strcmp(s, dst) != 0)
            anvil_strbuf_appendf(&emit->base.code, "\txorps %%%s, %%%s\n", dst, dst);
        anvil_strbuf_appendf(&emit->base.code, "\tcvtss2sd %%%s, %%%s\n", s, dst);
        break;
    }
    case ANVIL_MIR_OP_FPTRUNC: {
        const char *dst = x64_reg_name(emit, info->def);
        const char *s = x64_reg_name(emit, src);
        if (emit->base.failed)
            return;
        if (strcmp(s, dst) != 0)
            anvil_strbuf_appendf(&emit->base.code, "\txorps %%%s, %%%s\n", dst, dst);
        anvil_strbuf_appendf(&emit->base.code, "\tcvtsd2ss %%%s, %%%s\n", s, dst);
        break;
    }
    default:
        emit->base.failed = true;
        break;
    }
}

static void x64_emit_dyn_alloca(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
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

    const char *count_reg = x86_gas_gpr(x86_gas_phys_of(&emit->base, count), 8);
    int dst_phys = x86_gas_phys_of(&emit->base, info->def);
    if (emit->base.failed)
        return;

    anvil_strbuf_appendf(&emit->base.code, "\tmovq %%%s, %%r11\n", count_reg);
    if (info->imm != 1) {
        anvil_strbuf_appendf(&emit->base.code, "\timulq $%lld, %%r11, %%r11\n", (long long)info->imm);
    }
    anvil_strbuf_append(&emit->base.code, "\taddq $15, %r11\n");
    anvil_strbuf_append(&emit->base.code, "\tandq $-16, %r11\n");
    /* Relocate the outgoing area below each dynamic allocation. Otherwise
     * CALL_STACK_ARG (and Win64 callee home stores) overwrite the buffer. */
    if (emit->base.outgoing_size)
        anvil_strbuf_appendf(&emit->base.code, "\taddq $%d, %%r11\n", emit->base.outgoing_size);
    if (emit->desc->is_win64)
        x64_emit_stack_probe(emit, instr_index);
    anvil_strbuf_append(&emit->base.code, "\tsubq %r11, %rsp\n");
    anvil_strbuf_appendf(&emit->base.code, "\tleaq %d(%%rsp), %%%s\n", emit->base.outgoing_size, x86_gas_gpr(dst_phys, 8));
}

static void x64_emit_symbol_addr(x64_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->symbol || !info->symbol[0]) {
        emit->base.failed = true;
        return;
    }

    int dst_phys = x86_gas_phys_of(&emit->base, info->def);
    if (emit->base.failed)
        return;

    const char *prefix = x86_gas_symbol_ref_prefix(&emit->base, info->symbol);
    const char *dst = x86_gas_gpr(dst_phys, 8);
    if (info->symbol_got) {
        /* The GOT slot holds the symbol's final address. */
        anvil_strbuf_appendf(&emit->base.code, "\tmovq %s%s@GOTPCREL(%%rip), %%%s\n", prefix, info->symbol, dst);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->base.code, "\tleaq %lld(%%%s), %%%s\n", (long long)info->imm, dst, dst);
        return;
    }
    if (info->symbol && info->symbol[0] == '.') {
        anvil_strbuf_appendf(&emit->base.code, "\tleaq %s%s", prefix, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->base.code, "%+lld", (long long)info->imm);
        anvil_strbuf_appendf(&emit->base.code, "(%%rip), %%%s\n", dst);
    } else {
        anvil_strbuf_appendf(&emit->base.code, "\tleaq %s%s", prefix, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->base.code, "%+lld", (long long)info->imm);
        anvil_strbuf_appendf(&emit->base.code, "(%%rip), %%%s\n", dst);
    }
}

static const char *x64_mem_load_op(anvil_mir_reg_class_t reg_class, int size, bool is_signed, int dst_size)
{
    if (reg_class == ANVIL_MIR_REG_FPR) {
        return x64_fp_mov_op(size);
    }
    if (is_signed) {
        switch (size) {
        case 1:
            return dst_size <= 4 ? "movsbl" : "movsbq";
        case 2:
            return dst_size <= 4 ? "movswl" : "movswq";
        case 4:
            return dst_size <= 4 ? "movl" : "movslq";
        default:
            return "movq";
        }
    }
    switch (size) {
    case 1:
        return "movzbl";
    case 2:
        return "movzwl";
    case 4:
        return "movl";
    default:
        return "movq";
    }
}

/* The x86-64 mode of the shared printer (`user`: the ABI, NULL for the
 * registerless marking). */
static void x64_mode(x86_gas_t *gas, const void *user)
{
    const anvil_x64_abi_desc_t *desc = user;
    gas->pointer_bytes = 8;
    gas->symbol_prefix = desc ? desc->sym_prefix : "";
    gas->wraps_imm32 = false;
    gas->fp_memory_operands = true;
    gas->stack_reg = "rsp";
    gas->frame_reg = "rbp";
    gas->min_gpr_move = 1;
    gas->min_operation_bytes = 1;
    gas->outgoing_base = desc ? desc->shadow_space : 0;
    gas->fp_load = x64_fp_load;
    gas->fp_store = x64_fp_store;
    gas->fp_compare = x64_fp_compare;
    gas->fp_binary = x64_fp_binary;
    gas->gpr_divide = x64_gpr_divide;
    gas->fp_copy = x64_fp_copy;
    gas->fp_constant = x64_fp_constant;
    gas->fp_unary = x64_fp_unary;
    gas->local_label_prefix = desc ? desc->format->local_label_prefix : "";
}

bool anvil_x64_mark_registerless(anvil_mir_func_t *mir)
{
    return x86_gas_mark_registerless(mir, x64_mode, NULL);
}

static void x64_emit_incoming_stack_arg(x64_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm < 0 || info->imm > INT32_MAX - 32) {
        emit->base.failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    int def_phys = x86_gas_phys_of(&emit->base, info->def);
    if (!def_info || emit->base.failed)
        return;

    int size = x64_size_bytes(def_info->size_bits);
    int shadow = emit->desc->shadow_space;
    int frame_offset = 16 + shadow + (int)info->imm;
    if (emit->desc->is_win64)
        frame_offset += emit->base.frame_size;
    const char *op = x64_mem_load_op(def_info->reg_class, size, def_info->is_signed, size);
    const char *dst;
    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        dst = x86_gas_xmm(def_phys);
    } else if (!def_info->is_signed && size < 4) {
        dst = x86_gas_gpr(def_phys, 4);
    } else if (def_info->is_signed && size <= 2) {
        dst = x86_gas_gpr(def_phys, 4);
    } else {
        dst = x86_gas_gpr(def_phys, size);
    }
    anvil_strbuf_appendf(&emit->base.code, "\t%s %d(%%rbp), %%%s\n", op, frame_offset, dst);
}

static void x64_emit_call(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if ((info->call_cc == ANVIL_CC_WIN64 && !emit->desc->is_win64) || (info->call_cc == ANVIL_CC_SYSV && emit->desc->is_win64) || (info->call_cc != ANVIL_CC_SYSV && info->call_cc != ANVIL_CC_WIN64)) {
        emit->base.failed = true;
        return;
    }
    bool direct = info->symbol && info->symbol[0];
    if (!emit->desc->is_win64 && info->has_imm) {
        if (info->imm < 0 || info->imm > 8) {
            emit->base.failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->base.code, "\tmovb $%lld, %%al\n", (long long)info->imm);
    }

    if (direct) {
        anvil_strbuf_appendf(&emit->base.code, "\tcall %s%s%s\n", x86_gas_symbol_ref_prefix(&emit->base, info->symbol), info->symbol,
                             info->symbol_got && emit->desc->plt_suffix ? emit->desc->plt_suffix : "");
        return;
    }

    anvil_mir_vreg_t target = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
    if (target == ANVIL_MIR_NO_VREG) {
        emit->base.failed = true;
        return;
    }
    const char *target_reg = x86_gas_gpr(x86_gas_phys_of(&emit->base, target), 8);
    if (emit->base.failed)
        return;
    anvil_strbuf_appendf(&emit->base.code, "\tcall *%%%s\n", target_reg);
}

static void x64_emit_ret(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (info->num_uses > 0) {
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
        const anvil_mir_vreg_info_t *ret_info = x64_vreg_info_checked(emit, ret);
        int ret_phys = x86_gas_phys_of(&emit->base, ret);
        if (!ret_info || emit->base.failed)
            return;

        if (ret_info->reg_class == ANVIL_MIR_REG_FPR) {
            if (ret_phys != emit->desc->fp_ret_reg) {
                anvil_strbuf_appendf(&emit->base.code, "\t%s %%%s, %%%s\n", x64_fp_copy_op(), x86_gas_xmm(ret_phys), x86_gas_xmm(emit->desc->fp_ret_reg));
            }
        } else {
            int size = x64_size_bytes(ret_info->size_bits);
            if (size < 4)
                size = 4;
            if (ret_phys != emit->desc->int_ret_reg) {
                anvil_strbuf_appendf(&emit->base.code, "\tmov%s %%%s, %%%s\n", x86_gas_size_suffix(size), x86_gas_gpr(ret_phys, size), x86_gas_gpr(emit->desc->int_ret_reg, size));
            }
        }
    }
    if (emit->base.failed)
        return;
    /* Before the frame block nothing was saved: a plain return. */
    if (emit->base.wrapped && emit->base.block < anvil_mir_frame_block(emit->base.mir)) {
        anvil_strbuf_append(&emit->base.code, "\tret\n");
        return;
    }
    x64_emit_epilogue(emit);
}

/* Pointer -> RDX, operand -> RCX, expected -> RAX (CMPXCHG: expected in
 * RAX, desired in RCX), as one parallel move. */
static void x64_atomic_inputs(x64_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info)
{
    static const anvil_parallel_move_ops_t ops = { x86_gas_parallel_move, x86_gas_parallel_swap, -1 };
    int destinations[] = {X64_RDX, X64_RCX, X64_RAX};
    if (info->ir_op == ANVIL_OP_ATOMIC_CMPXCHG) {
        destinations[1] = X64_RAX;
        destinations[2] = X64_RCX;
    }

    int sources[3];
    size_t count = info->num_uses < 3 ? info->num_uses : 3;
    for (size_t operand = 0; operand < count; operand++)
        sources[operand] = x86_gas_phys_of(&emit->base, anvil_mir_get_instr_use(emit->base.mir, index, operand));
    if (!emit->base.failed && !anvil_parallel_move(&ops, emit, destinations, sources, count))
        emit->base.failed = true;
}

static void x64_emit_atomic(x64_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info)
{
    if (info->ir_op == ANVIL_OP_ATOMIC_FENCE) {
        if (info->atomic.order == ANVIL_ORDER_SEQ_CST)
            anvil_strbuf_append(&emit->base.code, "\tmfence\n");

        return;
    }

    anvil_mir_vreg_t value = info->def;
    if (value == ANVIL_MIR_NO_VREG)
        value = anvil_mir_get_instr_use(emit->base.mir, index, 1);

    const anvil_mir_vreg_info_t *type = x64_vreg_info_checked(emit, value);
    if (!type)
        return;

    int size = x64_size_bytes(type->size_bits);
    const char *suffix = x86_gas_size_suffix(size);
    const char *accumulator = x86_gas_gpr(X64_RAX, size);
    const char *operand = x86_gas_gpr(X64_RCX, size);
    const char *temporary = x86_gas_gpr(X64_R11, size);
    x64_atomic_inputs(emit, index, info);
    if (emit->base.failed)
        return;

    switch (info->ir_op) {
    case ANVIL_OP_ATOMIC_LOAD:
        anvil_strbuf_appendf(&emit->base.code, "\tmov%s (%%rdx), %%%s\n", suffix, accumulator);
        break;
    case ANVIL_OP_ATOMIC_STORE:
        anvil_strbuf_appendf(&emit->base.code, "\t%s%s %%%s, (%%rdx)\n", info->atomic.order == ANVIL_ORDER_SEQ_CST ? "xchg" : "mov", suffix, operand);
        return;
    case ANVIL_OP_ATOMIC_CMPXCHG:
        anvil_strbuf_appendf(&emit->base.code, "\tlock cmpxchg%s %%%s, (%%rdx)\n", suffix, operand);
        break;
    case ANVIL_OP_ATOMIC_RMW:
        if (info->atomic.rmw <= ANVIL_ATOMIC_SUB) {
            if (info->atomic.rmw == ANVIL_ATOMIC_SUB)
                anvil_strbuf_appendf(&emit->base.code, "\tneg%s %%%s\n", suffix, operand);

            const char *operation = info->atomic.rmw == ANVIL_ATOMIC_EXCHANGE ? "xchg" : "lock xadd";
            anvil_strbuf_appendf(&emit->base.code, "\t%s%s %%%s, (%%rdx)\n\tmov%s %%%s, %%%s\n", operation, suffix, operand, suffix, operand, accumulator);
        } else {
            const char *operation = info->atomic.rmw == ANVIL_ATOMIC_AND ? "and" : (info->atomic.rmw == ANVIL_ATOMIC_OR ? "or" : "xor");
            const char *function = anvil_mir_func_name(emit->base.mir);
            anvil_strbuf_appendf(&emit->base.code, "\tmov%s (%%rdx), %%%s\n%s%s_atomic_%zu:\n", suffix, accumulator, emit->desc->format->local_label_prefix, function, index);
            anvil_strbuf_appendf(&emit->base.code, "\tmov%s %%%s, %%%s\n\t%s%s %%%s, %%%s\n", suffix, accumulator, temporary, operation, suffix, operand, temporary);
            anvil_strbuf_appendf(&emit->base.code, "\tlock cmpxchg%s %%%s, (%%rdx)\n\tjne %s%s_atomic_%zu\n", suffix, temporary, emit->desc->format->local_label_prefix, function, index);
        }
        break;
    default:
        emit->base.failed = true;
        return;
    }

    /* Narrow results are kept extended, as loads leave them. */
    int def_phys = x86_gas_phys_of(&emit->base, info->def);
    if (emit->base.failed)
        return;
    if (size < 4)
        anvil_strbuf_appendf(&emit->base.code, "\tmov%c%cl %%%s, %%%s\n", type->is_signed ? 's' : 'z', size == 1 ? 'b' : 'w', accumulator,
                             x86_gas_gpr(def_phys, 4));
    else
        anvil_strbuf_appendf(&emit->base.code, "\tmov%s %%%s, %%%s\n", suffix, accumulator, x86_gas_gpr(def_phys, size));
}

static void x64_emit_instr(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (instr_index == emit->base.adc_skip) {
        emit->base.adc_skip = SIZE_MAX;
        return;
    }
    if (instr_index == emit->base.adc_at) {
        /* mov keeps the carry. */
        emit->base.adc_at = SIZE_MAX;
        const anvil_mir_vreg_info_t *def = x64_vreg_info_checked(emit, info->def);
        int to = x86_gas_phys_of(&emit->base, info->def), from = x86_gas_phys_of(&emit->base, emit->base.adc_other);
        if (!def || emit->base.failed)
            return;
        int size = x64_size_bytes(def->size_bits) < 4 ? 4 : x64_size_bytes(def->size_bits);
        if (to != from)
            anvil_strbuf_appendf(&emit->base.code, "\tmov%s %%%s, %%%s\n", x86_gas_size_suffix(size), x86_gas_gpr(from, size), x86_gas_gpr(to, size));
        anvil_strbuf_appendf(&emit->base.code, "\tadc%s $0, %%%s\n", x86_gas_size_suffix(size), x86_gas_gpr(to, size));
        return;
    }
    if (!anvil_mir_isel_registerless_ok(emit->base.mir, &emit->base.isel, instr_index, info)) {
        emit->base.failed = true;
        return;
    }
    switch (info->op) {
    case ANVIL_MIR_OP_ATOMIC:
        x64_emit_atomic(emit, instr_index, info);
        return;

    case ANVIL_MIR_OP_MOV:
        x86_gas_emit_mov(&emit->base, info);
        break;
    case ANVIL_MIR_OP_COPY: {
        anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
        if (src == ANVIL_MIR_NO_VREG) {
            emit->base.failed = true;
            break;
        }
        x86_gas_emit_copy(&emit->base, info->def, src);
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
    case ANVIL_MIR_OP_VECTOR:
    case ANVIL_MIR_OP_SCALAR:
        x64_emit_vector(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_NEG:
    case ANVIL_MIR_OP_NOT:
    case ANVIL_MIR_OP_FABS:
        x86_gas_emit_unary(&emit->base, instr_index, info);
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
        x64_emit_cast(emit, instr_index, info);
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
        x64_emit_symbol_addr(emit, info);
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
    case ANVIL_MIR_OP_VA_START: {
        if (!emit->desc->is_win64) {
            if (info->frame_slot < 0 || (size_t)info->frame_slot >= emit->base.num_frame_slot_offsets || info->named_gpr > 6 || info->named_fpr > 8 || info->named_stack_bytes > INT_MAX - 16) {
                emit->base.failed = true;
                break;
            }

            int base = -emit->base.frame_slot_offsets[info->frame_slot];
            int destination = x86_gas_phys_of(&emit->base, info->def);
            if (emit->base.failed)
                break;

            anvil_strbuf_appendf(&emit->base.code, "\tmovl $%u, %d(%%rbp)\n", info->named_gpr * 8, base);
            anvil_strbuf_appendf(&emit->base.code, "\tmovl $%u, %d(%%rbp)\n", 48 + info->named_fpr * 16, base + 4);
            anvil_strbuf_appendf(&emit->base.code, "\tleaq %zu(%%rbp), %%r11\n\tmovq %%r11, %d(%%rbp)\n", 16 + info->named_stack_bytes, base + 8);
            anvil_strbuf_appendf(&emit->base.code, "\tleaq %d(%%rbp), %%r11\n\tmovq %%r11, %d(%%rbp)\n", base + 32, base + 16);
            anvil_strbuf_appendf(&emit->base.code, "\tleaq %d(%%rbp), %%%s\n", base, x86_gas_gpr(destination, 8));
            break;
        }

        if (!emit->desc->is_win64 || !info->has_imm || info->imm < 0 || emit->base.frame_size > INT_MAX - 16 || info->imm > (INT_MAX - 16 - emit->base.frame_size) / 8) {
            emit->base.failed = true;
            break;
        }

        int destination = x86_gas_phys_of(&emit->base, info->def);
        if (emit->base.failed)
            break;

        int offset = emit->base.frame_size + 16 + (int)info->imm * 8;
        anvil_strbuf_appendf(&emit->base.code, "\tleaq %d(%%rbp), %%%s\n", offset, x86_gas_gpr(destination, 8));
        break;
    }
    case ANVIL_MIR_OP_DYN_ALLOCA:
        x64_emit_dyn_alloca(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        x64_emit_incoming_stack_arg(emit, info);
        break;
    case ANVIL_MIR_OP_CALL:
        x64_emit_call(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        x86_gas_emit_call_stack_arg(&emit->base, instr_index, info);
        break;
    case ANVIL_MIR_OP_BR:
        emit->base.failed |= !x86_gas_emit_jump(&emit->base.code, emit->desc->format->local_label_prefix, emit->base.mir, &emit->base.layout, emit->base.block, info->true_block);
        break;
    case ANVIL_MIR_OP_SELECT:
        x86_gas_emit_select(&emit->base, instr_index, info);
        break;
    case ANVIL_MIR_OP_BR_COND: {
        anvil_mir_vreg_t cond = anvil_mir_get_instr_use(emit->base.mir, instr_index, 0);
        if (cond == ANVIL_MIR_NO_VREG) {
            emit->base.failed = true;
            break;
        }
        if (emit->base.fused_jcc) {
            const char *cc = emit->base.fused_jcc;
            emit->base.fused_jcc = NULL;
            x64_emit_cond_branch(emit, cc, info->true_block, info->false_block);
            break;
        }
        const anvil_mir_vreg_info_t *cond_info = x64_vreg_info_checked(emit, cond);
        if (!cond_info)
            break;
        int size = x64_size_bytes(cond_info->size_bits);
        const char *cond_reg = x86_gas_gpr(x86_gas_phys_of(&emit->base, cond), size);
        if (emit->base.failed)
            break;
        anvil_strbuf_appendf(&emit->base.code, "\ttest%s %%%s, %%%s\n", x86_gas_size_suffix(size), cond_reg, cond_reg);
        x64_emit_cond_branch(emit, "ne", info->true_block, info->false_block);
        break;
    }
    case ANVIL_MIR_OP_RET:
        x64_emit_ret(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SPILL_LOAD:
        x86_gas_emit_spill_load(&emit->base, info);
        break;
    case ANVIL_MIR_OP_SPILL_STORE:
        x86_gas_emit_spill_store(&emit->base, instr_index, info);
        break;
    case ANVIL_MIR_OP_KEEPALIVE:
        break;
    case ANVIL_MIR_OP_CALL_RESULT:
        /* An additional fixed-register result of the preceding call: the
         * allocator placed it in that register, so nothing moves. */
        if (x86_gas_phys_of(&emit->base, info->def) < 0)
            emit->base.failed = true;
        break;
    case ANVIL_MIR_OP_RET_VALUE_PART:
        emit->base.failed = true;
        break;
    default:
        emit->base.failed = true;
        break;
    }
}

static void x64_emit_rodata(x64_mir_emit_t *emit)
{
    size_t count = anvil_mir_num_string_literals(emit->base.mir);
    if (count == 0 && !emit->base.emitted_fneg_mask && !emit->base.emitted_fabs_mask) {
        return;
    }

    if (!anvil_gnu_emit_mir_strings(&emit->base.code, emit->base.mir, emit->desc->format)) {
        emit->base.failed = true;
        return;
    }

    if (emit->base.emitted_fneg_mask || emit->base.emitted_fabs_mask) {
        anvil_strbuf_append(&emit->base.code, emit->desc->format->literal16_section);
        anvil_obj_emit_align(&emit->base.code, 16);
    }
    if (emit->base.emitted_fneg_mask) {
        anvil_strbuf_appendf(&emit->base.code, "%s%s_fneg_mask32:\n", emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->base.mir));
        anvil_strbuf_append(&emit->base.code, "\t.long 0x80000000\n");
        anvil_strbuf_append(&emit->base.code, "\t.long 0\n\t.long 0\n\t.long 0\n");
        anvil_strbuf_appendf(&emit->base.code, "%s%s_fneg_mask64:\n", emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->base.mir));
        anvil_strbuf_append(&emit->base.code, "\t.quad 0x8000000000000000\n");
        anvil_strbuf_append(&emit->base.code, "\t.quad 0\n");
    }
    if (emit->base.emitted_fabs_mask) {
        anvil_strbuf_appendf(&emit->base.code, "%s%s_fabs_mask32:\n", emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->base.mir));
        anvil_strbuf_append(&emit->base.code, "\t.long 0x7fffffff\n");
        anvil_strbuf_append(&emit->base.code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->base.code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->base.code, "\t.long 0xffffffff\n");
        anvil_strbuf_appendf(&emit->base.code, "%s%s_fabs_mask64:\n", emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->base.mir));
        anvil_strbuf_append(&emit->base.code, "\t.quad 0x7fffffffffffffff\n");
        anvil_strbuf_append(&emit->base.code, "\t.quad 0xffffffffffffffff\n");
    }
}

bool anvil_x86_64_emit_mir_abi(const anvil_mir_func_t *mir, anvil_abi_t abi, anvil_syntax_t syntax, char **output, size_t *len)
{
    if (!mir || !output)
        return false;
    *output = NULL;
    if (len)
        *len = 0;
    if (!anvil_x86_64_verify_mir_legal(mir, NULL, 0))
        return false;

    const anvil_x64_abi_desc_t *desc = anvil_x64_get_abi_desc(abi);
    if (!desc)
        return false;

    x64_mir_emit_t emit;
    memset(&emit, 0, sizeof(emit));
    emit.base.mir = mir;
    emit.base.adc_skip = emit.base.adc_at = emit.base.test_at = SIZE_MAX;
    emit.base.fold_target = SIZE_MAX;
    emit.desc = desc;
    emit.base.syntax = syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : syntax;
    x64_mode(&emit.base, desc);
    anvil_strbuf_init(&emit.base.code);
    if (!emit.base.code.data)
        return false;

    /* The facts first: the frame saves the registers printed code writes. */
    if (!anvil_mir_isel_facts_build(&emit.base.isel, mir, x86_gas_use_takes_immediate, NULL, &emit) || !x64_prepare_frame(&emit) ||
        !anvil_mir_layout_build(&emit.base.layout, mir)) {
        emit.base.wrapped = false;
        anvil_mir_layout_destroy(&emit.base.layout);
        anvil_strbuf_destroy(&emit.base.code);
        free(emit.base.spill_offsets);
        free(emit.base.frame_slot_offsets);
        anvil_mir_isel_facts_destroy(&emit.base.isel);
        return false;
    }

    /* Win64's unwind directives describe a prologue at the entry. */
    emit.base.wrapped = emit.has_frame && !desc->is_win64 && anvil_mir_isel_frameless_prefix(mir, &emit.base.isel, anvil_mir_frame_block(mir));
    x64_emit_prologue(&emit);

    size_t num_blocks = anvil_mir_num_blocks(mir);
    size_t num_instrs = anvil_mir_num_instrs(mir);
    /* Loop headers start aligned (padding outside the loop is cheap). */
    bool *loop_headers = calloc(anvil_mir_num_blocks(mir) ? anvil_mir_num_blocks(mir) : 1, sizeof(*loop_headers));
    if (!loop_headers || !anvil_mir_loop_header_blocks(mir, loop_headers))
        emit.base.failed = true;
    for (size_t b = 0; b < num_blocks && !emit.base.failed && !emit.base.code.failed; b++) {
        if (emit.base.layout.skip[b])
            continue;  /* prints only a jump: branches go where it leads */
        if (loop_headers && loop_headers[b])
            anvil_strbuf_append(&emit.base.code, "\t.p2align 4\n");
        if (!x86_gas_emit_label(&emit.base.code, emit.desc->format->local_label_prefix, mir, (anvil_mir_block_t)b)) {
            emit.base.failed = true;
            break;
        }
        emit.base.block = (anvil_mir_block_t)b;
        if (emit.base.wrapped && b == anvil_mir_frame_block(mir))
            x64_emit_frame_entry(&emit);
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
            x64_emit_instr(&emit, i, &info);
            anvil_print_flags_after(&emit.base.flags, mir, i, &info, emit.base.code.len != printed);
        }
    }
    free(loop_headers);

    if (!emit.base.failed && !emit.base.code.failed && emit.desc->format->cfi)
        anvil_strbuf_append(&emit.base.code, "\t.cfi_endproc\n");
    if (!emit.base.failed && !emit.base.code.failed && emit.desc->is_win64) {
        anvil_strbuf_append(&emit.base.code, "\t.seh_endproc\n");
    } else if (!emit.base.failed && !emit.base.code.failed && !emit.desc->is_darwin) {
        const char *prefix = emit.base.symbol_prefix;
        const char *name = anvil_mir_func_name(mir);
        anvil_strbuf_appendf(&emit.base.code, "\t.size %s%s, .-%s%s\n", prefix, name, prefix, name);
    }
    if (!emit.base.failed && !emit.base.code.failed) {
        x64_emit_rodata(&emit);
    }

    free(emit.base.spill_offsets);
    free(emit.base.frame_slot_offsets);
    anvil_mir_isel_facts_destroy(&emit.base.isel);
    anvil_mir_layout_destroy(&emit.base.layout);
    if (emit.base.failed || emit.base.code.failed) {
        anvil_strbuf_destroy(&emit.base.code);
        return false;
    }

    *output = anvil_strbuf_detach(&emit.base.code, len);
    return *output != NULL;
}

bool anvil_x86_64_emit_mir(const anvil_mir_func_t *mir, char **output, size_t *len)
{
    return anvil_x86_64_emit_mir_abi(mir, ANVIL_ABI_SYSV, ANVIL_SYNTAX_GAS, output, len);
}
