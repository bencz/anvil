#include "../x86_64_internal.h"
#include "anvil/anvil_analysis.h"
#include "../../common/mir_isel.h"
#include "../../common/parallel_move.h"
#include "x86_gas.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const anvil_mir_func_t *mir;
    const anvil_x64_abi_desc_t *desc;
    anvil_syntax_t syntax;
    anvil_strbuf_t code;
    int *spill_offsets;
    size_t num_spill_offsets;
    int *frame_slot_offsets;
    size_t num_frame_slot_offsets;
    int gpr_save_offsets[16];
    int fpr_save_offsets[16];
    int outgoing_size;
    int frame_size;
    bool has_frame;
    bool emitted_fneg_mask;
    bool emitted_fabs_mask;
    bool failed;
    anvil_mir_isel_facts_t isel;
    anvil_mir_block_t block;  /* being emitted (blocks print in order) */
    const char *fused_jcc;    /* set by a compare folded into the next BR_COND or SELECT */
} x64_mir_emit_t;

static bool x64_known_imm32(const x64_mir_emit_t *emit, anvil_mir_vreg_t vreg, int64_t *out);
static bool x64_use_takes_immediate(void *ctx, size_t index, const anvil_mir_instr_info_t *info, size_t use, int64_t value);

/* Whether the selection below encodes operand `use` of this instruction as
 * an immediate (mirrors x64_emit_gpr_binary and x64_emit_cmp). */
static bool x64_use_takes_immediate(void *ctx, size_t index, const anvil_mir_instr_info_t *info, size_t use, int64_t value)
{
    const x64_mir_emit_t *emit = ctx;
    const anvil_mir_vreg_info_t *def = info->def != ANVIL_MIR_NO_VREG ? anvil_mir_get_vreg_info(emit->mir, info->def) : NULL;
    if (!def || def->reg_class != ANVIL_MIR_REG_GPR || info->num_uses != 2 || value < INT32_MIN || value > INT32_MAX)
        return false;
    switch (info->op) {
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_MUL:
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR: {
        /* The right operand when constant, else a constant left one. */
        int64_t other;
        anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, index, 1);
        return use == 1 || !x64_known_imm32(emit, rhs, &other);
    }
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
        return use == 1;
    case ANVIL_MIR_OP_SCALAR:
        return info->ir_op == ANVIL_OP_ROTL && use == 1;
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
    case ANVIL_MIR_OP_CMP_UGE: {
        const anvil_mir_vreg_info_t *lhs = anvil_mir_get_vreg_info(emit->mir, anvil_mir_get_instr_use(emit->mir, index, 0));
        return use == 1 && lhs && lhs->reg_class == ANVIL_MIR_REG_GPR;
    }
    default:
        return false;
    }
}

/* The immediate a GPR vreg is known to hold, when it fits a sign-extended
 * 32-bit instruction field. */
static bool x64_facts_imm32(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg, int64_t *out)
{
    int64_t value;
    if (!anvil_mir_isel_constant(facts, vreg, &value) || value < INT32_MIN || value > INT32_MAX)
        return false;
    *out = value;
    return true;
}

static bool x64_known_imm32(const x64_mir_emit_t *emit, anvil_mir_vreg_t vreg, int64_t *out)
{
    return x64_facts_imm32(&emit->isel, vreg, out);
}

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
        return def && def->reg_class == ANVIL_MIR_REG_GPR && !x64_facts_imm32(facts, anvil_mir_get_instr_use(mir, index, 1), &count) ? rcx : 0;
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD:
        return rax | rdx;
    case ANVIL_MIR_OP_SCALAR:
        if (info->ir_op == ANVIL_OP_ROTL)
            return x64_facts_imm32(facts, anvil_mir_get_instr_use(mir, index, 1), &count) ? 0 : rcx;
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
    if (!anvil_mir_isel_facts_build(&facts, mir, NULL, NULL))
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

static int x64_align_int(int value, int align)
{
    return (value + align - 1) & ~(align - 1);
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
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->mir, vreg);
    if (!info)
        emit->failed = true;
    return info;
}

static const anvil_regalloc_assignment_t *x64_assignment_checked(x64_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(emit->mir, vreg);
    if (!assignment || assignment->spilled || assignment->phys_reg < 0) {
        emit->failed = true;
        return NULL;
    }
    return assignment;
}

static const char *x64_gpr_name(int phys_reg, int size)
{
    if (phys_reg < 0 || phys_reg >= 16)
        return "?";
    switch (size) {
    case 1:
        return x64_reg8_names[phys_reg];
    case 2:
        return x64_reg16_names[phys_reg];
    case 4:
        return x64_reg32_names[phys_reg];
    default:
        return x64_reg64_names[phys_reg];
    }
}

static int x64_phys_of(x64_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment = x64_assignment_checked(emit, vreg);
    return assignment ? assignment->phys_reg : -1;
}

static const char *x64_reg_name(x64_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = x64_vreg_info_checked(emit, vreg);
    const anvil_regalloc_assignment_t *assignment = x64_assignment_checked(emit, vreg);
    if (!info || !assignment)
        return "?";

    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        if (assignment->phys_reg < 0 || assignment->phys_reg >= 16) {
            emit->failed = true;
            return "?";
        }
        return x64_xmm_names[assignment->phys_reg];
    }

    int size = x64_size_bytes(info->size_bits);
    return x64_gpr_name(assignment->phys_reg, size);
}

static const char *x64_symbol_prefix(const x64_mir_emit_t *emit)
{
    return emit && emit->desc ? emit->desc->sym_prefix : "";
}

static bool x64_symbol_is_local(const char *symbol)
{
    return symbol && symbol[0] == '.';
}

static const char *x64_symbol_ref_prefix(const x64_mir_emit_t *emit, const char *symbol)
{
    return x64_symbol_is_local(symbol) ? "" : x64_symbol_prefix(emit);
}

static const char *x64_size_suffix(int size)
{
    switch (size) {
    case 1:
        return "b";
    case 2:
        return "w";
    case 4:
        return "l";
    default:
        return "q";
    }
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

    for (size_t i = 0; i < anvil_mir_num_instrs(emit->mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->mir, i, &info))
            return false;
        if (info.op != ANVIL_MIR_OP_CALL_STACK_ARG)
            continue;
        if (!info.has_imm || info.imm < 0 || info.num_uses != 1)
            return false;
        if (info.imm > INT32_MAX - 8)
            return false;

        int end = (int)info.imm + 8;
        if (end > outgoing_size)
            outgoing_size = end;
    }

    bool has_call = x64_instr_has_call(emit->mir);
    if (!has_call && outgoing_size > 0)
        return false;
    if (has_call) {
        if (outgoing_size > INT_MAX - emit->desc->shadow_space)
            return false;
        outgoing_size += emit->desc->shadow_space;
    }
    emit->outgoing_size = x64_align_int(outgoing_size, 16);
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
        bool used = false;
        for (size_t i = 0; i < anvil_mir_num_vregs(emit->mir); i++) {
            const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(emit->mir, (anvil_mir_vreg_t)i);
            if (!assignment || assignment->spilled)
                continue;
            if (assignment->reg_class == ANVIL_MIR_REG_GPR && assignment->phys_reg == reg) {
                used = true;
                break;
            }
        }
        if (used) {
            offset += 8;
            emit->gpr_save_offsets[reg] = offset;
        }
    }

    if (emit->desc->is_win64) {
        for (int reg = 6; reg <= 15; reg++) {
            bool used = false;
            for (size_t i = 0; i < anvil_mir_num_vregs(emit->mir); i++) {
                const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(emit->mir, (anvil_mir_vreg_t)i);
                if (!assignment || assignment->spilled)
                    continue;
                if (assignment->reg_class == ANVIL_MIR_REG_FPR && assignment->phys_reg == reg) {
                    used = true;
                    break;
                }
            }
            if (used) {
                offset = x64_align_int(offset, 16);
                offset += 16;
                emit->fpr_save_offsets[reg] = offset;
            }
        }
    }

    emit->num_spill_offsets = anvil_mir_num_spills(emit->mir);
    emit->num_frame_slot_offsets = anvil_mir_num_frame_slots(emit->mir);
    if (emit->num_frame_slot_offsets > 0) {
        emit->frame_slot_offsets = calloc(emit->num_frame_slot_offsets, sizeof(*emit->frame_slot_offsets));
        if (!emit->frame_slot_offsets)
            return false;
    }

    /* Addressed downward from RBP; the stack guarantees 16-byte alignment. */
    if (!anvil_mir_layout_frame_slots(emit->mir, offset, 8, 16, true, emit->frame_slot_offsets, &offset))
        return false;

    if (emit->num_spill_offsets > 0) {
        emit->spill_offsets = calloc(emit->num_spill_offsets, sizeof(*emit->spill_offsets));
        if (!emit->spill_offsets)
            return false;
    }

    for (size_t i = 0; i < emit->num_spill_offsets; i++) {
        anvil_mir_spill_slot_info_t slot;
        if (!anvil_mir_get_spill_slot_info(emit->mir, (int)i, &slot)) {
            return false;
        }
        offset += x64_align_int(x64_size_bytes(slot.size_bits), 8);
        emit->spill_offsets[i] = offset;
    }

    offset += emit->outgoing_size;
    emit->frame_size = x64_align_int(offset, 16);
    /* A leaf with no frame bytes runs on the caller's frame (no RBP setup).
     * Win64 keeps its prologue: the unwind directives describe it. */
    emit->has_frame = emit->frame_size > 0 || emit->desc->is_win64 || anvil_mir_needs_frame_record(emit->mir);
    return true;
}

/* Frame offsets are measured down from the saved RBP slot. Win64 records a
 * frame pointer at the bottom of the fixed allocation so unwind can recover
 * RSP even after a dynamic alloca (SEH frame offsets are limited to 240). */
static int x64_frame_disp(const x64_mir_emit_t *emit, int offset)
{
    return (emit->desc->is_win64 ? emit->frame_size : 0) - offset;
}

/* Probe every crossed guard page before moving RSP. Using inline probes
 * keeps generated GAS independent of MSVC/MinGW's different chkstk symbols.
 * R11 holds the size; RAX/R10 are reserved emitter temporaries. */
static void x64_emit_stack_probe(x64_mir_emit_t *emit, size_t id)
{
    const char *name = anvil_mir_func_name(emit->mir);
    anvil_strbuf_append(&emit->code, "\tmovq %rsp, %rax\n\tmovq %rsp, %r10\n\tsubq %r11, %r10\n");
    anvil_strbuf_appendf(&emit->code, "%s%s_probe_%zu_loop:\n", emit->desc->format->local_label_prefix, name, id);
    anvil_strbuf_append(&emit->code, "\tsubq $4096, %rax\n\tcmpq %r10, %rax\n");
    anvil_strbuf_appendf(&emit->code, "\tjb %s%s_probe_%zu_done\n", emit->desc->format->local_label_prefix, name, id);
    anvil_strbuf_append(&emit->code, "\ttestb $0, (%rax)\n");
    anvil_strbuf_appendf(&emit->code, "\tjmp %s%s_probe_%zu_loop\n", emit->desc->format->local_label_prefix, name, id);
    anvil_strbuf_appendf(&emit->code, "%s%s_probe_%zu_done:\n", emit->desc->format->local_label_prefix, name, id);
    anvil_strbuf_append(&emit->code, "\ttestb $0, (%r10)\n");
}

