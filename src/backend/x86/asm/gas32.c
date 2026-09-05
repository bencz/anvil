#include "../x86_32_internal.h"
#include "anvil/anvil_analysis.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const anvil_mir_func_t *mir;
    const anvil_func_t *source_func;
    const anvil_x86_cc_desc_t *desc;
    const anvil_x86_plat_desc_t *plat;
    anvil_syntax_t syntax;
    anvil_strbuf_t code;
    int *spill_offsets;
    size_t num_spill_offsets;
    int *frame_slot_offsets;
    size_t num_frame_slot_offsets;
    int gpr_save_offsets[8];
    int scratch_a_off;
    int scratch_b_off;
    int outgoing_size;
    int frame_size;
    int ret_pop_bytes;
    bool fp_returns;
    bool emitted_fneg_mask;
    bool emitted_fabs_mask;
    bool failed;
} x86_mir_emit_t;

static int x86_align_int(int value, int align)
{
    return (value + align - 1) & ~(align - 1);
}

static int x86_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0)
        return 4;
    int size = (int)((size_bits + 7) / 8);
    if (size <= 0)
        return 4;
    if (size > 8)
        return 8;
    return size;
}

static int x86_slot_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0)
        return 4;
    int size = (int)((size_bits + 7) / 8);
    return size > 0 ? size : 4;
}

static const anvil_mir_vreg_info_t *x86_vreg_info_checked(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->mir, vreg);
    if (!info)
        emit->failed = true;
    return info;
}

static int x86_phys_of(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(emit->mir, vreg);
    if (!assignment || assignment->spilled || assignment->phys_reg < 0) {
        emit->failed = true;
        return -1;
    }
    return assignment->phys_reg;
}

static const char *x86_gpr_name(int phys_reg, int size)
{
    if (phys_reg < 0 || phys_reg >= 8)
        return "?";
    switch (size) {
    case 1:
        return x86_reg8_names[phys_reg];
    case 2:
        return x86_reg16_names[phys_reg];
    default:
        return x86_reg32_names[phys_reg];
    }
}

static const char *x86_reg_name(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = x86_vreg_info_checked(emit, vreg);
    int phys = x86_phys_of(emit, vreg);
    if (!info || emit->failed)
        return "?";

    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        if (phys < 0 || phys >= 8) {
            emit->failed = true;
            return "?";
        }
        return x86_xmm_names[phys];
    }

    int size = x86_size_bytes(info->size_bits);
    return x86_gpr_name(phys, size);
}

static bool x86_emit_label(x86_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info))
        return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s:\n", anvil_mir_func_name(emit->mir), name);
    return true;
}

static bool x86_emit_branch_target(x86_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info))
        return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s", anvil_mir_func_name(emit->mir), name);
    return true;
}

static const char *x86_symbol_prefix(const x86_mir_emit_t *emit)
{
    return emit && emit->plat ? emit->plat->sym_prefix : "";
}

static bool x86_symbol_is_local(const char *symbol)
{
    return symbol && symbol[0] == '.';
}

static const char *x86_symbol_ref_prefix(const x86_mir_emit_t *emit, const char *symbol)
{
    return x86_symbol_is_local(symbol) ? "" : x86_symbol_prefix(emit);
}

static const char *x86_setcc(anvil_mir_opcode_t op)
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

static const char *x86_size_suffix(int size)
{
    switch (size) {
    case 1:
        return "b";
    case 2:
        return "w";
    default:
        return "l";
    }
}

static bool x86_scan_outgoing_stack_args(x86_mir_emit_t *emit)
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
        if (info.imm > INT32_MAX - 4)
            return false;

        int end = (int)info.imm + 4;
        if (end > outgoing_size)
            outgoing_size = end;
    }

    emit->outgoing_size = x86_align_int(outgoing_size, 16);
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
    emit->fp_returns = x86_func_returns_fp(emit->mir);

    int offset = 0;
    static const int callee_saved[] = {X86_EBX, X86_ESI, X86_EDI};
    for (size_t c = 0; c < sizeof(callee_saved) / sizeof(callee_saved[0]); c++) {
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
            offset += 4;
            emit->gpr_save_offsets[reg] = offset;
        }
    }

    offset += 4;
    emit->scratch_a_off = offset;
    offset += 4;
    emit->scratch_b_off = offset;

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
        int align = slot.align_bytes ? slot.align_bytes : 4;
        if (align > 16)
            align = 16;
        offset = x86_align_int(offset, align);
        offset += x86_slot_size_bytes(slot.size_bits);
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
        offset += x86_align_int(x86_slot_size_bytes(slot.size_bits), 4);
        emit->spill_offsets[i] = offset;
    }

    offset += emit->outgoing_size;
    emit->frame_size = x86_align_int(offset, 16);
    return true;
}

static void x86_emit_func_header(x86_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);
    const char *prefix = x86_symbol_prefix(emit);
    if (emit->plat->is_macho) {
        anvil_strbuf_append(&emit->code, "\t.section __TEXT,__text,regular,pure_instructions\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s%s\n", prefix, name);
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    } else if (emit->plat->is_coff) {
        char decorated[256];
        int argument_bytes = 0;
        if (emit->source_func && emit->source_func->type && emit->source_func->type->kind == ANVIL_TYPE_FUNC) {
            anvil_type_t *type = emit->source_func->type;
            for (size_t i = 0; i < type->data.func.num_params; i++) {
                argument_bytes += x86_stack_arg_slot_size(type->data.func.params[i]);
            }
        }
        if (emit->desc->decor == X86_DECOR_STDCALL)
            snprintf(decorated, sizeof(decorated), "_%s@%d", name, argument_bytes);
        else if (emit->desc->decor == X86_DECOR_FASTCALL)
            snprintf(decorated, sizeof(decorated), "@%s@%d", name, argument_bytes);
        else
            snprintf(decorated, sizeof(decorated), "%s%s", prefix, name);
        anvil_strbuf_append(&emit->code, "\t.text\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s\n", decorated);
        anvil_strbuf_appendf(&emit->code, "\t.def %s; .scl 2; .type 32; .endef\n", decorated);
        anvil_strbuf_appendf(&emit->code, "%s:\n", decorated);
    } else {
        anvil_strbuf_append(&emit->code, "\t.text\n");
        anvil_strbuf_appendf(&emit->code, "\t.globl %s%s\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "\t.type %s%s, @function\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    }
}

