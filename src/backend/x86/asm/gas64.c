#include "../x86_64_internal.h"
#include "anvil/anvil_analysis.h"

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
} x64_mir_emit_t;

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

static int x64_slot_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0)
        return 8;
    int size = (int)((size_bits + 7) / 8);
    return size > 0 ? size : 8;
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

static bool x64_emit_label(x64_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info))
        return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s:\n", anvil_mir_func_name(emit->mir), name);
    return true;
}

static bool x64_emit_branch_target(x64_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info))
        return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s", anvil_mir_func_name(emit->mir), name);
    return true;
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

static const char *x64_setcc(anvil_mir_opcode_t op)
{
    switch (op) {
    case ANVIL_MIR_OP_CMP_EQ:
        return "sete";
    case ANVIL_MIR_OP_CMP_NE:
    case ANVIL_MIR_OP_CMP:
        return "setne";
    case ANVIL_MIR_OP_CMP_LT:
        return "setl";
    case ANVIL_MIR_OP_CMP_LE:
        return "setle";
    case ANVIL_MIR_OP_CMP_GT:
        return "setg";
    case ANVIL_MIR_OP_CMP_GE:
        return "setge";
    case ANVIL_MIR_OP_CMP_ULT:
        return "setb";
    case ANVIL_MIR_OP_CMP_ULE:
        return "setbe";
    case ANVIL_MIR_OP_CMP_UGT:
        return "seta";
    case ANVIL_MIR_OP_CMP_UGE:
        return "setae";
    default:
        return "setne";
    }
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

    for (size_t i = 0; i < emit->num_frame_slot_offsets; i++) {
        anvil_mir_frame_slot_info_t slot;
        if (!anvil_mir_get_frame_slot_info(emit->mir, (int)i, &slot)) {
            return false;
        }
        int align = slot.align_bytes ? slot.align_bytes : 8;
        if (align > 16)
            align = 16;
        offset = x64_align_int(offset, align);
        offset += x64_slot_size_bytes(slot.size_bits);
        emit->frame_slot_offsets[i] = offset;
    }

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
    emit->has_frame = true;
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
    anvil_strbuf_appendf(&emit->code, ".L%s_probe_%zu_loop:\n", name, id);
    anvil_strbuf_append(&emit->code, "\tsubq $4096, %rax\n\tcmpq %r10, %rax\n");
    anvil_strbuf_appendf(&emit->code, "\tjb .L%s_probe_%zu_done\n", name, id);
    anvil_strbuf_append(&emit->code, "\ttestb $0, (%rax)\n");
    anvil_strbuf_appendf(&emit->code, "\tjmp .L%s_probe_%zu_loop\n", name, id);
    anvil_strbuf_appendf(&emit->code, ".L%s_probe_%zu_done:\n", name, id);
    anvil_strbuf_append(&emit->code, "\ttestb $0, (%r10)\n");
}

static void x64_emit_prologue(x64_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);
    const char *prefix = x64_symbol_prefix(emit);
    if (emit->desc->is_darwin) {
        anvil_strbuf_append(&emit->code, "\t.section __TEXT,__text,regular,pure_instructions\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s%s\n", prefix, name);
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    } else if (emit->desc->is_win64) {
        anvil_strbuf_append(&emit->code, "\t.text\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s%s\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "\t.def %s%s; .scl 2; .type 32; .endef\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "\t.seh_proc %s%s\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    } else {
        anvil_strbuf_append(&emit->code, "\t.text\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s%s\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "\t.type %s%s, @function\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    }

    anvil_strbuf_append(&emit->code, "\tpushq %rbp\n");
    if (emit->desc->is_win64)
        anvil_strbuf_append(&emit->code, "\t.seh_pushreg %rbp\n");
    if (!emit->desc->is_win64)
        anvil_strbuf_append(&emit->code, "\tmovq %rsp, %rbp\n");
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
    anvil_strbuf_append(&emit->code, "\tret\n");
}

static const char *x64_fp_mov_op(int size)
{
    if (size == 16)
        return "movups";

    return size <= 4 ? "movss" : "movsd";
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
        int size = x64_size_bytes(dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_mov_op(size), src_reg, dst_reg);
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
    int dst_phys = x64_phys_of(emit, info->def);
    if (emit->failed)
        return;

    int64_t imm = info->has_imm ? info->imm : 0;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x64_size_bytes(def_info->size_bits);
        const char *dst = x64_xmm_names[dst_phys];
        if (size <= 4) {
            anvil_strbuf_appendf(&emit->code, "\tmovl $%u, %%r11d\n", (uint32_t)imm);
            anvil_strbuf_appendf(&emit->code, "\tmovd %%r11d, %%%s\n", dst);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tmovabsq $%llu, %%r11\n", (unsigned long long)(uint64_t)imm);
            anvil_strbuf_appendf(&emit->code, "\tmovq %%r11, %%%s\n", dst);
        }
        return;
    }

    int size = x64_size_bytes(def_info->size_bits);
    if (size < 8) {
        anvil_strbuf_appendf(&emit->code, "\tmovl $%lld, %%%s\n", (long long)(int64_t)(int32_t)imm, x64_gpr_name(dst_phys, 4));
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmovabsq $%lld, %%%s\n", (long long)imm, x64_gpr_name(dst_phys, 8));
    }
}