static void x64_emit_prologue(x64_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);
    const char *prefix = x64_symbol_prefix(emit);
    char symbol[512];
    snprintf(symbol, sizeof(symbol), "%s%s", prefix, name);
    if (emit->desc->is_darwin) {
        anvil_strbuf_append(&emit->code, emit->desc->format->text_section);
        anvil_obj_emit_binding(&emit->code, emit->desc->format, anvil_mir_func_linkage(emit->mir), symbol);
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    } else if (emit->desc->is_win64) {
        anvil_strbuf_append(&emit->code, emit->desc->format->text_section);
        anvil_obj_emit_binding(&emit->code, emit->desc->format, anvil_mir_func_linkage(emit->mir), symbol);
        anvil_strbuf_appendf(&emit->code, "\t.def %s%s; .scl %d; .type 32; .endef\n", prefix, name,
                             anvil_mir_func_linkage(emit->mir) == ANVIL_LINK_INTERNAL ? 3 : 2);
        anvil_strbuf_appendf(&emit->code, "\t.seh_proc %s%s\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    } else {
        anvil_strbuf_append(&emit->code, emit->desc->format->text_section);
        anvil_obj_emit_binding(&emit->code, emit->desc->format, anvil_mir_func_linkage(emit->mir), symbol);
        anvil_strbuf_appendf(&emit->code, "\t.type %s%s, @function\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    }

    bool cfi = emit->desc->format->cfi;
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_startproc\n");
    if (!emit->has_frame)
        return;
    anvil_strbuf_append(&emit->code, "\tpushq %rbp\n");
    if (emit->desc->is_win64)
        anvil_strbuf_append(&emit->code, "\t.seh_pushreg %rbp\n");
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_def_cfa_offset 16\n\t.cfi_offset %rbp, -16\n");
    if (!emit->desc->is_win64)
        anvil_strbuf_append(&emit->code, "\tmovq %rsp, %rbp\n");
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_def_cfa_register %rbp\n");
    if (emit->frame_size > 0) {
        if (emit->desc->is_win64 && emit->frame_size >= 4096) {
            anvil_strbuf_appendf(&emit->code, "\tmovq $%d, %%r11\n", emit->frame_size);
            x64_emit_stack_probe(emit, SIZE_MAX);
        }
        anvil_strbuf_appendf(&emit->code, "\tsubq $%d, %%rsp\n", emit->frame_size);
        if (emit->desc->is_win64)
            anvil_strbuf_appendf(&emit->code, "\t.seh_stackalloc %d\n", emit->frame_size);
    }

    if (emit->desc->is_win64)
        anvil_strbuf_append(&emit->code, "\tmovq %rsp, %rbp\n\t.seh_setframe %rbp, 0\n");

    for (int reg = 0; reg < 16; reg++) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %d(%%rbp)\n", x64_reg64_names[reg], x64_frame_disp(emit, emit->gpr_save_offsets[reg]));
            if (cfi)
                anvil_strbuf_appendf(&emit->code, "\t.cfi_offset %%%s, %d\n", x64_reg64_names[reg], x64_frame_disp(emit, emit->gpr_save_offsets[reg]) - 16);
            if (emit->desc->is_win64)
                anvil_strbuf_appendf(&emit->code, "\t.seh_savereg %%%s, %d\n", x64_reg64_names[reg], emit->frame_size - emit->gpr_save_offsets[reg]);
        }
    }
    for (int reg = 6; reg < 16; reg++) {
        if (emit->fpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovdqu %%xmm%d, %d(%%rbp)\n", reg, x64_frame_disp(emit, emit->fpr_save_offsets[reg]));
            anvil_strbuf_appendf(&emit->code, "\t.seh_savexmm %%xmm%d, %d\n", reg, emit->frame_size - emit->fpr_save_offsets[reg]);
        }
    }
    if (emit->desc->is_win64)
        anvil_strbuf_append(&emit->code, "\t.seh_endprologue\n");

    /* Home the incoming register bits before parameter copies or generated
     * instructions can overwrite them. Win64 variadic FP arguments also arrive
     * in these GPRs, so the cursor sees one contiguous sequence of 8-byte slots. */
    for (size_t index = 0; index < anvil_mir_num_instrs(emit->mir); index++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->mir, index, &info)) {
            emit->failed = true;
            return;
        }
        if (info.op != ANVIL_MIR_OP_VA_START)
            continue;

        if (!emit->desc->is_win64) {
            if (info.frame_slot < 0 || (size_t)info.frame_slot >= emit->num_frame_slot_offsets) {
                emit->failed = true;
                return;
            }

            int save = -emit->frame_slot_offsets[info.frame_slot] + 32;
            const char *registers[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
            for (int slot = 0; slot < 6; slot++)
                anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %d(%%rbp)\n", registers[slot], save + slot * 8);

            for (int slot = 0; slot < 8; slot++)
                anvil_strbuf_appendf(&emit->code, "\tmovups %%xmm%d, %d(%%rbp)\n", slot, save + 48 + slot * 16);

            continue;
        }

        if (!emit->desc->is_win64 || emit->frame_size > INT_MAX - 40) {
            emit->failed = true;
            return;
        }
        const char *registers[] = {"rcx", "rdx", "r8", "r9"};
        for (int slot = 0; slot < 4; slot++)
            anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %d(%%rbp)\n", registers[slot], emit->frame_size + 16 + slot * 8);

        break;
    }
}

static void x64_emit_epilogue(x64_mir_emit_t *emit)
{
    bool cfi = emit->desc->format->cfi;
    if (!emit->has_frame) {
        anvil_strbuf_append(&emit->code, "\tret\n");
        return;
    }
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_remember_state\n");
    for (int reg = 15; reg >= 6; reg--) {
        if (emit->fpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovdqu %d(%%rbp), %%xmm%d\n", x64_frame_disp(emit, emit->fpr_save_offsets[reg]), reg);
        }
    }
    for (int reg = 15; reg >= 0; reg--) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovq %d(%%rbp), %%%s\n", x64_frame_disp(emit, emit->gpr_save_offsets[reg]), x64_reg64_names[reg]);
        }
    }
    if (emit->desc->is_win64)
        anvil_strbuf_appendf(&emit->code, "\tleaq %d(%%rbp), %%rsp\n", emit->frame_size);
    else
        anvil_strbuf_append(&emit->code, "\tmovq %rbp, %rsp\n");
    anvil_strbuf_append(&emit->code, "\tpopq %rbp\n");
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_def_cfa %rsp, 8\n");
    anvil_strbuf_append(&emit->code, "\tret\n");
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_restore_state\n");
}

/* The printer's XMM temporaries (see anvil_x64_abi_desc_t.sse_temps). */
static const char *x64_sse_temp(const x64_mir_emit_t *emit)
{
    return x64_xmm_names[emit->desc->sse_temps[0]];
}

static const char *x64_sse_temp2(const x64_mir_emit_t *emit)
{
    return x64_xmm_names[emit->desc->sse_temps[1]];
}

static const char *x64_fp_mov_op(int size)
{
    if (size == 16)
        return "movups";

    return size <= 4 ? "movss" : "movsd";
}

/* Register to register: movss/movsd between registers merge into the
 * destination's low lane, so they depend on its old value (a loop-carried
 * chain when the register was last written in the previous iteration);
 * movaps writes it whole. */
static const char *x64_fp_copy_op(void)
{
    return "movaps";
}

static void x64_emit_copy(x64_mir_emit_t *emit, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = x64_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = x64_vreg_info_checked(emit, src);
    int dst_phys = x64_phys_of(emit, dst);
    int src_phys = x64_phys_of(emit, src);
    if (!dst_info || !src_info || emit->failed)
        return;

    const char *dst_reg = x64_reg_name(emit, dst);
    const char *src_reg = x64_reg_name(emit, src);
    if (emit->failed)
        return;

    if (dst_info->reg_class == ANVIL_MIR_REG_FPR && src_info->reg_class == ANVIL_MIR_REG_FPR) {
        if (dst_phys == src_phys)
            return;
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_copy_op(), src_reg, dst_reg);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_GPR && src_info->reg_class == ANVIL_MIR_REG_GPR) {
        if (dst_phys == src_phys)
            return;
        int size = x64_size_bytes(dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", x64_size_suffix(size), x64_gpr_name(src_phys, size), x64_gpr_name(dst_phys, size));
    } else if (dst_info->reg_class == ANVIL_MIR_REG_FPR && src_info->reg_class == ANVIL_MIR_REG_GPR) {
        int size = x64_size_bytes(dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", size <= 4 ? "movd" : "movq", x64_gpr_name(src_phys, size <= 4 ? 4 : 8), dst_reg);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_GPR && src_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x64_size_bytes(src_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", size <= 4 ? "movd" : "movq", src_reg, x64_gpr_name(dst_phys, size <= 4 ? 4 : 8));
    } else {
        emit->failed = true;
    }
}

static void x64_emit_mov(x64_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    if (!def_info)
        return;
    if (!anvil_mir_isel_materialize(&emit->isel, info->def))
        return;
    int dst_phys = x64_phys_of(emit, info->def);
    if (emit->failed)
        return;

    int64_t imm = info->has_imm ? info->imm : 0;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x64_size_bytes(def_info->size_bits);
        const char *dst = x64_xmm_names[dst_phys];
        if (imm == 0) {
            anvil_strbuf_appendf(&emit->code, "\txorps %%%s, %%%s\n", dst, dst);
            return;
        }
        if (size <= 4) {
            anvil_strbuf_appendf(&emit->code, "\tmovl $%u, %%r11d\n", (uint32_t)imm);
            anvil_strbuf_appendf(&emit->code, "\tmovd %%r11d, %%%s\n", dst);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tmovabsq $%llu, %%r11\n", (unsigned long long)(uint64_t)imm);
            anvil_strbuf_appendf(&emit->code, "\tmovq %%r11, %%%s\n", dst);
        }
        return;
    }

    /* The shortest form: xor for zero (flags are dead here: a compare fused
     * with its consumer is adjacent to it), movl when the value fits 32
     * unsigned bits (it zero-extends), movq when it is a signed 32-bit
     * immediate, movabsq otherwise. */
    int size = x64_size_bytes(def_info->size_bits);
    const char *dst32 = x64_gpr_name(dst_phys, 4);
    if (size < 8)
        imm = (int64_t)(uint32_t)imm;
    if (imm == 0)
        anvil_strbuf_appendf(&emit->code, "\txorl %%%s, %%%s\n", dst32, dst32);
    else if (imm > 0 && imm <= (int64_t)UINT32_MAX)
        anvil_strbuf_appendf(&emit->code, "\tmovl $%lld, %%%s\n", (long long)imm, dst32);
    else if (imm >= INT32_MIN && imm < 0)
        anvil_strbuf_appendf(&emit->code, "\tmovq $%lld, %%%s\n", (long long)imm, x64_gpr_name(dst_phys, 8));
    else
        anvil_strbuf_appendf(&emit->code, "\tmovabsq $%lld, %%%s\n", (long long)imm, x64_gpr_name(dst_phys, 8));
}