static void x86_emit_prologue(x86_mir_emit_t *emit)
{
    anvil_strbuf_append(&emit->code, "\tpushl %ebp\n");
    anvil_strbuf_append(&emit->code, "\tmovl %esp, %ebp\n");
    if (emit->frame_size > 0) {
        anvil_strbuf_appendf(&emit->code, "\tsubl $%d, %%esp\n", emit->frame_size);
    }

    for (int reg = 0; reg < 8; reg++) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, -%d(%%ebp)\n", x86_reg32_names[reg], emit->gpr_save_offsets[reg]);
        }
    }
}

static void x86_emit_epilogue(x86_mir_emit_t *emit)
{
    for (int reg = 7; reg >= 0; reg--) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovl -%d(%%ebp), %%%s\n", emit->gpr_save_offsets[reg], x86_reg32_names[reg]);
        }
    }
    anvil_strbuf_append(&emit->code, "\tmovl %ebp, %esp\n");
    anvil_strbuf_append(&emit->code, "\tpopl %ebp\n");
    if (emit->desc->callee_cleans_stack && emit->ret_pop_bytes > 0) {
        anvil_strbuf_appendf(&emit->code, "\tret $%d\n", emit->ret_pop_bytes);
    } else {
        anvil_strbuf_append(&emit->code, "\tret\n");
    }
}

static const char *x86_fp_mov_op(int size)
{
    return size <= 4 ? "movss" : "movsd";
}

static void x86_emit_copy(x86_mir_emit_t *emit, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = x86_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = x86_vreg_info_checked(emit, src);
    int dst_phys = x86_phys_of(emit, dst);
    int src_phys = x86_phys_of(emit, src);
    if (!dst_info || !src_info || emit->failed)
        return;

    if (dst_info->reg_class == ANVIL_MIR_REG_FPR && src_info->reg_class == ANVIL_MIR_REG_FPR) {
        if (dst_phys == src_phys)
            return;
        int size = x86_size_bytes(dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x86_fp_mov_op(size), x86_xmm_names[src_phys], x86_xmm_names[dst_phys]);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_GPR && src_info->reg_class == ANVIL_MIR_REG_GPR) {
        if (dst_phys == src_phys)
            return;
        int size = x86_size_bytes(dst_info->size_bits);
        if (size < 4)
            size = 4;
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", x86_size_suffix(size), x86_gpr_name(src_phys, size), x86_gpr_name(dst_phys, size));
    } else if (dst_info->reg_class == ANVIL_MIR_REG_FPR && src_info->reg_class == ANVIL_MIR_REG_GPR) {
        anvil_strbuf_appendf(&emit->code, "\tmovd %%%s, %%%s\n", x86_gpr_name(src_phys, 4), x86_xmm_names[dst_phys]);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_GPR && src_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tmovd %%%s, %%%s\n", x86_xmm_names[src_phys], x86_gpr_name(dst_phys, 4));
    } else {
        emit->failed = true;
    }
}

static void x86_emit_mov(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info)
        return;
    int dst_phys = x86_phys_of(emit, info->def);
    if (emit->failed)
        return;

    int64_t imm = info->has_imm ? info->imm : 0;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x86_size_bytes(def_info->size_bits);
        const char *dst = x86_xmm_names[dst_phys];
        if (size <= 4) {
            anvil_strbuf_appendf(&emit->code, "\tmovl $%u, -%d(%%ebp)\n", (uint32_t)imm, emit->scratch_a_off);
            anvil_strbuf_appendf(&emit->code, "\tmovss -%d(%%ebp), %%%s\n", emit->scratch_a_off, dst);
        } else {
            uint64_t bits = (uint64_t)imm;
            anvil_strbuf_appendf(&emit->code, "\tmovl $%u, -%d(%%ebp)\n", (uint32_t)(bits & 0xffffffffu), emit->scratch_b_off);
            anvil_strbuf_appendf(&emit->code, "\tmovl $%u, -%d(%%ebp)\n", (uint32_t)(bits >> 32), emit->scratch_b_off - 4);
            anvil_strbuf_appendf(&emit->code, "\tmovsd -%d(%%ebp), %%%s\n", emit->scratch_b_off, dst);
        }
        return;
    }

    int size = x86_size_bytes(def_info->size_bits);
    anvil_strbuf_appendf(&emit->code, "\tmovl $%lld, %%%s\n", (long long)(int64_t)(int32_t)imm, x86_gpr_name(dst_phys, size < 4 ? 4 : size));
}

static void x86_emit_gpr_simple_binary(x86_mir_emit_t *emit, const char *mnemonic, const char *suf, int dst_phys, int a_phys, int b_phys, int size, bool commutative)
{
    const char *dst = x86_gpr_name(dst_phys, size);
    const char *a = x86_gpr_name(a_phys, size);
    const char *b = x86_gpr_name(b_phys, size);

    if (dst_phys == b_phys && !commutative) {
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, -%d(%%ebp)\n", suf, b, emit->scratch_b_off);
        if (dst_phys != a_phys) {
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, dst);
        }
        anvil_strbuf_appendf(&emit->code, "\t%s%s -%d(%%ebp), %%%s\n", mnemonic, suf, emit->scratch_b_off, dst);
        return;
    }

    if (dst_phys == b_phys && commutative) {
        anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n", mnemonic, suf, a, dst);
        return;
    }

    if (dst_phys != a_phys) {
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, a, dst);
    }
    anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n", mnemonic, suf, b, dst);
}