static void x64_emit_gpr_binary(x64_mir_emit_t *emit, const anvil_mir_instr_info_t *info, int a_phys, int b_phys, int size)
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

    switch (info->op) {
    case ANVIL_MIR_OP_ADD:
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
        anvil_strbuf_appendf(&emit->code, "\tadd%s %%%s, %%%s\n", suf, b, rax);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
        break;
    case ANVIL_MIR_OP_SUB:
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
        anvil_strbuf_appendf(&emit->code, "\tsub%s %%%s, %%%s\n", suf, b, rax);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
        break;
    case ANVIL_MIR_OP_MUL:
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
        anvil_strbuf_appendf(&emit->code, "\timul%s %%%s, %%%s\n", suf, b, rax);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
        break;
    case ANVIL_MIR_OP_AND:
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
        anvil_strbuf_appendf(&emit->code, "\tand%s %%%s, %%%s\n", suf, b, rax);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
        break;
    case ANVIL_MIR_OP_OR:
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
        anvil_strbuf_appendf(&emit->code, "\tor%s %%%s, %%%s\n", suf, b, rax);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
        break;
    case ANVIL_MIR_OP_XOR:
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
        anvil_strbuf_appendf(&emit->code, "\txor%s %%%s, %%%s\n", suf, b, rax);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
        break;
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR: {
        const char *sh = info->op == ANVIL_MIR_OP_SHL ? "shl" : info->op == ANVIL_MIR_OP_SHR ? "shr" : "sar";
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, b, rcx);
        anvil_strbuf_appendf(&emit->code, "\t%s%s %%cl, %%%s\n", sh, suf, rax);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, rax, dst);
        break;
    }
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD: {
        bool is_signed = info->op == ANVIL_MIR_OP_SDIV || info->op == ANVIL_MIR_OP_SMOD;
        bool is_mod = info->op == ANVIL_MIR_OP_SMOD || info->op == ANVIL_MIR_OP_UMOD;
        const char *r11 = x64_gpr_name(X64_R11, size);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, rax);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, b, r11);
        if (size == 1) {
            if (is_signed) {
                anvil_strbuf_append(&emit->code, "\tcbtw\n");
                anvil_strbuf_appendf(&emit->code, "\tidivb %%%s\n", r11);
            } else {
                anvil_strbuf_append(&emit->code, "\tmovzbw %al, %ax\n");
                anvil_strbuf_appendf(&emit->code, "\tdivb %%%s\n", r11);
            }
            if (is_mod) {
                anvil_strbuf_append(&emit->code, "\tshrw $8, %ax\n");
            }
            anvil_strbuf_appendf(&emit->code, "\tmovb %%al, %%%s\n", dst);
        } else {
            if (is_signed) {
                const char *ext = size == 2 ? "\tcwtd\n" : size == 4 ? "\tcltd\n" : "\tcqto\n";
                anvil_strbuf_append(&emit->code, ext);
                anvil_strbuf_appendf(&emit->code, "\tidiv%s %%%s\n", suf, r11);
            } else {
                anvil_strbuf_appendf(&emit->code, "\txor%s %%%s, %%%s\n", suf, rdx, rdx);
                anvil_strbuf_appendf(&emit->code, "\tdiv%s %%%s\n", suf, r11);
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
        const char *dst = x64_reg_name(emit, info->def);
        const char *a = x64_reg_name(emit, lhs);
        const char *b = x64_reg_name(emit, rhs);
        if (emit->failed)
            return;
        const char *op = NULL;
        const char *sfx = size <= 4 ? "ss" : "sd";
        if (size == 16)
            sfx = info->imm == 32 ? "ps" : "pd";

        switch (info->op) {
        case ANVIL_MIR_OP_VECTOR_FADD:
            op = "add";
            break;
        case ANVIL_MIR_OP_VECTOR_FSUB:
            op = "sub";
            break;
        case ANVIL_MIR_OP_VECTOR_FMUL:
            op = "mul";
            break;
        case ANVIL_MIR_OP_VECTOR_FDIV:
            op = "div";
            break;
        case ANVIL_MIR_OP_ADD:
            op = "add";
            break;
        case ANVIL_MIR_OP_SUB:
            op = "sub";
            break;
        case ANVIL_MIR_OP_MUL:
            op = "mul";
            break;
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_FDIV:
            op = "div";
            break;
        default:
            break;
        }
        if (!op) {
            emit->failed = true;
            return;
        }
        /* Linear scan may reuse the dying RHS for the result. SSE arithmetic
         * is destructive: copying LHS there first would lose RHS. XMM5 on
         * Win64 / XMM15 on SysV are volatile, non-allocatable emitter temps. */
        bool overlaps_rhs = x64_phys_of(emit, info->def) == x64_phys_of(emit, rhs) && x64_phys_of(emit, lhs) != x64_phys_of(emit, rhs);
        const char *work = overlaps_rhs ? (emit->desc->is_win64 ? "xmm5" : "xmm15") : dst;
        if (strcmp(a, work) != 0)
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_mov_op(size), a, work);
        anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n", op, sfx, b, work);
        if (overlaps_rhs)
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_mov_op(size), work, dst);
        return;
    }

    int size = x64_size_bytes(def_info->size_bits);
    int a_phys = x64_phys_of(emit, lhs);
    int b_phys = x64_phys_of(emit, rhs);
    if (emit->failed)
        return;
    x64_emit_gpr_binary(emit, info, a_phys, b_phys, size);
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
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", size <= 4 ? "ucomiss" : "ucomisd", b, a);
        if (info->op == ANVIL_MIR_OP_FCMP) {
            anvil_fcmp_pred_t pred = (anvil_fcmp_pred_t)info->imm;
            static const unsigned masks[] = {0, 4, 2, 6, 1, 5, 3, 7, 12, 10, 14, 9, 13, 11, 8, 15};
            unsigned mask = masks[pred];
            anvil_strbuf_appendf(&emit->code, "\tmovb $0, %%%s\n", dst8);
            if (mask == 15) {
                anvil_strbuf_appendf(&emit->code, "\tmovb $1, %%%s\n", dst8);
            } else if (mask != 0) {
                const char *name = anvil_mir_func_name(emit->mir);
                if (mask & 8)
                    anvil_strbuf_appendf(&emit->code, "\tjp .L%s_fcmp_true_%zu\n", name, instr_index);
                else
                    anvil_strbuf_appendf(&emit->code, "\tjp .L%s_fcmp_done_%zu\n", name, instr_index);
                if (mask & 1)
                    anvil_strbuf_appendf(&emit->code, "\tjb .L%s_fcmp_true_%zu\n", name, instr_index);
                if (mask & 2)
                    anvil_strbuf_appendf(&emit->code, "\tja .L%s_fcmp_true_%zu\n", name, instr_index);
                if (mask & 4)
                    anvil_strbuf_appendf(&emit->code, "\tje .L%s_fcmp_true_%zu\n", name, instr_index);
                anvil_strbuf_appendf(&emit->code,
                                     "\tjmp .L%s_fcmp_done_%zu\n.L%s_fcmp_true_%zu:\n"
                                     "\tmovb $1, %%%s\n.L%s_fcmp_done_%zu:\n",
                                     name, instr_index, name, instr_index, dst8, name, instr_index);
            }
        } else {
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n", x64_setcc(info->op), dst8);
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
    anvil_strbuf_appendf(&emit->code, "\tcmp%s %%%s, %%%s\n", suf, b, a);
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n", x64_setcc(info->op), dst8);
    anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", dst8, dst32);
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
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_mov_op(size), s, dst);
            anvil_strbuf_appendf(&emit->code, "\t%s .L%s_fneg_mask%s(%%rip), %%%s\n", size <= 4 ? "xorps" : "xorpd", anvil_mir_func_name(emit->mir), size <= 4 ? "32" : "64", dst);
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
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_mov_op(size), s, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s .L%s_fabs_mask%s(%%rip), %%%s\n", size <= 4 ? "andps" : "andpd", anvil_mir_func_name(emit->mir), size <= 4 ? "32" : "64", dst);
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
        if (src_size <= 4) {
            const char *s = x64_gpr_name(x64_phys_of(emit, src), 4);
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%r11d\n", s);
            anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%sq %%r11, %%%s\n", dst_size <= 4 ? "s" : "d", dst);
        } else {
            const char *s = x64_gpr_name(x64_phys_of(emit, src), 8);
            anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%sq %%%s, %%%s\n", dst_size <= 4 ? "s" : "d", s, dst);
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
        const char *dst = x64_gpr_name(x64_phys_of(emit, info->def), dst_size <= 4 ? 4 : 8);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\tcvtts%s2si %%%s, %%%s\n", src_size <= 4 ? "s" : "d", s, dst);
        break;
    }
    case ANVIL_MIR_OP_FPEXT: {
        const char *dst = x64_reg_name(emit, info->def);
        const char *s = x64_reg_name(emit, src);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\tcvtss2sd %%%s, %%%s\n", s, dst);
        break;
    }
    case ANVIL_MIR_OP_FPTRUNC: {
        const char *dst = x64_reg_name(emit, info->def);
        const char *s = x64_reg_name(emit, src);
        if (emit->failed)
            return;
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

static void x64_emit_load(x64_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const anvil_mir_vreg_info_t *def_info = x64_vreg_info_checked(emit, info->def);
    int def_phys = x64_phys_of(emit, info->def);
    const char *base = x64_gpr_name(x64_phys_of(emit, ptr), 8);
    if (!def_info || emit->failed)
        return;

    int size = x64_size_bytes(def_info->size_bits);
    int64_t offset = info->has_imm ? info->imm : 0;
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

    if (offset == 0) {
        anvil_strbuf_appendf(&emit->code, "\t%s (%%%s), %%%s\n", op, base, dst);
    } else {
        anvil_strbuf_appendf(&emit->code, "\t%s %lld(%%%s), %%%s\n", op, (long long)offset, base, dst);
    }
}

static void x64_emit_store(x64_mir_emit_t *emit, size_t instr_index)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (value == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const anvil_mir_vreg_info_t *value_info = x64_vreg_info_checked(emit, value);
    const char *base = x64_gpr_name(x64_phys_of(emit, ptr), 8);
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

    int64_t offset = 0;
    anvil_mir_instr_info_t full;
    if (anvil_mir_get_instr_info(emit->mir, instr_index, &full) && full.has_imm) {
        offset = full.imm;
    }
    if (offset == 0) {
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, (%%%s)\n", op, src, base);
    } else {
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %lld(%%%s)\n", op, src, (long long)offset, base);
    }
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
        anvil_strbuf_appendf(&emit->code, "\tcall %s%s\n", x64_symbol_ref_prefix(emit, info->symbol), info->symbol);
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
            int size = x64_size_bytes(ret_info->size_bits);
            if (ret_phys != emit->desc->fp_ret_reg) {
                anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x64_fp_mov_op(size), x64_xmm_names[ret_phys], x64_xmm_names[emit->desc->fp_ret_reg]);
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
static void x64_atomic_inputs(x64_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info)
{
    int destinations[] = {X64_RDX, X64_RCX, X64_RAX};
    if (info->atomic_op == ANVIL_OP_ATOMIC_CMPXCHG) {
        destinations[1] = X64_RAX;
        destinations[2] = X64_RCX;
    }

    int sources[3];
    bool pending[3] = {false};
    size_t count = info->num_uses;
    size_t remaining = 0;
    for (size_t operand = 0; operand < count; operand++) {
        sources[operand] = x64_phys_of(emit, anvil_mir_get_instr_use(emit->mir, index, operand));
        pending[operand] = sources[operand] != destinations[operand];
        remaining += pending[operand];
    }

    while (remaining && !emit->failed) {
        bool moved = false;
        for (size_t operand = 0; operand < count; operand++) {
            if (!pending[operand])
                continue;

            bool needed = false;
            for (size_t other = 0; other < count; other++)
                needed |= pending[other] && sources[other] == destinations[operand];

            if (needed)
                continue;

            anvil_strbuf_appendf(&emit->code, "\tmovq %%%s, %%%s\n", x64_gpr_name(sources[operand], 8), x64_gpr_name(destinations[operand], 8));
            pending[operand] = false;
            remaining--;
            moved = true;
        }

        if (moved)
            continue;

        size_t cycle = 0;
        while (!pending[cycle])
            cycle++;

        int left = destinations[cycle];
        int right = sources[cycle];
        anvil_strbuf_appendf(&emit->code, "\txchgq %%%s, %%%s\n", x64_gpr_name(left, 8), x64_gpr_name(right, 8));
        for (size_t operand = 0; operand < count; operand++) {
            if (!pending[operand])
                continue;

            if (sources[operand] == left)
                sources[operand] = right;
            else if (sources[operand] == right)
                sources[operand] = left;

            if (sources[operand] == destinations[operand]) {
                pending[operand] = false;
                remaining--;
            }
        }
    }
}

static void x64_emit_atomic(x64_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info)
{
    if (info->atomic_op == ANVIL_OP_ATOMIC_FENCE) {
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

    switch (info->atomic_op) {
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
            anvil_strbuf_appendf(&emit->code, "\tmov%s (%%rdx), %%%s\n.L%s_atomic_%zu:\n", suffix, accumulator, function, index);
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n\t%s%s %%%s, %%%s\n", suffix, accumulator, temporary, operation, suffix, operand, temporary);
            anvil_strbuf_appendf(&emit->code, "\tlock cmpxchg%s %%%s, (%%rdx)\n\tjne .L%s_atomic_%zu\n", suffix, temporary, function, index);
        }
        break;
    default:
        emit->failed = true;
        return;
    }

    const char *destination = x64_gpr_name(x64_phys_of(emit, info->def), size);
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suffix, accumulator, destination);
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
    case ANVIL_MIR_OP_VECTOR_FADD:
    case ANVIL_MIR_OP_VECTOR_FSUB:
    case ANVIL_MIR_OP_VECTOR_FMUL:
    case ANVIL_MIR_OP_VECTOR_FDIV:
        x64_emit_binary(emit, instr_index, info);
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
        x64_emit_store(emit, instr_index);
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
        anvil_strbuf_append(&emit->code, "\tjmp ");
        if (!x64_emit_branch_target(emit, info->true_block)) {
            emit->failed = true;
            break;
        }
        anvil_strbuf_append(&emit->code, "\n");
        break;
    case ANVIL_MIR_OP_BR_COND: {
        anvil_mir_vreg_t cond = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        if (cond == ANVIL_MIR_NO_VREG) {
            emit->failed = true;
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
        anvil_strbuf_append(&emit->code, "\tjne ");
        if (!x64_emit_branch_target(emit, info->true_block)) {
            emit->failed = true;
            break;
        }
        anvil_strbuf_append(&emit->code, "\n\tjmp ");
        if (!x64_emit_branch_target(emit, info->false_block)) {
            emit->failed = true;
            break;
        }
        anvil_strbuf_append(&emit->code, "\n");
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

    if (emit->desc->is_darwin) {
        anvil_strbuf_append(&emit->code, "\t.section __TEXT,__cstring,cstring_literals\n");
    } else {
        anvil_strbuf_append(&emit->code, emit->desc->is_win64 ? "\t.section .rdata,\"dr\"\n" : "\t.section .rodata\n");
    }

    for (size_t i = 0; i < count; i++) {
        anvil_mir_string_literal_info_t info;
        if (!anvil_mir_get_string_literal_info(emit->mir, i, &info) || !info.label) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "%s:\n", info.label);
        x64_emit_escaped_string(&emit->code, info.value);
    }

    if (emit->emitted_fneg_mask) {
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_appendf(&emit->code, ".L%s_fneg_mask32:\n", anvil_mir_func_name(emit->mir));
        anvil_strbuf_append(&emit->code, "\t.long 0x80000000\n");
        anvil_strbuf_append(&emit->code, "\t.long 0\n\t.long 0\n\t.long 0\n");
        anvil_strbuf_appendf(&emit->code, ".L%s_fneg_mask64:\n", anvil_mir_func_name(emit->mir));
        anvil_strbuf_append(&emit->code, "\t.quad 0x8000000000000000\n");
        anvil_strbuf_append(&emit->code, "\t.quad 0\n");
    }
    if (emit->emitted_fabs_mask) {
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_appendf(&emit->code, ".L%s_fabs_mask32:\n", anvil_mir_func_name(emit->mir));
        anvil_strbuf_append(&emit->code, "\t.long 0x7fffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_appendf(&emit->code, ".L%s_fabs_mask64:\n", anvil_mir_func_name(emit->mir));
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

    if (!x64_prepare_frame(&emit)) {
        anvil_strbuf_destroy(&emit.code);
        free(emit.spill_offsets);
        free(emit.frame_slot_offsets);
        return false;
    }

    x64_emit_prologue(&emit);

    size_t num_blocks = anvil_mir_num_blocks(mir);
    size_t num_instrs = anvil_mir_num_instrs(mir);
    for (size_t b = 0; b < num_blocks && !emit.failed && !emit.code.failed; b++) {
        if (!x64_emit_label(&emit, (anvil_mir_block_t)b)) {
            emit.failed = true;
            break;
        }

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