static void x64_emit_gpr_binary(x64_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info, int a_phys, int b_phys, int size)
{
    const char *suf = x64_size_suffix(size);
    const char *rax = x64_gpr_name(X64_RAX, size);
    const char *rdx = x64_gpr_name(X64_RDX, size);
    const char *rcx = x64_gpr_name(X64_RCX, size);
    const char *a = x64_gpr_name(a_phys, size);
    const char *b = x64_gpr_name(b_phys, size);
    int dst_phys = x64_phys_of(emit, info->def);
    const char *dst = x64_gpr_name(dst_phys, size);
    if (emit->failed)
        return;

    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, index, 1);
    bool commutative = info->op == ANVIL_MIR_OP_ADD || info->op == ANVIL_MIR_OP_MUL || info->op == ANVIL_MIR_OP_AND ||
                       info->op == ANVIL_MIR_OP_OR || info->op == ANVIL_MIR_OP_XOR;
    const char *mnemonic = info->op == ANVIL_MIR_OP_ADD ? "add" : info->op == ANVIL_MIR_OP_SUB ? "sub" : info->op == ANVIL_MIR_OP_MUL ? "imul"
                         : info->op == ANVIL_MIR_OP_AND ? "and" : info->op == ANVIL_MIR_OP_OR ? "or" : info->op == ANVIL_MIR_OP_XOR ? "xor" : NULL;
    int64_t imm = 0;
    bool b_imm = x64_known_imm32(emit, rhs, &imm);
    if (!b_imm && commutative && x64_known_imm32(emit, lhs, &imm)) {
        /* x OP c == c OP x */
        b_imm = true;
        a_phys = b_phys;
        a = b;
    }

    if (mnemonic && b_imm) {
        bool wide = size >= 4;
        if (info->op == ANVIL_MIR_OP_MUL && imm > 0 && (imm & (imm - 1)) == 0) {
            int shift = 0;
            while ((INT64_C(1) << shift) != imm)
                shift++;
            if (dst_phys != a_phys)
                anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, dst);
            if (shift != 0)
                anvil_strbuf_appendf(&emit->code, "\tshl%s $%d, %%%s\n", suf, shift, dst);
        } else if (info->op == ANVIL_MIR_OP_MUL) {
            /* Bytes multiply in 16 bits: the low byte is the same. */
            int width = size >= 2 ? size : 2;
            anvil_strbuf_appendf(&emit->code, "\timul%s $%lld, %%%s, %%%s\n", x64_size_suffix(width), (long long)imm, x64_gpr_name(a_phys, width),
                                 x64_gpr_name(dst_phys, width));
        } else if (dst_phys != a_phys && wide && (info->op == ANVIL_MIR_OP_ADD || (info->op == ANVIL_MIR_OP_SUB && imm != INT32_MIN))) {
            long long offset = info->op == ANVIL_MIR_OP_ADD ? (long long)imm : -(long long)imm;
            anvil_strbuf_appendf(&emit->code, "\tlea%s %lld(%%%s), %%%s\n", suf, offset, x64_gpr_name(a_phys, 8), dst);
        } else {
            if (dst_phys != a_phys)
                anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, dst);
            anvil_strbuf_appendf(&emit->code, "\t%s%s $%lld, %%%s\n", mnemonic, suf, (long long)imm, dst);
        }
        return;
    }

    if (mnemonic) {
        if (dst_phys == a_phys) {
            anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n", mnemonic, suf, b, dst);
        } else if (dst_phys != b_phys) {
            if (info->op == ANVIL_MIR_OP_ADD && size >= 4)
                anvil_strbuf_appendf(&emit->code, "\tlea%s (%%%s,%%%s), %%%s\n", suf, x64_gpr_name(a_phys, 8), x64_gpr_name(b_phys, 8), dst);
            else
                anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n\t%s%s %%%s, %%%s\n", suf, a, dst, mnemonic, suf, b, dst);
        } else if (commutative) {
            anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n", mnemonic, suf, a, dst);
        } else {
            /* dst holds the subtrahend: a - b == -b + a. */
            anvil_strbuf_appendf(&emit->code, "\tneg%s %%%s\n\tadd%s %%%s, %%%s\n", suf, dst, suf, a, dst);
        }
        return;
    }

    switch (info->op) {
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR: {
        const char *sh = info->op == ANVIL_MIR_OP_SHL ? "shl" : info->op == ANVIL_MIR_OP_SHR ? "shr" : "sar";
        int64_t count = 0;
        if (x64_known_imm32(emit, rhs, &count)) {
            if (dst_phys != a_phys)
                anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, dst);
            anvil_strbuf_appendf(&emit->code, "\t%s%s $%d, %%%s\n", sh, suf, (int)(count & (size == 8 ? 63 : 31)), dst);
            break;
        }
        /* RCX is an emitter temporary; dst may alias b, already copied. */
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, b, rcx);
        if (dst_phys != a_phys)
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s%s %%cl, %%%s\n", sh, suf, dst);
        break;
    }
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD: {
        bool is_signed = info->op == ANVIL_MIR_OP_SDIV || info->op == ANVIL_MIR_OP_SMOD;
        bool is_mod = info->op == ANVIL_MIR_OP_SMOD || info->op == ANVIL_MIR_OP_UMOD;
        /* RDX:RAX are this instruction's scratch: no operand is there. */
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
        if (size == 1) {
            if (is_signed) {
                anvil_strbuf_append(&emit->code, "\tcbtw\n");
                anvil_strbuf_appendf(&emit->code, "\tidivb %%%s\n", b);
            } else {
                anvil_strbuf_append(&emit->code, "\tmovzbw %al, %ax\n");
                anvil_strbuf_appendf(&emit->code, "\tdivb %%%s\n", b);
            }
            if (is_mod) {
                anvil_strbuf_append(&emit->code, "\tshrw $8, %ax\n");
            }
            anvil_strbuf_appendf(&emit->code, "\tmovb %%al, %%%s\n", dst);
        } else {
            if (is_signed) {
                const char *ext = size == 2 ? "\tcwtd\n" : size == 4 ? "\tcltd\n" : "\tcqto\n";
                anvil_strbuf_append(&emit->code, ext);
                anvil_strbuf_appendf(&emit->code, "\tidiv%s %%%s\n", suf, b);
            } else {
                anvil_strbuf_appendf(&emit->code, "\txorl %%edx, %%edx\n");
                anvil_strbuf_appendf(&emit->code, "\tdiv%s %%%s\n", suf, b);
            }
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, is_mod ? rdx : rax, dst);
        }
        break;
    }
    default:
        emit->failed = true;
        break;
    }
}

/* `dst = a OP b` with a destructive two-operand SSE instruction
 * (`OP b, dst`).  Linear scan may reuse the dying RHS for the result:
 * copying LHS there first would lose RHS, so a commutative operation
 * works on b's register and any other uses the volatile, non-allocatable
 * emitter temp (XMM5 on Win64, XMM15 on SysV). */
static void x64_emit_sse_destructive(x64_mir_emit_t *emit, const char *mnemonic, anvil_mir_vreg_t def, anvil_mir_vreg_t lhs,
                                     anvil_mir_vreg_t rhs, bool commutative)
{
    const char *dst = x64_reg_name(emit, def);
    const char *a = x64_reg_name(emit, lhs);
    const char *b = x64_reg_name(emit, rhs);
    if (emit->failed)
        return;
    bool overlaps_rhs = x64_phys_of(emit, def) == x64_phys_of(emit, rhs) && x64_phys_of(emit, lhs) != x64_phys_of(emit, rhs);
    if (overlaps_rhs && commutative) {
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", mnemonic, a, dst);
        return;
    }
    const char *work = overlaps_rhs ? x64_sse_temp(emit) : dst;
    if (strcmp(a, work) != 0)
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_copy_op(), a, work);
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", mnemonic, b, work);
    if (overlaps_rhs)
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_copy_op(), work, dst);
}

/* Broadcast of lane 0.  FP scalars are already in an XMM register; an
 * integer scalar moves from its GPR first, and narrow lanes widen to a
 * dword (bytes pair up, words fill the low quadword) before pshufd. */
static void x64_emit_vector_splat(x64_mir_emit_t *emit, const anvil_vector_isa_t *isa, const anvil_mir_instr_info_t *info,
                                  anvil_mir_vreg_t scalar)
{
    const char *dst = x64_reg_name(emit, info->def);
    const anvil_mir_vreg_info_t *source = x64_vreg_info_checked(emit, scalar);
    int phys = x64_phys_of(emit, scalar);
    if (emit->failed || !source)
        return;
    if (source->reg_class == ANVIL_MIR_REG_FPR) {
        if (strcmp(dst, x64_xmm_names[phys]) != 0)
            anvil_strbuf_appendf(&emit->code, "\tmovaps %%%s, %%%s\n", x64_xmm_names[phys], dst);
        if (info->imm == 32)
            anvil_strbuf_appendf(&emit->code, "\tshufps $0, %%%s, %%%s\n", dst, dst);
        else
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", isa->mnemonic, dst, dst);
        return;
    }
    if (info->imm == 64) {
        anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %%%s\n", x64_reg64_names[phys], dst);
        anvil_strbuf_appendf(&emit->code, "\tpunpcklqdq %%%s, %%%s\n", dst, dst);
        return;
    }
    anvil_strbuf_appendf(&emit->code, "\tmovd %%%s, %%%s\n", x64_reg32_names[phys], dst);
    if (info->imm == 8)
        anvil_strbuf_appendf(&emit->code, "\tpunpcklbw %%%s, %%%s\n", dst, dst);
    if (info->imm <= 16)
        anvil_strbuf_appendf(&emit->code, "\tpshuflw $0, %%%s, %%%s\n", dst, dst);
    anvil_strbuf_appendf(&emit->code, "\tpshufd $0, %%%s, %%%s\n", dst, dst);
}

static bool x64_vector_commutative(anvil_op_t operation)
{
    return operation == ANVIL_OP_FADD || operation == ANVIL_OP_FMUL || operation == ANVIL_OP_ADD || operation == ANVIL_OP_MUL ||
           operation == ANVIL_OP_AND || operation == ANVIL_OP_OR || operation == ANVIL_OP_XOR || operation == ANVIL_OP_ADD_SAT_U ||
           operation == ANVIL_OP_ADD_SAT_S;
}

/* dst = ~dst, against all ones in the temp. */
static void x64_emit_vector_not(x64_mir_emit_t *emit, const char *dst)
{
    const char *temp = x64_sse_temp(emit);
    anvil_strbuf_appendf(&emit->code, "\tpcmpeqd %%%s, %%%s\n\tpxor %%%s, %%%s\n", temp, temp, temp, dst);
}

static char x64_lane_suffix(int64_t lane_bits)
{
    return lane_bits == 8 ? 'b' : lane_bits == 16 ? 'w' : lane_bits == 32 ? 'd' : 'q';
}

/* Integer lane compares from pcmpeq/pcmpgt (signed) and the unsigned
 * max/min (a <=u b is max(a, b) == b; a >=u b is min(a, b) == b). */
static void x64_emit_vector_int_compare(x64_mir_emit_t *emit, const anvil_vector_isa_t *isa, const anvil_mir_instr_info_t *info,
                                        anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    bool complement = false;
    switch (info->ir_op) {
    case ANVIL_OP_CMP_NE:
        complement = true;
        /* fallthrough */
    case ANVIL_OP_CMP_EQ:
        x64_emit_sse_destructive(emit, isa->mnemonic, info->def, lhs, rhs, true);
        break;
    case ANVIL_OP_CMP_LE:
        complement = true;
        /* fallthrough */
    case ANVIL_OP_CMP_GT:
        x64_emit_sse_destructive(emit, isa->mnemonic, info->def, lhs, rhs, false);
        break;
    case ANVIL_OP_CMP_GE:
        complement = true;
        /* fallthrough */
    case ANVIL_OP_CMP_LT:
        x64_emit_sse_destructive(emit, isa->mnemonic, info->def, rhs, lhs, false);
        break;
    default: {
        /* Unsigned order, computed in the temp: inputs stay intact. */
        complement = info->ir_op == ANVIL_OP_CMP_UGT || info->ir_op == ANVIL_OP_CMP_ULT;
        const char *temp = x64_sse_temp(emit);
        const char *dst = x64_reg_name(emit, info->def), *a = x64_reg_name(emit, lhs), *b = x64_reg_name(emit, rhs);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\tmovaps %%%s, %%%s\n\t%s %%%s, %%%s\n\tpcmpeq%c %%%s, %%%s\n\tmovaps %%%s, %%%s\n", a, temp,
                             isa->mnemonic, b, temp, x64_lane_suffix(info->imm), b, temp, temp, dst);
        break;
    }
    }
    if (complement && !emit->failed)
        x64_emit_vector_not(emit, x64_reg_name(emit, info->def));
}