static void x86_emit_gpr_binary(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info, int dst_phys, int a_phys, int b_phys, int size)
{
    int ssize = size < 4 ? 4 : size;
    const char *ssuf = x86_size_suffix(ssize);
    if (emit->failed)
        return;

    switch (info->op) {
    case ANVIL_MIR_OP_ADD:
        x86_emit_gpr_simple_binary(emit, "add", ssuf, dst_phys, a_phys, b_phys, ssize, true);
        break;
    case ANVIL_MIR_OP_SUB:
        x86_emit_gpr_simple_binary(emit, "sub", ssuf, dst_phys, a_phys, b_phys, ssize, false);
        break;
    case ANVIL_MIR_OP_AND:
        x86_emit_gpr_simple_binary(emit, "and", ssuf, dst_phys, a_phys, b_phys, ssize, true);
        break;
    case ANVIL_MIR_OP_OR:
        x86_emit_gpr_simple_binary(emit, "or", ssuf, dst_phys, a_phys, b_phys, ssize, true);
        break;
    case ANVIL_MIR_OP_XOR:
        x86_emit_gpr_simple_binary(emit, "xor", ssuf, dst_phys, a_phys, b_phys, ssize, true);
        break;
    case ANVIL_MIR_OP_MUL: {
        const char *b = x86_gpr_name(b_phys, size < 4 ? 4 : size);
        int wsize = size < 4 ? 4 : size;
        const char *wsuf = x86_size_suffix(wsize);
        const char *wdst = x86_gpr_name(dst_phys, wsize);
        const char *wa = x86_gpr_name(a_phys, wsize);
        if (dst_phys == b_phys) {
            anvil_strbuf_appendf(&emit->code, "\timul%s %%%s, %%%s\n", wsuf, wa, wdst);
        } else {
            if (dst_phys != a_phys) {
                anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", wsuf, wa, wdst);
            }
            anvil_strbuf_appendf(&emit->code, "\timul%s %%%s, %%%s\n", wsuf, b, wdst);
        }
        break;
    }
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR: {
        const char *sh = info->op == ANVIL_MIR_OP_SHL ? "shl" : info->op == ANVIL_MIR_OP_SHR ? "shr" : "sar";
        const char *suf = x86_size_suffix(size);
        const char *a = x86_gpr_name(a_phys, size);
        const char *dst = x86_gpr_name(dst_phys, size);
        const char *b = x86_gpr_name(b_phys, 4);
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, -%d(%%ebp)\n", b, emit->scratch_b_off);
        anvil_strbuf_appendf(&emit->code, "\tmovl -%d(%%ebp), %%ecx\n", emit->scratch_b_off);
        if (size == 1) {
            const char *scratch8 = x86_reg8_names[X86_EAX];
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%eax\n", x86_gpr_name(a_phys, 4));
            anvil_strbuf_appendf(&emit->code, "\t%sb %%cl, %%%s\n", sh, scratch8);
            anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", scratch8, x86_gpr_name(dst_phys, 4));
        } else {
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, -%d(%%ebp)\n", suf, a, emit->scratch_a_off);
            anvil_strbuf_appendf(&emit->code, "\tmov%s -%d(%%ebp), %%%s\n", suf, emit->scratch_a_off, dst);
            anvil_strbuf_appendf(&emit->code, "\t%s%s %%cl, %%%s\n", sh, suf, dst);
        }
        break;
    }
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD: {
        bool is_signed = info->op == ANVIL_MIR_OP_SDIV || info->op == ANVIL_MIR_OP_SMOD;
        bool is_mod = info->op == ANVIL_MIR_OP_SMOD || info->op == ANVIL_MIR_OP_UMOD;
        int wsize = size < 4 ? 4 : size;
        const char *wsuf = x86_size_suffix(wsize);
        const char *wa = x86_gpr_name(a_phys, wsize);
        const char *wb = x86_gpr_name(b_phys, wsize);
        const char *wdst = x86_gpr_name(dst_phys, wsize);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, -%d(%%ebp)\n", wsuf, wb, emit->scratch_b_off);
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, -%d(%%ebp)\n", wsuf, wa, emit->scratch_a_off);
        anvil_strbuf_appendf(&emit->code, "\tmov%s -%d(%%ebp), %%eax\n", wsuf, emit->scratch_a_off);
        if (is_signed) {
            anvil_strbuf_append(&emit->code, "\tcltd\n");
            anvil_strbuf_appendf(&emit->code, "\tidiv%s -%d(%%ebp)\n", wsuf, emit->scratch_b_off);
        } else {
            anvil_strbuf_append(&emit->code, "\txorl %edx, %edx\n");
            anvil_strbuf_appendf(&emit->code, "\tdiv%s -%d(%%ebp)\n", wsuf, emit->scratch_b_off);
        }
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", wsuf, is_mod ? "edx" : "eax", wdst);
        break;
    }
    default:
        emit->failed = true;
        break;
    }
}

static void x86_emit_binary(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info)
        return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x86_size_bytes(def_info->size_bits);
        const char *dst = x86_reg_name(emit, info->def);
        const char *a = x86_reg_name(emit, lhs);
        const char *b = x86_reg_name(emit, rhs);
        if (emit->failed)
            return;
        const char *op = NULL;
        const char *sfx = size <= 4 ? "ss" : "sd";
        switch (info->op) {
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
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x86_fp_mov_op(size), a, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n", op, sfx, b, dst);
        return;
    }

    int size = x86_size_bytes(def_info->size_bits);
    int dst_phys = x86_phys_of(emit, info->def);
    int a_phys = x86_phys_of(emit, lhs);
    int b_phys = x86_phys_of(emit, rhs);
    if (emit->failed)
        return;
    x86_emit_gpr_binary(emit, info, dst_phys, a_phys, b_phys, size);
}