/* cmpps/cmppd predicate immediates: eq lt le unord neq nlt nle ord. */
static void x64_emit_vector_fcmp(x64_mir_emit_t *emit, const anvil_vector_isa_t *isa, const anvil_mir_instr_info_t *info,
                                 anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    static const struct {
        int imm;      /* -1: two compares or a constant */
        bool swap;
    } forms[] = {
        [ANVIL_FCMP_OEQ] = {0, false}, [ANVIL_FCMP_OLT] = {1, false}, [ANVIL_FCMP_OLE] = {2, false},
        [ANVIL_FCMP_OGT] = {1, true},  [ANVIL_FCMP_OGE] = {2, true},  [ANVIL_FCMP_UNO] = {3, false},
        [ANVIL_FCMP_UNE] = {4, false}, [ANVIL_FCMP_UGE] = {5, false}, [ANVIL_FCMP_UGT] = {6, false},
        [ANVIL_FCMP_ULT] = {6, true},  [ANVIL_FCMP_ULE] = {5, true},  [ANVIL_FCMP_ORD] = {7, false},
        [ANVIL_FCMP_ONE] = {-1, false}, [ANVIL_FCMP_UEQ] = {-1, false}, [ANVIL_FCMP_FALSE] = {-1, false},
        [ANVIL_FCMP_TRUE] = {-1, false},
    };
    const char *dst = x64_reg_name(emit, info->def);
    if (emit->failed || (unsigned)info->fcmp_pred > (unsigned)ANVIL_FCMP_TRUE) {
        emit->failed = true;
        return;
    }
    char mnemonic[24];
    if (info->fcmp_pred == ANVIL_FCMP_FALSE || info->fcmp_pred == ANVIL_FCMP_TRUE) {
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", info->fcmp_pred == ANVIL_FCMP_TRUE ? "pcmpeqd" : "pxor", dst, dst);
        return;
    }
    int imm = forms[info->fcmp_pred].imm;
    if (imm >= 0) {
        /* eq, unord, neq and ord are symmetric. */
        snprintf(mnemonic, sizeof(mnemonic), "%s $%d,", isa->mnemonic, imm);
        bool swap = forms[info->fcmp_pred].swap;
        x64_emit_sse_destructive(emit, mnemonic, info->def, swap ? rhs : lhs, swap ? lhs : rhs, imm == 0 || imm == 3 || imm == 4 || imm == 7);
        return;
    }
    /* one = ord & neq, ueq = unord | eq: the first half in the temp, the
     * second (symmetric) in dst. */
    bool one = info->fcmp_pred == ANVIL_FCMP_ONE;
    const char *temp = x64_sse_temp(emit), *a = x64_reg_name(emit, lhs), *b = x64_reg_name(emit, rhs);
    anvil_strbuf_appendf(&emit->code, "\tmovaps %%%s, %%%s\n\t%s $%d, %%%s, %%%s\n", a, temp, isa->mnemonic, one ? 7 : 3, b, temp);
    snprintf(mnemonic, sizeof(mnemonic), "%s $%d,", isa->mnemonic, one ? 4 : 0);
    x64_emit_sse_destructive(emit, mnemonic, info->def, lhs, rhs, true);
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", one ? "andps" : "orps", temp, dst);
}

/* Lane select: (mask & then) | (~mask & else), the second half in the
 * temp before dst (which may hold an input) is written. */
static void x64_emit_vector_select(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t mask = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t then_value = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    anvil_mir_vreg_t else_value = anvil_mir_get_instr_use(emit->mir, instr_index, 2);
    if (mask == ANVIL_MIR_NO_VREG || then_value == ANVIL_MIR_NO_VREG || else_value == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const char *dst = x64_reg_name(emit, info->def), *m = x64_reg_name(emit, mask), *a = x64_reg_name(emit, then_value),
               *b = x64_reg_name(emit, else_value), *temp = x64_sse_temp(emit);
    if (emit->failed)
        return;
    anvil_strbuf_appendf(&emit->code, "\tmovaps %%%s, %%%s\n\tpandn %%%s, %%%s\n", m, temp, b, temp);
    if (strcmp(dst, m) == 0)
        anvil_strbuf_appendf(&emit->code, "\tpand %%%s, %%%s\n", a, dst);
    else if (strcmp(dst, a) == 0)
        anvil_strbuf_appendf(&emit->code, "\tpand %%%s, %%%s\n", m, dst);
    else
        anvil_strbuf_appendf(&emit->code, "\tmovaps %%%s, %%%s\n\tpand %%%s, %%%s\n", m, dst, a, dst);
    anvil_strbuf_appendf(&emit->code, "\tpor %%%s, %%%s\n", temp, dst);
}

/* A reduction: halves folded with the table's lane operation in the two
 * temporaries (quadwords, then dwords, words and bytes down to the lane
 * width), lane 0 then moved to the result.  A lane mask ORs faster through
 * its sign bits. */
static void x64_emit_vector_reduce(x64_mir_emit_t *emit, size_t instr_index, const anvil_vector_isa_t *isa, const anvil_mir_instr_info_t *info,
                                   anvil_mir_vreg_t source)
{
    const char *vector = x64_reg_name(emit, source), *t1 = x64_sse_temp(emit), *t2 = x64_sse_temp2(emit);
    const anvil_mir_vreg_info_t *result = x64_vreg_info_checked(emit, info->def);
    int dst = x64_phys_of(emit, info->def);
    if (emit->failed || !result)
        return;
    if (info->ir_op == ANVIL_OP_REDUCE_OR && anvil_vector_mir_is_mask(emit->mir, instr_index, source)) {
        anvil_strbuf_appendf(&emit->code, "\tpmovmskb %%%s, %%%s\n\tnegl %%%s\n\tsbbq %%%s, %%%s\n", vector, x64_reg32_names[dst],
                             x64_reg32_names[dst], x64_reg64_names[dst], x64_reg64_names[dst]);
        return;
    }
    const char *m = isa->mnemonic;
    anvil_strbuf_appendf(&emit->code, "\tpshufd $0x4e, %%%s, %%%s\n\t%s %%%s, %%%s\n", vector, t1, m, vector, t1);
    if (info->imm <= 32)
        anvil_strbuf_appendf(&emit->code, "\tpshufd $0x55, %%%s, %%%s\n\t%s %%%s, %%%s\n", t1, t2, m, t2, t1);
    if (info->imm <= 16)
        anvil_strbuf_appendf(&emit->code, "\tpshuflw $0x55, %%%s, %%%s\n\t%s %%%s, %%%s\n", t1, t2, m, t2, t1);
    if (info->imm <= 8)
        anvil_strbuf_appendf(&emit->code, "\tmovdqa %%%s, %%%s\n\tpsrlw $8, %%%s\n\t%s %%%s, %%%s\n", t1, t2, t2, m, t2, t1);
    if (result->reg_class == ANVIL_MIR_REG_FPR)
        anvil_strbuf_appendf(&emit->code, "\tmovaps %%%s, %%%s\n", t1, x64_xmm_names[dst]);
    else if (info->imm == 64)
        anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %%%s\n", t1, x64_reg64_names[dst]);
    else
        anvil_strbuf_appendf(&emit->code, "\tmovd %%%s, %%%s\n", t1, x64_reg32_names[dst]);
}

/* dst = acc + the narrow lanes of x summed into acc's lanes (ACCUM_*): the
 * partial sums of x in the first temporary (the second helps), then one
 * add, so dst may hold acc or x. */
static void x64_emit_vector_accumulate(x64_mir_emit_t *emit, const anvil_mir_instr_info_t *info, anvil_mir_vreg_t acc, anvil_mir_vreg_t lanes)
{
    const char *dst = x64_reg_name(emit, info->def), *a = x64_reg_name(emit, acc), *x = x64_reg_name(emit, lanes);
    const char *t1 = x64_sse_temp(emit), *t2 = x64_sse_temp2(emit);
    if (emit->failed)
        return;
    bool sign = info->ir_op == ANVIL_OP_ACCUM_SEXT;
    anvil_strbuf_appendf(&emit->code, "\tmovdqa %%%s, %%%s\n", x, t1);
    if (!sign && info->widening >= 4) {
        anvil_strbuf_appendf(&emit->code, "\tpxor %%%s, %%%s\n\tpsadbw %%%s, %%%s\n", t2, t2, t2, t1);
    } else if (!sign) {
        anvil_strbuf_appendf(&emit->code, "\tmovdqa %%%s, %%%s\n\tpsrld $16, %%%s\n\tpslld $16, %%%s\n\tpsrld $16, %%%s\n\tpaddd %%%s, %%%s\n",
                             x, t2, t1, t2, t2, t2, t1);
    } else if (info->widening == 2) {
        anvil_strbuf_appendf(&emit->code, "\tpcmpeqw %%%s, %%%s\n\tpsrlw $15, %%%s\n\tpmaddwd %%%s, %%%s\n", t2, t2, t2, t2, t1);
    } else {
        /* pmaddubsw multiplies its destination's unsigned bytes (ones)
         * by the source's signed ones (x). */
        anvil_strbuf_appendf(&emit->code, "\tpcmpeqb %%%s, %%%s\n\tpabsb %%%s, %%%s\n\tpmaddubsw %%%s, %%%s\n", t2, t2, t2, t2, t1, t2);
        anvil_strbuf_appendf(&emit->code, "\tmovdqa %%%s, %%%s\n\tpcmpeqw %%%s, %%%s\n\tpsrlw $15, %%%s\n\tpmaddwd %%%s, %%%s\n", t2, t1, t2, t2, t2,
                             t2, t1);
    }
    if (strcmp(dst, a) != 0)
        anvil_strbuf_appendf(&emit->code, "\tmovdqa %%%s, %%%s\n", a, dst);
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", info->imm == 64 ? "paddq" : "paddd", t1, dst);
}

/* dst = a * b + c with an accumulating FMA3 form (`OP b, a, dst` computes
 * dst = a * b + dst): c goes into dst first, through the temp when dst
 * holds a or b. */
static void x64_emit_fma(x64_mir_emit_t *emit, const char *mnemonic, anvil_mir_vreg_t def, anvil_mir_vreg_t a, anvil_mir_vreg_t b,
                         anvil_mir_vreg_t c)
{
    const char *dst = x64_reg_name(emit, def), *x = x64_reg_name(emit, a), *y = x64_reg_name(emit, b), *z = x64_reg_name(emit, c);
    if (emit->failed)
        return;
    const char *work = strcmp(dst, z) == 0 || (strcmp(dst, x) != 0 && strcmp(dst, y) != 0) ? dst : x64_sse_temp(emit);
    if (strcmp(work, z) != 0)
        anvil_strbuf_appendf(&emit->code, "\tmovaps %%%s, %%%s\n", z, work);
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s, %%%s\n", mnemonic, y, x, work);
    if (strcmp(work, dst) != 0)
        anvil_strbuf_appendf(&emit->code, "\tmovaps %%%s, %%%s\n", work, dst);
}

/* A lane-wise operation, or a scalar one the table prices, as the SSE table
 * names it for this CPU. */
static void x64_emit_vector(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    const anvil_vector_isa_t *isa =
        anvil_vector_isa_for_mir(x64_vector_isa, x64_vector_isa_count, emit->mir, instr_index, info, anvil_mir_func_cpu_features(emit->mir));
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t rhs = info->num_uses > 1 ? anvil_mir_get_instr_use(emit->mir, instr_index, 1) : ANVIL_MIR_NO_VREG;
    if (!isa || lhs == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    if (info->ir_op == ANVIL_OP_SPLAT) {
        x64_emit_vector_splat(emit, isa, info, lhs);
        return;
    }
    if (info->ir_op == ANVIL_OP_SELECT) {
        x64_emit_vector_select(emit, instr_index, info);
        return;
    }
    if (info->ir_op == ANVIL_OP_FMA) {
        x64_emit_fma(emit, isa->mnemonic, info->def, lhs, rhs, anvil_mir_get_instr_use(emit->mir, instr_index, 2));
        return;
    }
    if (info->ir_op == ANVIL_OP_ROTL) {
        /* The count into CL first (dst may hold it), then rotate a copy. */
        int size = info->imm == 64 ? 8 : 4;
        const char *a = x64_gpr_name(x64_phys_of(emit, lhs), size), *dst = x64_gpr_name(x64_phys_of(emit, info->def), size);
        int64_t count;
        bool immediate = x64_known_imm32(emit, rhs, &count);
        if (!immediate)
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%ecx\n", x64_gpr_name(x64_phys_of(emit, rhs), 4));
        if (emit->failed)
            return;
        if (strcmp(a, dst) != 0)
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", x64_size_suffix(size), a, dst);
        if (immediate)
            anvil_strbuf_appendf(&emit->code, "\t%s $%d, %%%s\n", isa->mnemonic, (int)(count & (size * 8 - 1)), dst);
        else
            anvil_strbuf_appendf(&emit->code, "\t%s %%cl, %%%s\n", isa->mnemonic, dst);
        return;
    }
    if (info->ir_op == ANVIL_OP_UMULH || info->ir_op == ANVIL_OP_SMULH) {
        /* RAX and RDX are emitter temporaries: a into RAX, the high half
         * from RDX. */
        int size = info->imm == 64 ? 8 : 4;
        const char *a = x64_gpr_name(x64_phys_of(emit, lhs), size), *b = x64_gpr_name(x64_phys_of(emit, rhs), size);
        const char *dst = x64_gpr_name(x64_phys_of(emit, info->def), size);
        if (!emit->failed)
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n\t%s %%%s\n\tmov%s %%%s, %%%s\n", x64_size_suffix(size), a,
                                 x64_gpr_name(X64_RAX, size), isa->mnemonic, b, x64_size_suffix(size), x64_gpr_name(X64_RDX, size), dst);
        return;
    }
    if (anvil_op_is_reduction(info->ir_op)) {
        x64_emit_vector_reduce(emit, instr_index, isa, info, lhs);
        return;
    }
    if (info->widening) {
        x64_emit_vector_accumulate(emit, info, lhs, rhs);
        return;
    }
    if (info->num_uses == 1) {
        /* A unary operation writes its destination from one source (a
         * scalar one only its low lane: zeroing the rest first breaks the
         * dependency on the old value). */
        const char *dst = x64_reg_name(emit, info->def), *source = x64_reg_name(emit, lhs);
        if (emit->failed)
            return;
        if (info->op == ANVIL_MIR_OP_SCALAR && strcmp(dst, source) != 0)
            anvil_strbuf_appendf(&emit->code, "\txorps %%%s, %%%s\n", dst, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", isa->mnemonic, source, dst);
        return;
    }
    if (rhs == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    if (info->ir_op == ANVIL_OP_FCMP) {
        x64_emit_vector_fcmp(emit, isa, info, lhs, rhs);
        return;
    }
    if (info->ir_op >= ANVIL_OP_CMP_EQ && info->ir_op <= ANVIL_OP_CMP_UGE) {
        x64_emit_vector_int_compare(emit, isa, info, lhs, rhs);
        return;
    }
    if (isa->shape == ANVIL_VECTOR_SHAPE_UNIFORM) {
        /* One count for every lane, from its GPR into the temp. */
        const anvil_mir_vreg_info_t *count = x64_vreg_info_checked(emit, rhs);
        const char *dst = x64_reg_name(emit, info->def);
        const char *a = x64_reg_name(emit, lhs);
        int phys = x64_phys_of(emit, rhs);
        if (emit->failed || !count)
            return;
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", count->size_bits > 32 ? "movq" : "movd",
                             count->size_bits > 32 ? x64_reg64_names[phys] : x64_reg32_names[phys], x64_sse_temp(emit));
        if (strcmp(a, dst) != 0)
            anvil_strbuf_appendf(&emit->code, "\tmovaps %%%s, %%%s\n", a, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", isa->mnemonic, x64_sse_temp(emit), dst);
        return;
    }
    x64_emit_sse_destructive(emit, isa->mnemonic, info->def, lhs, rhs, x64_vector_commutative(info->ir_op));
}

static void x64_emit_binary(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    if (!def_info)
        return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x64_size_bytes(def_info->size_bits);
        const char *op = NULL;
        switch (info->op) {
        case ANVIL_MIR_OP_ADD:
            op = size <= 4 ? "addss" : "addsd";
            break;
        case ANVIL_MIR_OP_SUB:
            op = size <= 4 ? "subss" : "subsd";
            break;
        case ANVIL_MIR_OP_MUL:
            op = size <= 4 ? "mulss" : "mulsd";
            break;
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_FDIV:
            op = size <= 4 ? "divss" : "divsd";
            break;
        default:
            emit->failed = true;
            return;
        }
        x64_emit_sse_destructive(emit, op, info->def, lhs, rhs, info->op == ANVIL_MIR_OP_ADD || info->op == ANVIL_MIR_OP_MUL);
        return;
    }

    int size = x64_size_bytes(def_info->size_bits);
    int a_phys = x64_phys_of(emit, lhs);
    int b_phys = x64_phys_of(emit, rhs);
    if (emit->failed)
        return;
    x64_emit_gpr_binary(emit, instr_index, info, a_phys, b_phys, size);
}

/* Whether the compare at `instr_index` hands its flags to the BR_COND or
 * SELECT right after it (fused_jcc) instead of materializing a boolean. */
static bool x64_fuses(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    return anvil_mir_isel_feeds_next_branch(&emit->isel, instr_index, info) || anvil_mir_isel_feeds_next_select(&emit->isel, instr_index, info);
}

static void x64_emit_cmp(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *lhs_info = x64_vreg_info_checked(emit, lhs);
    int dst_phys = x64_phys_of(emit, info->def);
    if (!lhs_info || emit->failed)
        return;

    const char *dst8 = x64_gpr_name(dst_phys, 1);
    const char *dst32 = x64_gpr_name(dst_phys, 4);

    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x64_size_bytes(lhs_info->size_bits);
        const char *a = x64_reg_name(emit, lhs);
        const char *b = x64_reg_name(emit, rhs);
        if (emit->failed)
            return;
        /* A branch right after tests the predicates one condition decides
         * (x86_gas_fcmp_branch); OEQ and UNE stay materialized. */
        const char *branch_cc;
        bool swap;
        if (info->op == ANVIL_MIR_OP_FCMP && x86_gas_fcmp_branch((anvil_fcmp_pred_t)info->imm, &branch_cc, &swap) && x64_fuses(emit, instr_index, info)) {
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", size <= 4 ? "ucomiss" : "ucomisd", swap ? a : b, swap ? b : a);
            emit->fused_jcc = branch_cc;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", size <= 4 ? "ucomiss" : "ucomisd", b, a);
        if (info->op == ANVIL_MIR_OP_FCMP) {
            x86_gas_emit_fcmp_bool(&emit->code, emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->mir), instr_index,
                                   (anvil_fcmp_pred_t)info->imm, dst8);
        } else {
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n", x86_gas_setcc(info->op), dst8);
        }
        anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", dst8, dst32);
        return;
    }

    int size = x64_size_bytes(lhs_info->size_bits);
    const char *suf = x64_size_suffix(size);
    const char *a = x64_gpr_name(x64_phys_of(emit, lhs), size);
    const char *b = x64_gpr_name(x64_phys_of(emit, rhs), size);
    if (emit->failed)
        return;
    int64_t imm = 0;
    if (x64_known_imm32(emit, rhs, &imm))
        anvil_strbuf_appendf(&emit->code, "\tcmp%s $%lld, %%%s\n", suf, (long long)imm, a);
    else
        anvil_strbuf_appendf(&emit->code, "\tcmp%s %%%s, %%%s\n", suf, b, a);

    /* Consumed only by the branch or select right after it: use the flags. */
    if (x64_fuses(emit, instr_index, info)) {
        emit->fused_jcc = x86_gas_jcc(info->op);
        return;
    }
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n", x86_gas_setcc(info->op), dst8);
    anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", dst8, dst32);
}

static void x64_emit_cond_branch(x64_mir_emit_t *emit, const char *cc, anvil_mir_block_t true_block, anvil_mir_block_t false_block)
{
    emit->failed |= !x86_gas_emit_cond_branch(&emit->code, emit->desc->format->local_label_prefix, emit->mir, emit->block, cc, true_block,
                                              false_block);
}

/* dst = cond ? then : else on general registers: a conditional move on the
 * flags of the fused compare right before (or a test of the boolean),
 * moving the other value in first (mov leaves the flags); a destination
 * already holding `then` takes `else` on the inverse condition.  Bytes and
 * words move as doublewords (no 8-bit cmov). */
static void x64_emit_select(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t cond = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t then_v = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    anvil_mir_vreg_t else_v = anvil_mir_get_instr_use(emit->mir, instr_index, 2);
    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    if (cond == ANVIL_MIR_NO_VREG || then_v == ANVIL_MIR_NO_VREG || else_v == ANVIL_MIR_NO_VREG || !def_info) {
        emit->failed = true;
        return;
    }
    int size = x64_size_bytes(def_info->size_bits) == 8 ? 8 : 4;
    int dst = x64_phys_of(emit, info->def), then_reg = x64_phys_of(emit, then_v), else_reg = x64_phys_of(emit, else_v);
    const char *cc = emit->fused_jcc;
    emit->fused_jcc = NULL;
    if (!cc) {
        const char *flag = x64_gpr_name(x64_phys_of(emit, cond), 1);
        if (!emit->failed)
            anvil_strbuf_appendf(&emit->code, "\ttestb %%%s, %%%s\n", flag, flag);
        cc = "ne";
    }
    const char *inverse = x86_gas_inverse_jcc(cc), *suf = x64_size_suffix(size);
    if (emit->failed || !inverse) {
        emit->failed = true;
        return;
    }
    if (dst == then_reg) {
        anvil_strbuf_appendf(&emit->code, "\tcmov%s%s %%%s, %%%s\n", inverse, suf, x64_gpr_name(else_reg, size), x64_gpr_name(dst, size));
        return;
    }
    if (dst != else_reg)
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, x64_gpr_name(else_reg, size), x64_gpr_name(dst, size));
    anvil_strbuf_appendf(&emit->code, "\tcmov%s%s %%%s, %%%s\n", cc, suf, x64_gpr_name(then_reg, size), x64_gpr_name(dst, size));
}

static void x64_emit_unary(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    if (!def_info || emit->failed)
        return;

    if (info->op == ANVIL_MIR_OP_NEG) {
        if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
            int size = x64_size_bytes(def_info->size_bits);
            const char *dst = x64_reg_name(emit, info->def);
            const char *s = x64_reg_name(emit, src);
            if (emit->failed)
                return;
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_copy_op(), s, dst);
            anvil_strbuf_appendf(&emit->code, "\t%s %s%s_fneg_mask%s(%%rip), %%%s\n", size <= 4 ? "xorps" : "xorpd", emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->mir), size <= 4 ? "32" : "64", dst);
            emit->emitted_fneg_mask = true;
        } else {
            int size = x64_size_bytes(def_info->size_bits);
            const char *suf = x64_size_suffix(size);
            const char *dst = x64_gpr_name(x64_phys_of(emit, info->def), size);
            const char *s = x64_gpr_name(x64_phys_of(emit, src), size);
            if (emit->failed)
                return;
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, s, dst);
            anvil_strbuf_appendf(&emit->code, "\tneg%s %%%s\n", suf, dst);
        }
    } else if (info->op == ANVIL_MIR_OP_FABS && def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x64_size_bytes(def_info->size_bits);
        const char *dst = x64_reg_name(emit, info->def);
        const char *s = x64_reg_name(emit, src);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_copy_op(), s, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s %s%s_fabs_mask%s(%%rip), %%%s\n", size <= 4 ? "andps" : "andpd", emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->mir), size <= 4 ? "32" : "64", dst);
        emit->emitted_fabs_mask = true;
    } else if (info->op == ANVIL_MIR_OP_NOT && def_info->reg_class == ANVIL_MIR_REG_GPR) {
        int size = x64_size_bytes(def_info->size_bits);
        const char *suf = x64_size_suffix(size);
        const char *dst = x64_gpr_name(x64_phys_of(emit, info->def), size);
        const char *s = x64_gpr_name(x64_phys_of(emit, src), size);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, s, dst);
        anvil_strbuf_appendf(&emit->code, "\tnot%s %%%s\n", suf, dst);
    } else {
        emit->failed = true;
    }
}

/* Whether the emitters write a 32-bit result of `op` with a 32-bit
 * instruction, which clears bits 63..32. */
static bool x64_writes_zero_extended(anvil_mir_opcode_t op)
{
    switch (op) {
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_MUL:
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR:
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD:
    case ANVIL_MIR_OP_LOAD:
    case ANVIL_MIR_OP_MOV:
        return true;
    default:
        return false;
    }
}