static void x86_emit_cmp(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *lhs_info = x86_vreg_info_checked(emit, lhs);
    int dst_phys = x86_phys_of(emit, info->def);
    if (!lhs_info || emit->failed)
        return;

    const char *dst32 = x86_gpr_name(dst_phys, 4);
    const char *dst8 = x86_byte_reg_name(dst_phys);

    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x86_size_bytes(lhs_info->size_bits);
        const char *a = x86_reg_name(emit, lhs);
        const char *b = x86_reg_name(emit, rhs);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", size <= 4 ? "ucomiss" : "ucomisd", b, a);
        if (info->op == ANVIL_MIR_OP_FCMP) {
            anvil_fcmp_pred_t pred = (anvil_fcmp_pred_t)info->imm;
            static const unsigned masks[] = {0, 4, 2, 6, 1, 5, 3, 7, 12, 10, 14, 9, 13, 11, 8, 15};
            unsigned mask = masks[pred];
            const char *name = anvil_mir_func_name(emit->mir);
            if (dst8)
                anvil_strbuf_appendf(&emit->code, "\tmovb $0, %%%s\n", dst8);
            else
                anvil_strbuf_appendf(&emit->code, "\tmovb $0, -%d(%%ebp)\n", emit->scratch_a_off);
            if (mask == 15) {
                if (dst8)
                    anvil_strbuf_appendf(&emit->code, "\tmovb $1, %%%s\n", dst8);
                else
                    anvil_strbuf_appendf(&emit->code, "\tmovb $1, -%d(%%ebp)\n", emit->scratch_a_off);
            } else if (mask != 0) {
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
                anvil_strbuf_appendf(&emit->code, "\tjmp .L%s_fcmp_done_%zu\n.L%s_fcmp_true_%zu:\n", name, instr_index, name, instr_index);
                if (dst8)
                    anvil_strbuf_appendf(&emit->code, "\tmovb $1, %%%s\n", dst8);
                else
                    anvil_strbuf_appendf(&emit->code, "\tmovb $1, -%d(%%ebp)\n", emit->scratch_a_off);
                anvil_strbuf_appendf(&emit->code, ".L%s_fcmp_done_%zu:\n", name, instr_index);
            }
        } else {
            if (dst8)
                anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n", x86_setcc(info->op), dst8);
            else
                anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%ebp)\n", x86_setcc(info->op), emit->scratch_a_off);
        }
        if (dst8)
            anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", dst8, dst32);
        else
            anvil_strbuf_appendf(&emit->code, "\tmovzbl -%d(%%ebp), %%%s\n", emit->scratch_a_off, dst32);
        return;
    }

    int size = x86_size_bytes(lhs_info->size_bits);
    int a_phys = x86_phys_of(emit, lhs);
    int b_phys = x86_phys_of(emit, rhs);
    if (emit->failed)
        return;
    if (size == 1) {
        const char *a8 = x86_byte_reg_name(a_phys);
        const char *b8 = x86_byte_reg_name(b_phys);
        if (a8 && b8) {
            anvil_strbuf_appendf(&emit->code, "\tcmpb %%%s, %%%s\n", b8, a8);
        } else if (a8) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, -%d(%%ebp)\n", x86_gpr_name(b_phys, 4), emit->scratch_b_off);
            anvil_strbuf_appendf(&emit->code, "\tcmpb -%d(%%ebp), %%%s\n", emit->scratch_b_off, a8);
        } else {
            anvil_strbuf_append(&emit->code, "\tpushl %eax\n");
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, -%d(%%ebp)\n", x86_gpr_name(b_phys, 4), emit->scratch_b_off);
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%eax\n", x86_gpr_name(a_phys, 4));
            anvil_strbuf_appendf(&emit->code, "\tcmpb -%d(%%ebp), %%al\n", emit->scratch_b_off);
            anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%ebp)\n", x86_setcc(info->op), emit->scratch_b_off);
            anvil_strbuf_append(&emit->code, "\tpopl %eax\n");
            anvil_strbuf_appendf(&emit->code, "\tmovzbl -%d(%%ebp), %%%s\n", emit->scratch_b_off, dst32);
            return;
        }
    } else {
        const char *suf = x86_size_suffix(size);
        const char *a = x86_gpr_name(a_phys, size);
        const char *b = x86_gpr_name(b_phys, size);
        anvil_strbuf_appendf(&emit->code, "\tcmp%s %%%s, %%%s\n", suf, b, a);
    }
    if (dst8) {
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n", x86_setcc(info->op), dst8);
        anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", dst8, dst32);
    } else {
        anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%ebp)\n", x86_setcc(info->op), emit->scratch_a_off);
        anvil_strbuf_appendf(&emit->code, "\tmovzbl -%d(%%ebp), %%%s\n", emit->scratch_a_off, dst32);
    }
}

static void x86_emit_unary(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info || emit->failed)
        return;

    if (info->op == ANVIL_MIR_OP_NEG) {
        if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
            int size = x86_size_bytes(def_info->size_bits);
            const char *dst = x86_reg_name(emit, info->def);
            const char *s = x86_reg_name(emit, src);
            if (emit->failed)
                return;
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x86_fp_mov_op(size), s, dst);
            anvil_strbuf_appendf(&emit->code, "\t%s .Lx86_fneg_mask%s, %%%s\n", size <= 4 ? "xorps" : "xorpd", size <= 4 ? "32" : "64", dst);
            emit->emitted_fneg_mask = true;
        } else {
            int size = x86_size_bytes(def_info->size_bits);
            if (size < 4)
                size = 4;
            const char *suf = x86_size_suffix(size);
            const char *dst = x86_gpr_name(x86_phys_of(emit, info->def), size);
            const char *s = x86_gpr_name(x86_phys_of(emit, src), size);
            if (emit->failed)
                return;
            anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, s, dst);
            anvil_strbuf_appendf(&emit->code, "\tneg%s %%%s\n", suf, dst);
        }
    } else if (info->op == ANVIL_MIR_OP_FABS && def_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = x86_size_bytes(def_info->size_bits);
        const char *dst = x86_reg_name(emit, info->def);
        const char *s = x86_reg_name(emit, src);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", x86_fp_mov_op(size), s, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s .Lx86_fabs_mask%s, %%%s\n", size <= 4 ? "andps" : "andpd", size <= 4 ? "32" : "64", dst);
        emit->emitted_fabs_mask = true;
    } else if (info->op == ANVIL_MIR_OP_NOT && def_info->reg_class == ANVIL_MIR_REG_GPR) {
        int size = x86_size_bytes(def_info->size_bits);
        if (size < 4)
            size = 4;
        const char *suf = x86_size_suffix(size);
        const char *dst = x86_gpr_name(x86_phys_of(emit, info->def), size);
        const char *s = x86_gpr_name(x86_phys_of(emit, src), size);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", suf, s, dst);
        anvil_strbuf_appendf(&emit->code, "\tnot%s %%%s\n", suf, dst);
    } else {
        emit->failed = true;
    }
}

static void x86_emit_gpr_extend(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info, bool sign_extend)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = x86_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = x86_vreg_info_checked(emit, src);
    int dst_phys = x86_phys_of(emit, info->def);
    int src_phys = x86_phys_of(emit, src);
    if (!dst_info || !src_info || emit->failed)
        return;
    if (dst_info->reg_class != ANVIL_MIR_REG_GPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        emit->failed = true;
        return;
    }

    int src_size = x86_size_bytes(src_info->size_bits);
    int dst_size = x86_size_bytes(dst_info->size_bits);
    if (dst_size < src_size)
        dst_size = src_size;
    if (dst_size < 4)
        dst_size = 4;

    if (src_size >= 4) {
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x86_gpr_name(src_phys, 4), x86_gpr_name(dst_phys, 4));
        return;
    }

    const char *zs = sign_extend ? "movs" : "movz";
    if (src_size == 1 && !x86_reg_has_byte(src_phys)) {
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%eax\n", x86_gpr_name(src_phys, 4));
        anvil_strbuf_appendf(&emit->code, "\t%sbl %%%s, %%%s\n", zs, x86_reg8_names[X86_EAX], x86_gpr_name(dst_phys, 4));
        return;
    }
    const char *suffix = src_size == 1 ? "bl" : "wl";
    anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n", zs, suffix, x86_gpr_name(src_phys, src_size), x86_gpr_name(dst_phys, 4));
}

static void x86_emit_cast(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
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
    case ANVIL_MIR_OP_SITOFP: {
        if (dst_info->reg_class != ANVIL_MIR_REG_FPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
            emit->failed = true;
            return;
        }
        int dst_size = x86_size_bytes(dst_info->size_bits);
        const char *dst = x86_reg_name(emit, info->def);
        const char *s = x86_gpr_name(x86_phys_of(emit, src), 4);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%sl %%%s, %%%s\n", dst_size <= 4 ? "s" : "d", s, dst);
        break;
    }
    case ANVIL_MIR_OP_UITOFP: {
        if (dst_info->reg_class != ANVIL_MIR_REG_FPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
            emit->failed = true;
            return;
        }
        int dst_size = x86_size_bytes(dst_info->size_bits);
        const char *dst = x86_reg_name(emit, info->def);
        const char *s = x86_gpr_name(x86_phys_of(emit, src), 4);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, -%d(%%ebp)\n", s, emit->scratch_b_off);
        anvil_strbuf_append(&emit->code, "\tmovl $0, -");
        anvil_strbuf_appendf(&emit->code, "%d(%%ebp)\n", emit->scratch_b_off - 4);
        anvil_strbuf_appendf(&emit->code, "\tcvtsi2s%sq -%d(%%ebp), %%%s\n", dst_size <= 4 ? "s" : "d", emit->scratch_b_off, dst);
        break;
    }
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI: {
        if (dst_info->reg_class != ANVIL_MIR_REG_GPR || src_info->reg_class != ANVIL_MIR_REG_FPR) {
            emit->failed = true;
            return;
        }
        int src_size = x86_size_bytes(src_info->size_bits);
        const char *s = x86_reg_name(emit, src);
        const char *dst = x86_gpr_name(x86_phys_of(emit, info->def), 4);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\tcvtts%s2si %%%s, %%%s\n", src_size <= 4 ? "s" : "d", s, dst);
        break;
    }
    case ANVIL_MIR_OP_FPEXT: {
        const char *dst = x86_reg_name(emit, info->def);
        const char *s = x86_reg_name(emit, src);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\tcvtss2sd %%%s, %%%s\n", s, dst);
        break;
    }
    case ANVIL_MIR_OP_FPTRUNC: {
        const char *dst = x86_reg_name(emit, info->def);
        const char *s = x86_reg_name(emit, src);
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

static void x86_emit_frame_addr(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (info->frame_slot < 0 || (size_t)info->frame_slot >= emit->num_frame_slot_offsets) {
        emit->failed = true;
        return;
    }

    int dst_phys = x86_phys_of(emit, info->def);
    if (emit->failed)
        return;

    int offset = emit->frame_slot_offsets[info->frame_slot];
    anvil_strbuf_appendf(&emit->code, "\tleal -%d(%%ebp), %%%s\n", offset, x86_gpr_name(dst_phys, 4));
}

static void x86_emit_dyn_alloca(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
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

    const char *count_reg = x86_gpr_name(x86_phys_of(emit, count), 4);
    int dst_phys = x86_phys_of(emit, info->def);
    if (emit->failed)
        return;

    anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, -%d(%%ebp)\n", count_reg, emit->scratch_a_off);
    anvil_strbuf_appendf(&emit->code, "\tmovl -%d(%%ebp), %%eax\n", emit->scratch_a_off);
    if (info->imm != 1) {
        anvil_strbuf_appendf(&emit->code, "\timull $%lld, %%eax, %%eax\n", (long long)info->imm);
    }
    anvil_strbuf_append(&emit->code, "\taddl $15, %eax\n");
    anvil_strbuf_append(&emit->code, "\tandl $-16, %eax\n");
    anvil_strbuf_append(&emit->code, "\tsubl %eax, %esp\n");
    anvil_strbuf_appendf(&emit->code, "\tmovl %%esp, %%%s\n", x86_gpr_name(dst_phys, 4));
}

static void x86_emit_symbol_addr(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->symbol || !info->symbol[0]) {
        emit->failed = true;
        return;
    }

    int dst_phys = x86_phys_of(emit, info->def);
    if (emit->failed)
        return;

    const char *prefix = x86_symbol_ref_prefix(emit, info->symbol);
    const char *dst = x86_gpr_name(dst_phys, 4);
    anvil_strbuf_appendf(&emit->code, "\tmovl $%s%s", prefix, info->symbol);
    if (info->has_imm && info->imm != 0)
        anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
    anvil_strbuf_appendf(&emit->code, ", %%%s\n", dst);
}

static void x86_emit_load(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    int def_phys = x86_phys_of(emit, info->def);
    const char *base = x86_gpr_name(x86_phys_of(emit, ptr), 4);
    if (!def_info || emit->failed)
        return;

    int size = x86_size_bytes(def_info->size_bits);
    int64_t offset = info->has_imm ? info->imm : 0;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *op = size <= 4 ? "movss" : "movsd";
        const char *dst = x86_xmm_names[def_phys];
        if (offset == 0) {
            anvil_strbuf_appendf(&emit->code, "\t%s (%%%s), %%%s\n", op, base, dst);
        } else {
            anvil_strbuf_appendf(&emit->code, "\t%s %lld(%%%s), %%%s\n", op, (long long)offset, base, dst);
        }
        return;
    }

    const char *op;
    const char *dst;
    if (def_info->is_signed && size == 1) {
        op = "movsbl";
        dst = x86_gpr_name(def_phys, 4);
    } else if (def_info->is_signed && size == 2) {
        op = "movswl";
        dst = x86_gpr_name(def_phys, 4);
    } else if (!def_info->is_signed && size == 1) {
        op = "movzbl";
        dst = x86_gpr_name(def_phys, 4);
    } else if (!def_info->is_signed && size == 2) {
        op = "movzwl";
        dst = x86_gpr_name(def_phys, 4);
    } else {
        op = "movl";
        dst = x86_gpr_name(def_phys, 4);
    }

    if (offset == 0) {
        anvil_strbuf_appendf(&emit->code, "\t%s (%%%s), %%%s\n", op, base, dst);
    } else {
        anvil_strbuf_appendf(&emit->code, "\t%s %lld(%%%s), %%%s\n", op, (long long)offset, base, dst);
    }
}