static void x64_emit_gpr_extend(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info, bool sign_extend)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = x64_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = x64_vreg_info_checked(emit, src);
    int dst_phys = x64_phys_of(emit, info->def);
    int src_phys = x64_phys_of(emit, src);
    if (!dst_info || !src_info || emit->failed)
        return;
    if (dst_info->reg_class != ANVIL_MIR_REG_GPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        emit->failed = true;
        return;
    }

    int src_size = x64_size_bytes(src_info->size_bits);
    int dst_size = x64_size_bytes(dst_info->size_bits);
    if (dst_size < src_size)
        dst_size = src_size;

    /* A narrow load of the same signedness already extended the value
     * (x64_emit_load: movs*l to 32 bits, movz*l to 64). */
    if (src_size < 4 && src_info->is_signed == sign_extend && (!sign_extend || dst_size <= 4) &&
        anvil_mir_isel_def_op(&emit->isel, src) == ANVIL_MIR_OP_LOAD) {
        if (src_phys != dst_phys)
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x64_gpr_name(src_phys, 4), x64_gpr_name(dst_phys, 4));
        return;
    }

    if (sign_extend) {
        if (src_size == dst_size) {
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", x64_size_suffix(dst_size), x64_gpr_name(src_phys, dst_size), x64_gpr_name(dst_phys, dst_size));
            return;
        }
        const char *suffix = src_size == 1 ? (dst_size <= 4 ? "bl" : "bq") : src_size == 2 ? (dst_size <= 4 ? "wl" : "wq") : "lq";
        anvil_strbuf_appendf(&emit->code, "\tmovs%s %%%s, %%%s\n", suffix, x64_gpr_name(src_phys, src_size), x64_gpr_name(dst_phys, dst_size));
        return;
    }

    if (src_size >= 4) {
        /* A 32-bit operation already zeroed the upper half (an omitted
         * TRUNC or COPY in the same register did not). */
        if (src_phys == dst_phys && x64_writes_zero_extended(anvil_mir_isel_def_op(&emit->isel, src)))
            return;
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x64_gpr_name(src_phys, 4), x64_gpr_name(dst_phys, 4));
        return;
    }
    const char *suffix = src_size == 1 ? "bl" : "wl";
    anvil_strbuf_appendf(&emit->code, "\tmovz%s %%%s, %%%s\n", suffix, x64_gpr_name(src_phys, src_size), x64_gpr_name(dst_phys, 4));
}

static void x64_emit_cast(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = x64_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = x64_vreg_info_checked(emit, src);
    if (!dst_info || !src_info)
        return;

    switch (info->op) {
    case ANVIL_MIR_OP_ZEXT:
        x64_emit_gpr_extend(emit, instr_index, info, false);
        break;
    case ANVIL_MIR_OP_SEXT:
        x64_emit_gpr_extend(emit, instr_index, info, true);
        break;
    case ANVIL_MIR_OP_TRUNC:
    case ANVIL_MIR_OP_BITCAST:
        x64_emit_copy(emit, info->def, src);
        break;
    case ANVIL_MIR_OP_SITOFP: {
        if (dst_info->reg_class != ANVIL_MIR_REG_FPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
            emit->failed = true;
            return;
        }
        int dst_size = x64_size_bytes(dst_info->size_bits);
        int src_size = x64_size_bytes(src_info->size_bits);
        if (src_size < 4)
            src_size = 4;
        const char *dst = x64_reg_name(emit, info->def);
        const char *s = x64_gpr_name(x64_phys_of(emit, src), src_size);
        if (emit->failed)
            return;
        /* Conversions into an XMM register merge into its upper bits:
         * zeroing it first breaks the dependency on its old value. */
        anvil_strbuf_appendf(&emit->code, "\txorps %%%s, %%%s\n", dst, dst);
        anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%s%s %%%s, %%%s\n", dst_size <= 4 ? "s" : "d", src_size <= 4 ? "l" : "q", s, dst);
        break;
    }
    case ANVIL_MIR_OP_UITOFP: {
        if (dst_info->reg_class != ANVIL_MIR_REG_FPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
            emit->failed = true;
            return;
        }
        int dst_size = x64_size_bytes(dst_info->size_bits);
        int src_size = x64_size_bytes(src_info->size_bits);
        const char *dst = x64_reg_name(emit, info->def);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\txorps %%%s, %%%s\n", dst, dst);
        if (src_size <= 4) {
            const char *s = x64_gpr_name(x64_phys_of(emit, src), 4);
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%r11d\n", s);
            anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%sq %%r11, %%%s\n", dst_size <= 4 ? "s" : "d", dst);
        } else {
            /* Values with bit 63 set convert at half scale, keeping the low
             * bit for rounding, and are then doubled. */
            const char *s = x64_gpr_name(x64_phys_of(emit, src), 8);
            const char *sd = dst_size <= 4 ? "s" : "d";
            anvil_strbuf_appendf(&emit->code, "\ttestq %%%s, %%%s\n\tjs 1f\n", s, s);
            anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%sq %%%s, %%%s\n\tjmp 2f\n", sd, s, dst);
            anvil_strbuf_appendf(&emit->code, "1:\tmovq %%%s, %%r11\n\tshrq %%r11\n", s);
            anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %%rax\n\tandl $1, %%eax\n\torq %%rax, %%r11\n", s);
            anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%sq %%r11, %%%s\n\tadds%s %%%s, %%%s\n2:\n", sd, dst, sd, dst, dst);
        }
        break;
    }
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI: {
        if (dst_info->reg_class != ANVIL_MIR_REG_GPR || src_info->reg_class != ANVIL_MIR_REG_FPR) {
            emit->failed = true;
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
        const char *dst = x64_gpr_name(x64_phys_of(emit, info->def), dst_size <= 4 && !is_unsigned ? 4 : 8);
        if (emit->failed)
            return;
        if (!is_unsigned || dst_size <= 4) {
            anvil_strbuf_appendf(&emit->code, "\tcvtts%s2si %%%s, %%%s\n", sd, s, dst);
            break;
        }
        /* From 2^63 up: convert x - 2^63 and set bit 63.  Both steps are
         * exact in that range, so the source is restored afterwards. */
        const char *limit = x64_sse_temp(emit);
        if (src_size <= 4)
            anvil_strbuf_appendf(&emit->code, "\tmovl $0x5f000000, %%eax\n\tmovd %%eax, %%%s\n", limit);
        else
            anvil_strbuf_appendf(&emit->code, "\tmovabsq $0x43e0000000000000, %%rax\n\tmovq %%rax, %%%s\n", limit);
        anvil_strbuf_appendf(&emit->code, "\tucomis%s %%%s, %%%s\n\tjae 1f\n", sd, limit, s);
        anvil_strbuf_appendf(&emit->code, "\tcvtts%s2si %%%s, %%%s\n\tjmp 2f\n", sd, s, dst);
        anvil_strbuf_appendf(&emit->code, "1:\tsubs%s %%%s, %%%s\n\tcvtts%s2si %%%s, %%%s\n", sd, limit, s, sd, s, dst);
        anvil_strbuf_appendf(&emit->code, "\tadds%s %%%s, %%%s\n\tbtcq $63, %%%s\n2:\n", sd, limit, s, dst);
        break;
    }
    case ANVIL_MIR_OP_FPEXT: {
        const char *dst = x64_reg_name(emit, info->def);
        const char *s = x64_reg_name(emit, src);
        if (emit->failed)
            return;
        if (strcmp(s, dst) != 0)
            anvil_strbuf_appendf(&emit->code, "\txorps %%%s, %%%s\n", dst, dst);
        anvil_strbuf_appendf(&emit->code, "\tcvtss2sd %%%s, %%%s\n", s, dst);
        break;
    }
    case ANVIL_MIR_OP_FPTRUNC: {
        const char *dst = x64_reg_name(emit, info->def);
        const char *s = x64_reg_name(emit, src);
        if (emit->failed)
            return;
        if (strcmp(s, dst) != 0)
            anvil_strbuf_appendf(&emit->code, "\txorps %%%s, %%%s\n", dst, dst);
        anvil_strbuf_appendf(&emit->code, "\tcvtsd2ss %%%s, %%%s\n", s, dst);
        break;
    }
    default:
        emit->failed = true;
        break;
    }
}

static void x64_emit_frame_addr(x64_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (info->frame_slot < 0 || (size_t)info->frame_slot >= emit->num_frame_slot_offsets) {
        emit->failed = true;
        return;
    }

    int dst_phys = x64_phys_of(emit, info->def);
    if (emit->failed)
        return;

    int offset = emit->frame_slot_offsets[info->frame_slot];
    anvil_strbuf_appendf(&emit->code, "\tleaq %d(%%rbp), %%%s\n", x64_frame_disp(emit, offset), x64_gpr_name(dst_phys, 8));
}

static void x64_emit_dyn_alloca(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm <= 0 || info->num_uses != 1) {
        emit->failed = true;
        return;
    }

    anvil_mir_vreg_t count = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (count == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const char *count_reg = x64_gpr_name(x64_phys_of(emit, count), 8);
    int dst_phys = x64_phys_of(emit, info->def);
    if (emit->failed)
        return;

    anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %%r11\n", count_reg);
    if (info->imm != 1) {
        anvil_strbuf_appendf(&emit->code, "\timulq $%lld, %%r11, %%r11\n", (long long)info->imm);
    }
    anvil_strbuf_append(&emit->code, "\taddq $15, %r11\n");
    anvil_strbuf_append(&emit->code, "\tandq $-16, %r11\n");
    /* Relocate the outgoing area below each dynamic allocation. Otherwise
     * CALL_STACK_ARG (and Win64 callee home stores) overwrite the buffer. */
    if (emit->outgoing_size)
        anvil_strbuf_appendf(&emit->code, "\taddq $%d, %%r11\n", emit->outgoing_size);
    if (emit->desc->is_win64)
        x64_emit_stack_probe(emit, instr_index);
    anvil_strbuf_append(&emit->code, "\tsubq %r11, %rsp\n");
    anvil_strbuf_appendf(&emit->code, "\tleaq %d(%%rsp), %%%s\n", emit->outgoing_size, x64_gpr_name(dst_phys, 8));
}

static void x64_emit_symbol_addr(x64_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->symbol || !info->symbol[0]) {
        emit->failed = true;
        return;
    }

    int dst_phys = x64_phys_of(emit, info->def);
    if (emit->failed)
        return;

    const char *prefix = x64_symbol_ref_prefix(emit, info->symbol);
    const char *dst = x64_gpr_name(dst_phys, 8);
    if (info->symbol_got) {
        /* The GOT slot holds the symbol's final address. */
        anvil_strbuf_appendf(&emit->code, "\tmovq %s%s@GOTPCREL(%%rip), %%%s\n", prefix, info->symbol, dst);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->code, "\tleaq %lld(%%%s), %%%s\n", (long long)info->imm, dst, dst);
        return;
    }
    if (x64_symbol_is_local(info->symbol)) {
        anvil_strbuf_appendf(&emit->code, "\tleaq %s%s", prefix, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
        anvil_strbuf_appendf(&emit->code, "(%%rip), %%%s\n", dst);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tleaq %s%s", prefix, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
        anvil_strbuf_appendf(&emit->code, "(%%rip), %%%s\n", dst);
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

static const char *x64_mem_store_op(anvil_mir_reg_class_t reg_class, int size)
{
    if (reg_class == ANVIL_MIR_REG_FPR) {
        return x64_fp_mov_op(size);
    }
    switch (size) {
    case 1:
        return "movb";
    case 2:
        return "movw";
    case 4:
        return "movl";
    default:
        return "movq";
    }
}

/* disp(%base) or disp(%base,%index,scale) of a LOAD/STORE whose base is use
 * `base_use` (an index, when present, follows it). */
static bool x64_memory_operand(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info, size_t base_use, char *out,
                               size_t out_len)
{
    anvil_mir_vreg_t base = anvil_mir_get_instr_use(emit->mir, instr_index, base_use);
    if (base == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return false;
    }
    const char *base_name = x64_gpr_name(x64_phys_of(emit, base), 8);
    char disp[24] = "";
    if (info->has_imm && info->imm != 0)
        snprintf(disp, sizeof(disp), "%lld", (long long)info->imm);
    if (info->index_scale) {
        anvil_mir_vreg_t index = anvil_mir_get_instr_use(emit->mir, instr_index, base_use + 1);
        if (index == ANVIL_MIR_NO_VREG) {
            emit->failed = true;
            return false;
        }
        snprintf(out, out_len, "%s(%%%s,%%%s,%u)", disp, base_name, x64_gpr_name(x64_phys_of(emit, index), 8), info->index_scale);
    } else {
        snprintf(out, out_len, "%s(%%%s)", disp, base_name);
    }
    return !emit->failed;
}

static void x64_emit_load(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    int def_phys = x64_phys_of(emit, info->def);
    char address[96];
    if (!def_info || emit->failed || !x64_memory_operand(emit, instr_index, info, 0, address, sizeof(address)))
        return;

    int size = x64_size_bytes(def_info->size_bits);
    const char *op = x64_mem_load_op(def_info->reg_class, size, def_info->is_signed, size);
    const char *dst;
    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        dst = x64_xmm_names[def_phys];
    } else if (!def_info->is_signed && size < 4) {
        dst = x64_gpr_name(def_phys, 4);
    } else if (def_info->is_signed && size <= 2) {
        dst = x64_gpr_name(def_phys, 4);
    } else {
        dst = x64_gpr_name(def_phys, size);
    }
    anvil_strbuf_appendf(&emit->code, "\t%s %s, %%%s\n", op, address, dst);
}

static void x64_emit_store(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (value == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const anvil_mir_vreg_info_t *value_info = x64_vreg_info_checked(emit, value);
    char address[96];
    if (!value_info || emit->failed || !x64_memory_operand(emit, instr_index, info, 1, address, sizeof(address)))
        return;

    int size = x64_size_bytes(value_info->size_bits);
    const char *op = x64_mem_store_op(value_info->reg_class, size);
    const char *src = value_info->reg_class == ANVIL_MIR_REG_FPR ? x64_reg_name(emit, value) : x64_gpr_name(x64_phys_of(emit, value), size);
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %s\n", op, src, address);
}

static void x64_emit_incoming_stack_arg(x64_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm < 0 || info->imm > INT32_MAX - 32) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    int def_phys = x64_phys_of(emit, info->def);
    if (!def_info || emit->failed)
        return;

    int size = x64_size_bytes(def_info->size_bits);
    int shadow = emit->desc->shadow_space;
    int frame_offset = 16 + shadow + (int)info->imm;
    if (emit->desc->is_win64)
        frame_offset += emit->frame_size;
    const char *op = x64_mem_load_op(def_info->reg_class, size, def_info->is_signed, size);
    const char *dst;
    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        dst = x64_xmm_names[def_phys];
    } else if (!def_info->is_signed && size < 4) {
        dst = x64_gpr_name(def_phys, 4);
    } else if (def_info->is_signed && size <= 2) {
        dst = x64_gpr_name(def_phys, 4);
    } else {
        dst = x64_gpr_name(def_phys, size);
    }
    anvil_strbuf_appendf(&emit->code, "\t%s %d(%%rbp), %%%s\n", op, frame_offset, dst);
}

static void x64_emit_call_stack_arg(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm < 0 || info->num_uses != 1 || info->imm > INT32_MAX) {
        emit->failed = true;
        return;
    }

    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (value == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *value_info = x64_vreg_info_checked(emit, value);
    if (!value_info || emit->failed)
        return;

    int size = x64_size_bytes(value_info->size_bits);
    const char *op = x64_mem_store_op(value_info->reg_class, size);
    const char *src;
    if (value_info->reg_class == ANVIL_MIR_REG_FPR) {
        src = x64_reg_name(emit, value);
    } else {
        src = x64_gpr_name(x64_phys_of(emit, value), size);
    }
    if (emit->failed)
        return;
    int slot = emit->desc->shadow_space + (int)info->imm;
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %d(%%rsp)\n", op, src, slot);
}

static void x64_emit_call(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if ((info->call_cc == ANVIL_CC_WIN64 && !emit->desc->is_win64) || (info->call_cc == ANVIL_CC_SYSV && emit->desc->is_win64) || (info->call_cc != ANVIL_CC_SYSV && info->call_cc != ANVIL_CC_WIN64)) {
        emit->failed = true;
        return;
    }
    bool direct = info->symbol && info->symbol[0];
    if (!emit->desc->is_win64 && info->has_imm) {
        if (info->imm < 0 || info->imm > 8) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\tmovb $%lld, %%al\n", (long long)info->imm);
    }

    if (direct) {
        anvil_strbuf_appendf(&emit->code, "\tcall %s%s%s\n", x64_symbol_ref_prefix(emit, info->symbol), info->symbol,
                             info->symbol_got && emit->desc->plt_suffix ? emit->desc->plt_suffix : "");
        return;
    }

    anvil_mir_vreg_t target = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (target == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const char *target_reg = x64_gpr_name(x64_phys_of(emit, target), 8);
    if (emit->failed)
        return;
    anvil_strbuf_appendf(&emit->code, "\tcall *%%%s\n", target_reg);
}

static void x64_emit_spill_load(x64_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (info->spill_slot < 0 || (size_t)info->spill_slot >= emit->num_spill_offsets) {
        emit->failed = true;
        return;
    }

    anvil_mir_spill_slot_info_t slot;
    if (!anvil_mir_get_spill_slot_info(emit->mir, info->spill_slot, &slot)) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    int def_phys = x64_phys_of(emit, info->def);
    if (!def_info || emit->failed)
        return;

    int offset = x64_frame_disp(emit, emit->spill_offsets[info->spill_slot]);
    int size = x64_size_bytes(slot.size_bits);
    if (slot.reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\t%s %d(%%rbp), %%%s\n", x64_fp_mov_op(size), offset, x64_xmm_names[def_phys]);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmov%s %d(%%rbp), %%%s\n", x64_size_suffix(size), offset, x64_gpr_name(def_phys, size));
    }
}

static void x64_emit_spill_store(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (info->spill_slot < 0 || (size_t)info->spill_slot >= emit->num_spill_offsets) {
        emit->failed = true;
        return;
    }

    anvil_mir_vreg_t src_vreg = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src_vreg == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    anvil_mir_spill_slot_info_t slot;
    if (!anvil_mir_get_spill_slot_info(emit->mir, info->spill_slot, &slot)) {
        emit->failed = true;
        return;
    }

    int src_phys = x64_phys_of(emit, src_vreg);
    if (emit->failed)
        return;

    int offset = x64_frame_disp(emit, emit->spill_offsets[info->spill_slot]);
    int size = x64_size_bytes(slot.size_bits);
    if (slot.reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %d(%%rbp)\n", x64_fp_mov_op(size), x64_xmm_names[src_phys], offset);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %d(%%rbp)\n", x64_size_suffix(size), x64_gpr_name(src_phys, size), offset);
    }
}

static void x64_emit_ret(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (info->num_uses > 0) {
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        const anvil_mir_vreg_info_t *ret_info = x64_vreg_info_checked(emit, ret);
        int ret_phys = x64_phys_of(emit, ret);
        if (!ret_info || emit->failed)
            return;

        if (ret_info->reg_class == ANVIL_MIR_REG_FPR) {
            if (ret_phys != emit->desc->fp_ret_reg) {
                anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_copy_op(), x64_xmm_names[ret_phys], x64_xmm_names[emit->desc->fp_ret_reg]);
            }
        } else {
            int size = x64_size_bytes(ret_info->size_bits);
            if (size < 4)
                size = 4;
            if (ret_phys != emit->desc->int_ret_reg) {
                anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", x64_size_suffix(size), x64_gpr_name(ret_phys, size), x64_gpr_name(emit->desc->int_ret_reg, size));
            }
        }
    }
    if (!emit->failed)
        x64_emit_epilogue(emit);
}

/* Gather implicit operands without assuming their allocated registers differ
 * from the instruction's scratch registers. Cycles use register exchanges. */
static void x64_parallel_move_reg(void *user, int dst, int src)
{
    x64_mir_emit_t *emit = user;
    anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %%%s\n", x64_gpr_name(src, 8), x64_gpr_name(dst, 8));
}

static void x64_parallel_swap_reg(void *user, int a, int b)
{
    x64_mir_emit_t *emit = user;
    anvil_strbuf_appendf(&emit->code, "\txchgq %%%s, %%%s\n", x64_gpr_name(a, 8), x64_gpr_name(b, 8));
}

/* Pointer -> RDX, operand -> RCX, expected -> RAX (CMPXCHG: expected in
 * RAX, desired in RCX), as one parallel move. */
static void x64_atomic_inputs(x64_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info)
{
    static const anvil_parallel_move_ops_t ops = { x64_parallel_move_reg, x64_parallel_swap_reg, -1 };
    int destinations[] = {X64_RDX, X64_RCX, X64_RAX};
    if (info->ir_op == ANVIL_OP_ATOMIC_CMPXCHG) {
        destinations[1] = X64_RAX;
        destinations[2] = X64_RCX;
    }

    int sources[3];
    size_t count = info->num_uses < 3 ? info->num_uses : 3;
    for (size_t operand = 0; operand < count; operand++)
        sources[operand] = x64_phys_of(emit, anvil_mir_get_instr_use(emit->mir, index, operand));
    if (!emit->failed && !anvil_parallel_move(&ops, emit, destinations, sources, count))
        emit->failed = true;
}

static void x64_emit_atomic(x64_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info)
{
    if (info->ir_op == ANVIL_OP_ATOMIC_FENCE) {
        if (info->atomic.order == ANVIL_ORDER_SEQ_CST)
            anvil_strbuf_append(&emit->code, "\tmfence\n");

        return;
    }

    anvil_mir_vreg_t value = info->def;
    if (value == ANVIL_MIR_NO_VREG)
        value = anvil_mir_get_instr_use(emit->mir, index, 1);

    const anvil_mir_vreg_info_t *type = x64_vreg_info_checked(emit, value);
    if (!type)
        return;

    int size = x64_size_bytes(type->size_bits);
    const char *suffix = x64_size_suffix(size);
    const char *accumulator = x64_gpr_name(X64_RAX, size);
    const char *operand = x64_gpr_name(X64_RCX, size);
    const char *temporary = x64_gpr_name(X64_R11, size);
    x64_atomic_inputs(emit, index, info);
    if (emit->failed)
        return;

    switch (info->ir_op) {
    case ANVIL_OP_ATOMIC_LOAD:
        anvil_strbuf_appendf(&emit->code, "\tmov%s (%%rdx), %%%s\n", suffix, accumulator);
        break;
    case ANVIL_OP_ATOMIC_STORE:
        anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, (%%rdx)\n", info->atomic.order == ANVIL_ORDER_SEQ_CST ? "xchg" : "mov", suffix, operand);
        return;
    case ANVIL_OP_ATOMIC_CMPXCHG:
        anvil_strbuf_appendf(&emit->code, "\tlock cmpxchg%s %%%s, (%%rdx)\n", suffix, operand);
        break;
    case ANVIL_OP_ATOMIC_RMW:
        if (info->atomic.rmw <= ANVIL_ATOMIC_SUB) {
            if (info->atomic.rmw == ANVIL_ATOMIC_SUB)
                anvil_strbuf_appendf(&emit->code, "\tneg%s %%%s\n", suffix, operand);

            const char *operation = info->atomic.rmw == ANVIL_ATOMIC_EXCHANGE ? "xchg" : "lock xadd";
            anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, (%%rdx)\n\tmov%s %%%s, %%%s\n", operation, suffix, operand, suffix, operand, accumulator);
        } else {
            const char *operation = info->atomic.rmw == ANVIL_ATOMIC_AND ? "and" : (info->atomic.rmw == ANVIL_ATOMIC_OR ? "or" : "xor");
            const char *function = anvil_mir_func_name(emit->mir);
            anvil_strbuf_appendf(&emit->code, "\tmov%s (%%rdx), %%%s\n%s%s_atomic_%zu:\n", suffix, accumulator, emit->desc->format->local_label_prefix, function, index);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n\t%s%s %%%s, %%%s\n", suffix, accumulator, temporary, operation, suffix, operand, temporary);
            anvil_strbuf_appendf(&emit->code, "\tlock cmpxchg%s %%%s, (%%rdx)\n\tjne %s%s_atomic_%zu\n", suffix, temporary, emit->desc->format->local_label_prefix, function, index);
        }
        break;
    default:
        emit->failed = true;
        return;
    }

    /* Narrow results are kept extended, as loads leave them. */
    int def_phys = x64_phys_of(emit, info->def);
    if (emit->failed)
        return;
    if (size < 4)
        anvil_strbuf_appendf(&emit->code, "\tmov%c%cl %%%s, %%%s\n", type->is_signed ? 's' : 'z', size == 1 ? 'b' : 'w', accumulator,
                             x64_gpr_name(def_phys, 4));
    else
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suffix, accumulator, x64_gpr_name(def_phys, size));
}