static void x86_emit_store(x86_mir_emit_t *emit, size_t instr_index)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (value == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    const anvil_mir_vreg_info_t *value_info = x86_vreg_info_checked(emit, value);
    int ptr_phys = x86_phys_of(emit, ptr);
    const char *base = x86_gpr_name(ptr_phys, 4);
    if (!value_info || emit->failed)
        return;

    int size = x86_size_bytes(value_info->size_bits);
    const char *op;
    const char *src;
    if (value_info->reg_class == ANVIL_MIR_REG_FPR) {
        op = size <= 4 ? "movss" : "movsd";
        src = x86_reg_name(emit, value);
    } else {
        int value_phys = x86_phys_of(emit, value);
        op = size == 1 ? "movb" : size == 2 ? "movw" : "movl";
        if (size == 1 && !x86_reg_has_byte(value_phys)) {
            int scratch = ptr_phys == X86_EAX ? X86_ECX : X86_EAX;
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x86_gpr_name(value_phys, 4), x86_gpr_name(scratch, 4));
            src = x86_reg8_names[scratch];
        } else {
            src = x86_gpr_name(value_phys, size);
        }
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

static void x86_emit_incoming_stack_arg(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm < 0 || info->imm > INT32_MAX - 32) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    int def_phys = x86_phys_of(emit, info->def);
    if (!def_info || emit->failed)
        return;

    int size = x86_size_bytes(def_info->size_bits);
    int frame_offset = 8 + (int)info->imm;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *op = size <= 4 ? "movss" : "movsd";
        anvil_strbuf_appendf(&emit->code, "\t%s %d(%%ebp), %%%s\n", op, frame_offset, x86_xmm_names[def_phys]);
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
    anvil_strbuf_appendf(&emit->code, "\t%s %d(%%ebp), %%%s\n", op, frame_offset, x86_gpr_name(def_phys, 4));
}

static void x86_emit_call_stack_arg(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
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

    const anvil_mir_vreg_info_t *value_info = x86_vreg_info_checked(emit, value);
    if (!value_info || emit->failed)
        return;

    int size = x86_size_bytes(value_info->size_bits);
    int slot = (int)info->imm;
    if (value_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *op = size <= 4 ? "movss" : "movsd";
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %d(%%esp)\n", op, x86_reg_name(emit, value), slot);
        return;
    }
    const char *src = x86_gpr_name(x86_phys_of(emit, value), 4);
    if (emit->failed)
        return;
    anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %d(%%esp)\n", src, slot);
}

static int x86_call_stack_bytes(x86_mir_emit_t *emit, size_t call_index)
{
    int max_end = 0;
    anvil_mir_instr_info_t call_info;
    if (!anvil_mir_get_instr_info(emit->mir, call_index, &call_info))
        return 0;

    /* CALL_STACK_ARG belongs to the next CALL in the same block.  Walking
       backward to the previous call avoids folding an earlier call's frame
       into this call's stdcall/fastcall decoration. */
    for (size_t i = call_index; i-- > 0;) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->mir, i, &info))
            return 0;
        if (info.block != call_info.block)
            continue;
        if (info.op == ANVIL_MIR_OP_CALL)
            break;
        if (info.op != ANVIL_MIR_OP_CALL_STACK_ARG)
            continue;
        if (!info.has_imm)
            continue;
        int end = (int)info.imm + 4;
        if (end > max_end)
            max_end = end;
    }
    return max_end;
}

static void x86_emit_call(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    const anvil_x86_cc_desc_t *call_desc = anvil_x86_get_cc_desc(info->call_cc);
    if (!call_desc) {
        emit->failed = true;
        return;
    }
    bool direct = info->symbol && info->symbol[0];
    size_t arg_start = direct ? 0 : 1;

    for (size_t u = arg_start; u < info->num_uses; u++) {
        size_t idx = u - arg_start;
        if ((int)idx >= call_desc->num_reg_int_args)
            break;
        anvil_mir_vreg_t arg = anvil_mir_get_instr_use(emit->mir, instr_index, u);
        int arg_phys = x86_phys_of(emit, arg);
        int reg = call_desc->reg_int_args[idx];
        if (emit->failed)
            return;
        if (arg_phys != reg) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x86_gpr_name(arg_phys, 4), x86_reg32_names[reg]);
        }
    }

    if (direct) {
        const char *prefix = x86_symbol_ref_prefix(emit, info->symbol);
        char decorated[256];
        int stack_bytes = x86_call_stack_bytes(emit, instr_index);
        if (emit->plat->is_coff && call_desc->decor == X86_DECOR_STDCALL) {
            snprintf(decorated, sizeof(decorated), "%s%s@%d", prefix, info->symbol, stack_bytes);
            anvil_strbuf_appendf(&emit->code, "\tcall %s\n", decorated);
        } else if (emit->plat->is_coff && call_desc->decor == X86_DECOR_FASTCALL) {
            snprintf(decorated, sizeof(decorated), "@%s@%d", info->symbol, stack_bytes + (int)(info->num_uses * 4));
            anvil_strbuf_appendf(&emit->code, "\tcall %s\n", decorated);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tcall %s%s\n", prefix, info->symbol);
        }
    } else {
        anvil_mir_vreg_t target = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        if (target == ANVIL_MIR_NO_VREG) {
            emit->failed = true;
            return;
        }
        const char *target_reg = x86_gpr_name(x86_phys_of(emit, target), 4);
        if (emit->failed)
            return;
        anvil_strbuf_appendf(&emit->code, "\tcall *%%%s\n", target_reg);
    }

    if (info->def != ANVIL_MIR_NO_VREG) {
        int def_phys = x86_phys_of(emit, info->def);
        if (emit->failed)
            return;
        if (def_phys != call_desc->int_ret_reg) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x86_reg32_names[call_desc->int_ret_reg], x86_gpr_name(def_phys, 4));
        }
    }
}

static void x86_emit_spill_load(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
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

    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    int def_phys = x86_phys_of(emit, info->def);
    if (!def_info || emit->failed)
        return;

    int offset = emit->spill_offsets[info->spill_slot];
    int size = x86_size_bytes(slot.size_bits);
    if (slot.reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%ebp), %%%s\n", x86_fp_mov_op(size), offset, x86_xmm_names[def_phys]);
    } else {
        if (size < 4)
            size = 4;
        anvil_strbuf_appendf(&emit->code, "\tmov%s -%d(%%ebp), %%%s\n", x86_size_suffix(size), offset, x86_gpr_name(def_phys, size));
    }
}

static void x86_emit_spill_store(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
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

    int src_phys = x86_phys_of(emit, src_vreg);
    if (emit->failed)
        return;

    int offset = emit->spill_offsets[info->spill_slot];
    int size = x86_size_bytes(slot.size_bits);
    if (slot.reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, -%d(%%ebp)\n", x86_fp_mov_op(size), x86_xmm_names[src_phys], offset);
    } else {
        if (size < 4)
            size = 4;
        anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, -%d(%%ebp)\n", x86_size_suffix(size), x86_gpr_name(src_phys, size), offset);
    }
}

static void x86_emit_ret(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (info->num_uses == 1) {
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        const anvil_mir_vreg_info_t *ret_info = x86_vreg_info_checked(emit, ret);
        int ret_phys = x86_phys_of(emit, ret);
        if (!ret_info || emit->failed)
            return;

        if (ret_info->reg_class == ANVIL_MIR_REG_FPR) {
            int size = x86_size_bytes(ret_info->size_bits);
            const char *mov = x86_fp_mov_op(size);
            const char *fld = size <= 4 ? "flds" : "fldl";
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, -%d(%%ebp)\n", mov, x86_xmm_names[ret_phys], emit->scratch_b_off);
            anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%ebp)\n", fld, emit->scratch_b_off);
        } else {
            int size = x86_size_bytes(ret_info->size_bits);
            if (size < 4)
                size = 4;
            if (ret_phys != emit->desc->int_ret_reg) {
                anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x86_gpr_name(ret_phys, 4), x86_reg32_names[emit->desc->int_ret_reg]);
            }
        }
    }
    if (!emit->failed)
        x86_emit_epilogue(emit);
}

static void x86_emit_other(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (info->def == ANVIL_MIR_NO_VREG) {
        return;
    }
    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info || emit->failed)
        return;

    int phys = x86_phys_of(emit, info->def);
    if (emit->failed)
        return;

    if (def_info->reg_class == ANVIL_MIR_REG_GPR) {
        if (phys != emit->desc->int_ret_hi_reg) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x86_reg32_names[emit->desc->int_ret_hi_reg], x86_gpr_name(phys, 4));
        }
        return;
    }

    int size = x86_size_bytes(def_info->size_bits);
    const char *st = size <= 4 ? "fstps" : "fstpl";
    const char *mov = x86_fp_mov_op(size);
    anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%ebp)\n", st, emit->scratch_b_off);
    anvil_strbuf_appendf(&emit->code, "\t%s -%d(%%ebp), %%%s\n", mov, emit->scratch_b_off, x86_xmm_names[phys]);
}