static void x64_emit_instr(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    switch (info->op) {
    case ANVIL_MIR_OP_ATOMIC:
        x64_emit_atomic(emit, instr_index, info);
        return;

    case ANVIL_MIR_OP_MOV:
        x64_emit_mov(emit, info);
        break;
    case ANVIL_MIR_OP_COPY: {
        anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        if (src == ANVIL_MIR_NO_VREG) {
            emit->failed = true;
            break;
        }
        x64_emit_copy(emit, info->def, src);
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
        x64_emit_binary(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_VECTOR:
    case ANVIL_MIR_OP_SCALAR:
        x64_emit_vector(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_NEG:
    case ANVIL_MIR_OP_NOT:
    case ANVIL_MIR_OP_FABS:
        x64_emit_unary(emit, instr_index, info);
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
        x64_emit_cmp(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SYMBOL_ADDR:
        x64_emit_symbol_addr(emit, info);
        break;
    case ANVIL_MIR_OP_LOAD:
        x64_emit_load(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_STORE:
        x64_emit_store(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_FRAME_ADDR:
        x64_emit_frame_addr(emit, info);
        break;
    case ANVIL_MIR_OP_VA_START: {
        if (!emit->desc->is_win64) {
            if (info->frame_slot < 0 || (size_t)info->frame_slot >= emit->num_frame_slot_offsets || info->named_gpr > 6 || info->named_fpr > 8 || info->named_stack_bytes > INT_MAX - 16) {
                emit->failed = true;
                break;
            }

            int base = -emit->frame_slot_offsets[info->frame_slot];
            int destination = x64_phys_of(emit, info->def);
            if (emit->failed)
                break;

            anvil_strbuf_appendf(&emit->code, "\tmovl $%u, %d(%%rbp)\n", info->named_gpr * 8, base);
            anvil_strbuf_appendf(&emit->code, "\tmovl $%u, %d(%%rbp)\n", 48 + info->named_fpr * 16, base + 4);
            anvil_strbuf_appendf(&emit->code, "\tleaq %zu(%%rbp), %%r11\n\tmovq %%r11, %d(%%rbp)\n", 16 + info->named_stack_bytes, base + 8);
            anvil_strbuf_appendf(&emit->code, "\tleaq %d(%%rbp), %%r11\n\tmovq %%r11, %d(%%rbp)\n", base + 32, base + 16);
            anvil_strbuf_appendf(&emit->code, "\tleaq %d(%%rbp), %%%s\n", base, x64_gpr_name(destination, 8));
            break;
        }

        if (!emit->desc->is_win64 || !info->has_imm || info->imm < 0 || emit->frame_size > INT_MAX - 16 || info->imm > (INT_MAX - 16 - emit->frame_size) / 8) {
            emit->failed = true;
            break;
        }

        int destination = x64_phys_of(emit, info->def);
        if (emit->failed)
            break;

        int offset = emit->frame_size + 16 + (int)info->imm * 8;
        anvil_strbuf_appendf(&emit->code, "\tleaq %d(%%rbp), %%%s\n", offset, x64_gpr_name(destination, 8));
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
        x64_emit_call_stack_arg(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_BR:
        emit->failed |= !x86_gas_emit_jump(&emit->code, emit->desc->format->local_label_prefix, emit->mir, emit->block, info->true_block);
        break;
    case ANVIL_MIR_OP_SELECT:
        x64_emit_select(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_BR_COND: {
        anvil_mir_vreg_t cond = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        if (cond == ANVIL_MIR_NO_VREG) {
            emit->failed = true;
            break;
        }
        if (emit->fused_jcc) {
            const char *cc = emit->fused_jcc;
            emit->fused_jcc = NULL;
            x64_emit_cond_branch(emit, cc, info->true_block, info->false_block);
            break;
        }
        const anvil_mir_vreg_info_t *cond_info = x64_vreg_info_checked(emit, cond);
        if (!cond_info)
            break;
        int size = x64_size_bytes(cond_info->size_bits);
        const char *cond_reg = x64_gpr_name(x64_phys_of(emit, cond), size);
        if (emit->failed)
            break;
        anvil_strbuf_appendf(&emit->code, "\ttest%s %%%s, %%%s\n", x64_size_suffix(size), cond_reg, cond_reg);
        x64_emit_cond_branch(emit, "ne", info->true_block, info->false_block);
        break;
    }
    case ANVIL_MIR_OP_RET:
        x64_emit_ret(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SPILL_LOAD:
        x64_emit_spill_load(emit, info);
        break;
    case ANVIL_MIR_OP_SPILL_STORE:
        x64_emit_spill_store(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_KEEPALIVE:
        break;
    case ANVIL_MIR_OP_CALL_RESULT:
        /* An additional fixed-register result of the preceding call: the
         * allocator placed it in that register, so nothing moves. */
        if (x64_phys_of(emit, info->def) < 0)
            emit->failed = true;
        break;
    case ANVIL_MIR_OP_RET_VALUE_PART:
        emit->failed = true;
        break;
    default:
        emit->failed = true;
        break;
    }
}

static void x64_emit_escaped_string(anvil_strbuf_t *code, const char *value)
{
    anvil_strbuf_append(code, "\t.asciz \"");
    for (const char *p = value ? value : ""; *p; p++) {
        switch (*p) {
        case '\n':
            anvil_strbuf_append(code, "\\n");
            break;
        case '\r':
            anvil_strbuf_append(code, "\\r");
            break;
        case '\t':
            anvil_strbuf_append(code, "\\t");
            break;
        case '\\':
            anvil_strbuf_append(code, "\\\\");
            break;
        case '"':
            anvil_strbuf_append(code, "\\\"");
            break;
        default:
            anvil_strbuf_append_char(code, *p);
            break;
        }
    }
    anvil_strbuf_append(code, "\"\n");
}

static void x64_emit_rodata(x64_mir_emit_t *emit)
{
    size_t count = anvil_mir_num_string_literals(emit->mir);
    if (count == 0 && !emit->emitted_fneg_mask && !emit->emitted_fabs_mask) {
        return;
    }

    if (count > 0)
        anvil_strbuf_append(&emit->code, emit->desc->format->cstring_section);

    for (size_t i = 0; i < count; i++) {
        anvil_mir_string_literal_info_t info;
        if (!anvil_mir_get_string_literal_info(emit->mir, i, &info) || !info.label) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "%s:\n", info.label);
        x64_emit_escaped_string(&emit->code, info.value);
    }

    if (emit->emitted_fneg_mask || emit->emitted_fabs_mask) {
        anvil_strbuf_append(&emit->code, emit->desc->format->literal16_section);
        anvil_obj_emit_align(&emit->code, 16);
    }
    if (emit->emitted_fneg_mask) {
        anvil_strbuf_appendf(&emit->code, "%s%s_fneg_mask32:\n", emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->mir));
        anvil_strbuf_append(&emit->code, "\t.long 0x80000000\n");
        anvil_strbuf_append(&emit->code, "\t.long 0\n\t.long 0\n\t.long 0\n");
        anvil_strbuf_appendf(&emit->code, "%s%s_fneg_mask64:\n", emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->mir));
        anvil_strbuf_append(&emit->code, "\t.quad 0x8000000000000000\n");
        anvil_strbuf_append(&emit->code, "\t.quad 0\n");
    }
    if (emit->emitted_fabs_mask) {
        anvil_strbuf_appendf(&emit->code, "%s%s_fabs_mask32:\n", emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->mir));
        anvil_strbuf_append(&emit->code, "\t.long 0x7fffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_appendf(&emit->code, "%s%s_fabs_mask64:\n", emit->desc->format->local_label_prefix, anvil_mir_func_name(emit->mir));
        anvil_strbuf_append(&emit->code, "\t.quad 0x7fffffffffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.quad 0xffffffffffffffff\n");
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
    emit.mir = mir;
    emit.desc = desc;
    emit.syntax = syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : syntax;
    anvil_strbuf_init(&emit.code);
    if (!emit.code.data)
        return false;

    if (!x64_prepare_frame(&emit) || !anvil_mir_isel_facts_build(&emit.isel, mir, x64_use_takes_immediate, &emit)) {
        anvil_strbuf_destroy(&emit.code);
        free(emit.spill_offsets);
        free(emit.frame_slot_offsets);
        anvil_mir_isel_facts_destroy(&emit.isel);
        return false;
    }

    x64_emit_prologue(&emit);

    size_t num_blocks = anvil_mir_num_blocks(mir);
    size_t num_instrs = anvil_mir_num_instrs(mir);
    /* Loop headers start aligned (padding outside the loop is cheap). */
    bool *loop_headers = calloc(anvil_mir_num_blocks(mir) ? anvil_mir_num_blocks(mir) : 1, sizeof(*loop_headers));
    if (!loop_headers || !anvil_mir_loop_header_blocks(mir, loop_headers))
        emit.failed = true;
    for (size_t b = 0; b < num_blocks && !emit.failed && !emit.code.failed; b++) {
        if (loop_headers && loop_headers[b])
            anvil_strbuf_append(&emit.code, "\t.p2align 4\n");
        if (!x86_gas_emit_label(&emit.code, emit.desc->format->local_label_prefix, mir, (anvil_mir_block_t)b)) {
            emit.failed = true;
            break;
        }
        emit.block = (anvil_mir_block_t)b;

        for (size_t i = 0; i < num_instrs && !emit.failed && !emit.code.failed; i++) {
            anvil_mir_instr_info_t info;
            if (!anvil_mir_get_instr_info(mir, i, &info)) {
                emit.failed = true;
                break;
            }
            if (info.block != (anvil_mir_block_t)b)
                continue;
            x64_emit_instr(&emit, i, &info);
        }
    }
    free(loop_headers);

    if (!emit.failed && !emit.code.failed && emit.desc->format->cfi)
        anvil_strbuf_append(&emit.code, "\t.cfi_endproc\n");
    if (!emit.failed && !emit.code.failed && emit.desc->is_win64) {
        anvil_strbuf_append(&emit.code, "\t.seh_endproc\n");
    } else if (!emit.failed && !emit.code.failed && !emit.desc->is_darwin) {
        const char *prefix = x64_symbol_prefix(&emit);
        const char *name = anvil_mir_func_name(mir);
        anvil_strbuf_appendf(&emit.code, "\t.size %s%s, .-%s%s\n", prefix, name, prefix, name);
    }
    if (!emit.failed && !emit.code.failed) {
        x64_emit_rodata(&emit);
    }

    free(emit.spill_offsets);
    free(emit.frame_slot_offsets);
    anvil_mir_isel_facts_destroy(&emit.isel);
    if (emit.failed || emit.code.failed) {
        anvil_strbuf_destroy(&emit.code);
        return false;
    }

    *output = anvil_strbuf_detach(&emit.code, len);
    return *output != NULL;
}

bool anvil_x86_64_emit_mir(const anvil_mir_func_t *mir, char **output, size_t *len)
{
    return anvil_x86_64_emit_mir_abi(mir, ANVIL_ABI_SYSV, ANVIL_SYNTAX_GAS, output, len);
}