static void x86_emit_instr(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    switch (info->op) {
    case ANVIL_MIR_OP_MOV:
        x86_emit_mov(emit, info);
        break;
    case ANVIL_MIR_OP_COPY: {
        anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        if (src == ANVIL_MIR_NO_VREG) {
            emit->failed = true;
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
        x86_emit_binary(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_NEG:
    case ANVIL_MIR_OP_NOT:
    case ANVIL_MIR_OP_FABS:
        x86_emit_unary(emit, instr_index, info);
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
        x86_emit_cmp(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SYMBOL_ADDR:
        x86_emit_symbol_addr(emit, info);
        break;
    case ANVIL_MIR_OP_LOAD:
        x86_emit_load(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_STORE:
        x86_emit_store(emit, instr_index);
        break;
    case ANVIL_MIR_OP_FRAME_ADDR:
        x86_emit_frame_addr(emit, info);
        break;
    case ANVIL_MIR_OP_DYN_ALLOCA:
        x86_emit_dyn_alloca(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        x86_emit_incoming_stack_arg(emit, info);
        break;
    case ANVIL_MIR_OP_CALL:
        x86_emit_call(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        x86_emit_call_stack_arg(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_BR:
        anvil_strbuf_append(&emit->code, "\tjmp ");
        if (!x86_emit_branch_target(emit, info->true_block)) {
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
        const anvil_mir_vreg_info_t *cond_info = x86_vreg_info_checked(emit, cond);
        if (!cond_info)
            break;
        const char *cond_reg = x86_gpr_name(x86_phys_of(emit, cond), 4);
        if (emit->failed)
            break;
        anvil_strbuf_appendf(&emit->code, "\ttestl %%%s, %%%s\n", cond_reg, cond_reg);
        anvil_strbuf_append(&emit->code, "\tjne ");
        if (!x86_emit_branch_target(emit, info->true_block)) {
            emit->failed = true;
            break;
        }
        anvil_strbuf_append(&emit->code, "\n\tjmp ");
        if (!x86_emit_branch_target(emit, info->false_block)) {
            emit->failed = true;
            break;
        }
        anvil_strbuf_append(&emit->code, "\n");
        break;
    }
    case ANVIL_MIR_OP_RET:
        x86_emit_ret(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SPILL_LOAD:
        x86_emit_spill_load(emit, info);
        break;
    case ANVIL_MIR_OP_SPILL_STORE:
        x86_emit_spill_store(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_RET_VALUE_PART: {
        anvil_mir_vreg_t hi = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        int hi_phys = x86_phys_of(emit, hi);
        if (emit->failed)
            break;
        if (hi_phys != emit->desc->int_ret_hi_reg) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x86_gpr_name(hi_phys, 4), x86_reg32_names[emit->desc->int_ret_hi_reg]);
        }
    } break;
    case ANVIL_MIR_OP_CALL_RESULT:
        x86_emit_other(emit, info);
        break;
    case ANVIL_MIR_OP_KEEPALIVE:
        break;
    default:
        emit->failed = true;
        break;
    }
}

static void x86_emit_escaped_string(anvil_strbuf_t *code, const char *value)
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

static void x86_emit_rodata(x86_mir_emit_t *emit)
{
    size_t count = anvil_mir_num_string_literals(emit->mir);
    if (count == 0 && !emit->emitted_fneg_mask && !emit->emitted_fabs_mask) {
        return;
    }

    if (emit->plat->is_macho) {
        anvil_strbuf_append(&emit->code, "\t.section __TEXT,__cstring,cstring_literals\n");
    } else if (emit->plat->is_coff) {
        anvil_strbuf_append(&emit->code, "\t.section .rdata,\"dr\"\n");
    } else {
        anvil_strbuf_append(&emit->code, "\t.section .rodata\n");
    }

    for (size_t i = 0; i < count; i++) {
        anvil_mir_string_literal_info_t info;
        if (!anvil_mir_get_string_literal_info(emit->mir, i, &info) || !info.label) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "%s:\n", info.label);
        x86_emit_escaped_string(&emit->code, info.value);
    }

    if (emit->emitted_fneg_mask) {
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_append(&emit->code, ".Lx86_fneg_mask32:\n");
        anvil_strbuf_append(&emit->code, "\t.long 0x80000000\n");
        anvil_strbuf_append(&emit->code, "\t.long 0\n\t.long 0\n\t.long 0\n");
        anvil_strbuf_append(&emit->code, ".Lx86_fneg_mask64:\n");
        anvil_strbuf_append(&emit->code, "\t.long 0\n");
        anvil_strbuf_append(&emit->code, "\t.long 0x80000000\n");
        anvil_strbuf_append(&emit->code, "\t.long 0\n\t.long 0\n");
    }
    if (emit->emitted_fabs_mask) {
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_append(&emit->code, ".Lx86_fabs_mask32:\n");
        anvil_strbuf_append(&emit->code, "\t.long 0x7fffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, ".Lx86_fabs_mask64:\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0x7fffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
        anvil_strbuf_append(&emit->code, "\t.long 0xffffffff\n");
    }
}

static int x86_compute_ret_pop(const anvil_mir_func_t *mir, const anvil_x86_cc_desc_t *desc, anvil_func_t *func)
{
    if (!desc->callee_cleans_stack || !func)
        return 0;
    (void)mir;
    int pop = 0;
    size_t int_reg_count = 0;
    for (size_t i = 0; i < func->num_params; i++) {
        anvil_value_t *param = func->params[i];
        anvil_type_t *type = param ? param->type : NULL;
        if (x86_needs_pair(type)) {
            pop += 8;
            continue;
        }
        if (!x86_type_is_float(type) && (int)int_reg_count < desc->num_reg_int_args) {
            int_reg_count++;
            continue;
        }
        pop += (int)x86_stack_arg_slot_size(type);
    }
    return pop;
}

bool anvil_x86_emit_mir_abi(const anvil_mir_func_t *mir, anvil_func_t *func, anvil_abi_t abi, anvil_syntax_t syntax, char **output, size_t *len)
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
    emit.mir = mir;
    emit.source_func = func;
    emit.desc = desc;
    emit.plat = plat;
    emit.syntax = syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : syntax;
    emit.ret_pop_bytes = x86_compute_ret_pop(mir, desc, func);
    anvil_strbuf_init(&emit.code);
    if (!emit.code.data)
        return false;

    if (!x86_prepare_frame(&emit)) {
        anvil_strbuf_destroy(&emit.code);
        free(emit.spill_offsets);
        free(emit.frame_slot_offsets);
        return false;
    }

    x86_emit_func_header(&emit);
    x86_emit_prologue(&emit);

    size_t num_blocks = anvil_mir_num_blocks(mir);
    size_t num_instrs = anvil_mir_num_instrs(mir);
    for (size_t b = 0; b < num_blocks && !emit.failed && !emit.code.failed; b++) {
        if (!x86_emit_label(&emit, (anvil_mir_block_t)b)) {
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
            x86_emit_instr(&emit, i, &info);
        }
    }

    if (!emit.failed && !emit.code.failed && !emit.plat->is_macho && !emit.plat->is_coff) {
        const char *prefix = x86_symbol_prefix(&emit);
        const char *name = anvil_mir_func_name(mir);
        anvil_strbuf_appendf(&emit.code, "\t.size %s%s, .-%s%s\n", prefix, name, prefix, name);
    }
    if (!emit.failed && !emit.code.failed) {
        x86_emit_rodata(&emit);
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

bool anvil_x86_emit_mir(const anvil_mir_func_t *mir, char **output, size_t *len)
{
    return anvil_x86_emit_mir_abi(mir, NULL, ANVIL_ABI_SYSV, ANVIL_SYNTAX_GAS, output, len);
}
