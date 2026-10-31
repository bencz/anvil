#include "gas32_internal.h"
#include "../../common/parallel_move.h"
#include "x86_gas.h"
#include "anvil/anvil_analysis.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int x86_align_int(int value, int align)
{
    return (value + align - 1) & ~(align - 1);
}

static int x86_slot_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0)
        return 4;
    int size = (int)((size_bits + 7) / 8);
    return size > 0 ? size : 4;
}

/* The immediate a GPR operand is known to hold.  Every i386 GPR value has
 * at most 32 bits, so any constant fits an instruction field. */
static bool x86_known_imm(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg, int64_t *out)
{
    int64_t value;
    if (!anvil_mir_isel_constant(facts, vreg, &value))
        return false;
    *out = (int32_t)value;
    return true;
}

/* Whether the selection below encodes operand `use` as an immediate
 * (mirrors x86_emit_gpr_binary and x86_emit_cmp).  Called while the facts
 * are built, after their constants are known. */
static bool x86_use_takes_immediate(void *ctx, size_t index, const anvil_mir_instr_info_t *info, size_t use, int64_t value)
{
    const x86_mir_emit_t *emit = ctx;
    const anvil_mir_vreg_info_t *def = info->def != ANVIL_MIR_NO_VREG ? anvil_mir_get_vreg_info(emit->mir, info->def) : NULL;
    int64_t other;
    (void)value;
    if (!def || def->reg_class != ANVIL_MIR_REG_GPR || info->num_uses != 2)
        return false;
    switch (info->op) {
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_MUL:
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR:
        /* The right operand when constant, else a constant left one. */
        return use == 1 || !x86_known_imm(&emit->isel, anvil_mir_get_instr_use(emit->mir, index, 1), &other);
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
        return use == 1;
    default:
        if (anvil_mir_isel_is_compare(info->op) && info->op != ANVIL_MIR_OP_FCMP) {
            const anvil_mir_vreg_info_t *lhs = anvil_mir_get_vreg_info(emit->mir, anvil_mir_get_instr_use(emit->mir, index, 0));
            return use == 1 && lhs && lhs->reg_class == ANVIL_MIR_REG_GPR;
        }
        return false;
    }
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
        return def && def->reg_class == ANVIL_MIR_REG_GPR && !x86_known_imm(facts, anvil_mir_get_instr_use(mir, index, 1), &count)
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

bool x86_annotate_printer_registers(anvil_mir_func_t *mir)
{
    anvil_mir_isel_facts_t facts;
    if (!anvil_mir_isel_facts_build(&facts, mir, NULL, NULL))
        return false;
    bool ok = true;
    for (size_t index = 0; ok && index < anvil_mir_num_instrs(mir); index++) {
        anvil_mir_instr_info_t info;
        ok = anvil_mir_get_instr_info(mir, index, &info);
        uint64_t clobbers = ok ? x86_printer_gpr_clobbers(&info) : 0;
        ok = ok && anvil_mir_set_instr_scratch(mir, index, ANVIL_MIR_REG_GPR, x86_printer_gpr_scratch(mir, &facts, index, &info)) &&
             (!clobbers || anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, info.clobbers[ANVIL_MIR_REG_GPR] | clobbers));
    }
    anvil_mir_isel_facts_destroy(&facts);
    return ok;
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

static void x86_parallel_move_reg(void *user, int dst, int src)
{
    x86_mir_emit_t *emit = user;
    anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x86_reg32_names[src], x86_reg32_names[dst]);
}

static void x86_parallel_swap_reg(void *user, int a, int b)
{
    x86_mir_emit_t *emit = user;
    anvil_strbuf_appendf(&emit->code, "\txchgl %%%s, %%%s\n", x86_reg32_names[a], x86_reg32_names[b]);
}

static void x86_emit_parallel_move(x86_mir_emit_t *emit, const int *dst, const int *src, size_t count)
{
    static const anvil_parallel_move_ops_t ops = { x86_parallel_move_reg, x86_parallel_swap_reg, -1 };
    if (!anvil_parallel_move(&ops, emit, dst, src, count))
        emit->failed = true;
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

    int fp_bytes = emit->fp->frame_bytes(emit);
    if (fp_bytes > 0) {
        offset = x86_align_int(offset, 8) + fp_bytes;
        emit->fp_area_off = offset;
    }

    emit->num_spill_offsets = anvil_mir_num_spills(emit->mir);
    emit->num_frame_slot_offsets = anvil_mir_num_frame_slots(emit->mir);
    if (emit->num_frame_slot_offsets > 0) {
        emit->frame_slot_offsets = calloc(emit->num_frame_slot_offsets, sizeof(*emit->frame_slot_offsets));
        if (!emit->frame_slot_offsets)
            return false;
    }

    /* Addressed downward from EBP; the stack guarantees 16-byte alignment. */
    if (!anvil_mir_layout_frame_slots(emit->mir, offset, 4, 16, true, emit->frame_slot_offsets, &offset))
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
        offset += x86_align_int(x86_slot_size_bytes(slot.size_bits), 4);
        emit->spill_offsets[i] = offset;
    }

    offset += emit->outgoing_size;
    /* Entry leaves ESP 4 bytes below an aligned boundary and the EBP push
     * another 4: the frame restores the alignment for calls. */
    int align = emit->plat->stack_alignment;
    emit->frame_size = x86_align_int(offset + 8, align) - 8;
    return true;
}

static void x86_emit_func_header(x86_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);
    const char *prefix = x86_symbol_prefix(emit);
    const anvil_obj_format_t *format = emit->plat->format;
    anvil_linkage_t linkage = anvil_mir_func_linkage(emit->mir);
    char symbol[512];
    snprintf(symbol, sizeof(symbol), "%s%s", prefix, name);
    if (emit->plat->is_macho) {
        anvil_strbuf_append(&emit->code, format->text_section);
        anvil_obj_emit_binding(&emit->code, format, linkage, symbol);
        anvil_strbuf_append(&emit->code, "\t.p2align 4\n");
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    } else if (emit->plat->is_coff) {
        char decorated[512];
        if (!emit->source_func ||
            !x86_decorated_symbol(emit->plat, emit->desc, emit->source_func->type, name, decorated, sizeof(decorated)))
            snprintf(decorated, sizeof(decorated), "%s%s", prefix, name);
        anvil_strbuf_append(&emit->code, format->text_section);
        anvil_obj_emit_binding(&emit->code, format, linkage, decorated);
        anvil_strbuf_appendf(&emit->code, "\t.def %s; .scl %d; .type 32; .endef\n", decorated, linkage == ANVIL_LINK_INTERNAL ? 3 : 2);
        anvil_strbuf_appendf(&emit->code, "%s:\n", decorated);
    } else {
        anvil_strbuf_append(&emit->code, format->text_section);
        anvil_obj_emit_binding(&emit->code, format, linkage, symbol);
        anvil_strbuf_appendf(&emit->code, "\t.type %s%s, @function\n", prefix, name);
        anvil_strbuf_appendf(&emit->code, "%s%s:\n", prefix, name);
    }
}

static void x86_emit_prologue(x86_mir_emit_t *emit)
{
    bool cfi = emit->plat->format->cfi;
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_startproc\n");
    anvil_strbuf_append(&emit->code, "\tpushl %ebp\n");
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_def_cfa_offset 8\n\t.cfi_offset %ebp, -8\n");
    anvil_strbuf_append(&emit->code, "\tmovl %esp, %ebp\n");
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_def_cfa_register %ebp\n");
    if (emit->frame_size > 0) {
        anvil_strbuf_appendf(&emit->code, "\tsubl $%d, %%esp\n", emit->frame_size);
    }

    for (int reg = 0; reg < 8; reg++) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, -%d(%%ebp)\n", x86_reg32_names[reg], emit->gpr_save_offsets[reg]);
            if (cfi)
                anvil_strbuf_appendf(&emit->code, "\t.cfi_offset %%%s, %d\n", x86_reg32_names[reg], -emit->gpr_save_offsets[reg] - 8);
        }
    }
}

/* `pop_bytes`: what the function drops from the caller's stack, as the
 * lowering recorded on RET. */
static void x86_emit_epilogue(x86_mir_emit_t *emit, int64_t pop_bytes)
{
    bool cfi = emit->plat->format->cfi;
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_remember_state\n");
    for (int reg = 7; reg >= 0; reg--) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            anvil_strbuf_appendf(&emit->code, "\tmovl -%d(%%ebp), %%%s\n", emit->gpr_save_offsets[reg], x86_reg32_names[reg]);
        }
    }
    anvil_strbuf_append(&emit->code, "\tmovl %ebp, %esp\n");
    anvil_strbuf_append(&emit->code, "\tpopl %ebp\n");
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_def_cfa %esp, 4\n");
    if (pop_bytes > 0) {
        anvil_strbuf_appendf(&emit->code, "\tret $%lld\n", (long long)pop_bytes);
    } else {
        anvil_strbuf_append(&emit->code, "\tret\n");
    }
    if (cfi)
        anvil_strbuf_append(&emit->code, "\t.cfi_restore_state\n");
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
    if (!dst_info || !src_info || emit->failed)
        return;

    if (dst_info->reg_class == ANVIL_MIR_REG_FPR || src_info->reg_class == ANVIL_MIR_REG_FPR) {
        emit->fp->copy(emit, dst, src);
        return;
    }
    int dst_phys = x86_phys_of(emit, dst);
    int src_phys = x86_phys_of(emit, src);
    if (emit->failed || dst_phys == src_phys)
        return;
    int size = x86_size_bytes(dst_info->size_bits);
    if (size < 4)
        size = 4;
    anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %%%s\n", x86_size_suffix(size), x86_gpr_name(src_phys, size), x86_gpr_name(dst_phys, size));
}

static void x86_emit_mov(x86_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    if (!def_info || !anvil_mir_isel_materialize(&emit->isel, info->def))
        return;
    int dst_phys = x86_phys_of(emit, info->def);
    if (emit->failed)
        return;

    int64_t imm = info->has_imm ? info->imm : 0;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        emit->fp->constant(emit, info->def, (uint64_t)imm);
        return;
    }

    /* xor for zero: the flags are dead here (a compare fused with its
     * branch is adjacent to it). */
    const char *dst = x86_gpr_name(dst_phys, 4);
    if ((int32_t)imm == 0)
        anvil_strbuf_appendf(&emit->code, "\txorl %%%s, %%%s\n", dst, dst);
    else
        anvil_strbuf_appendf(&emit->code, "\tmovl $%lld, %%%s\n", (long long)(int32_t)imm, dst);
}

/* Integer arithmetic.  ADD..XOR and MUL work on the full registers (the
 * low bits of a narrow result are the same); shifts use the operand width.
 * Variable shift counts go through CL, the instruction's printer scratch
 * (no operand or result is there); division uses EDX:EAX, which only
 * clobbers. */
static void x86_emit_gpr_binary(x86_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info, int dst_phys, int a_phys, int b_phys,
                                int size)
{
    int wsize = size < 4 ? 4 : size;
    const char *wsuf = x86_size_suffix(wsize);
    const char *a = x86_gpr_name(a_phys, wsize), *b = x86_gpr_name(b_phys, wsize), *dst = x86_gpr_name(dst_phys, wsize);
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, index, 1);
    if (emit->failed)
        return;

    bool commutative = info->op == ANVIL_MIR_OP_ADD || info->op == ANVIL_MIR_OP_MUL || info->op == ANVIL_MIR_OP_AND ||
                       info->op == ANVIL_MIR_OP_OR || info->op == ANVIL_MIR_OP_XOR;
    const char *mnemonic = info->op == ANVIL_MIR_OP_ADD ? "add" : info->op == ANVIL_MIR_OP_SUB ? "sub" : info->op == ANVIL_MIR_OP_MUL ? "imul"
                         : info->op == ANVIL_MIR_OP_AND ? "and" : info->op == ANVIL_MIR_OP_OR ? "or" : info->op == ANVIL_MIR_OP_XOR ? "xor" : NULL;
    int64_t imm = 0;
    bool b_imm = x86_known_imm(&emit->isel, rhs, &imm);
    if (!b_imm && commutative && x86_known_imm(&emit->isel, lhs, &imm)) {
        /* x OP c == c OP x */
        b_imm = true;
        a_phys = b_phys;
        a = b;
    }

    if (mnemonic && b_imm) {
        if (info->op == ANVIL_MIR_OP_MUL && imm > 0 && (imm & (imm - 1)) == 0) {
            int shift = 0;
            while ((INT64_C(1) << shift) != imm)
                shift++;
            if (dst_phys != a_phys)
                anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", a, dst);
            if (shift != 0)
                anvil_strbuf_appendf(&emit->code, "\tshll $%d, %%%s\n", shift, dst);
        } else if (info->op == ANVIL_MIR_OP_MUL) {
            anvil_strbuf_appendf(&emit->code, "\timull $%lld, %%%s, %%%s\n", (long long)imm, a, dst);
        } else if (dst_phys != a_phys && (info->op == ANVIL_MIR_OP_ADD || (info->op == ANVIL_MIR_OP_SUB && imm != INT32_MIN))) {
            long long offset = info->op == ANVIL_MIR_OP_ADD ? (long long)imm : -(long long)imm;
            anvil_strbuf_appendf(&emit->code, "\tleal %lld(%%%s), %%%s\n", offset, a, dst);
        } else {
            if (dst_phys != a_phys)
                anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", a, dst);
            anvil_strbuf_appendf(&emit->code, "\t%s%s $%lld, %%%s\n", mnemonic, wsuf, (long long)imm, dst);
        }
        return;
    }

    if (mnemonic) {
        if (dst_phys == a_phys) {
            anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n", mnemonic, wsuf, b, dst);
        } else if (dst_phys != b_phys) {
            if (info->op == ANVIL_MIR_OP_ADD)
                anvil_strbuf_appendf(&emit->code, "\tleal (%%%s,%%%s), %%%s\n", a, b, dst);
            else
                anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n\t%s%s %%%s, %%%s\n", a, dst, mnemonic, wsuf, b, dst);
        } else if (commutative) {
            anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n", mnemonic, wsuf, a, dst);
        } else {
            /* dst holds the subtrahend: a - b == -b + a. */
            anvil_strbuf_appendf(&emit->code, "\tnegl %%%s\n\taddl %%%s, %%%s\n", dst, a, dst);
        }
        return;
    }

    switch (info->op) {
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR: {
        const char *sh = info->op == ANVIL_MIR_OP_SHL ? "shl" : info->op == ANVIL_MIR_OP_SHR ? "shr" : "sar";
        const char *suf = x86_size_suffix(size);
        const char *nd = x86_gpr_name(dst_phys, size);
        int64_t count = 0;
        if (size == 1 && !x86_reg_has_byte(dst_phys)) {
            emit->failed = true;
            return;
        }
        if (x86_known_imm(&emit->isel, rhs, &count)) {
            if (dst_phys != a_phys)
                anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", a, dst);
            anvil_strbuf_appendf(&emit->code, "\t%s%s $%d, %%%s\n", sh, suf, (int)(count & 31), nd);
            break;
        }
        /* dst may share b's register: b reaches CL first. */
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%ecx\n", b);
        if (dst_phys != a_phys)
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", a, dst);
        anvil_strbuf_appendf(&emit->code, "\t%s%s %%cl, %%%s\n", sh, suf, nd);
        break;
    }
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
            anvil_strbuf_appendf(&emit->code, "\tpushl %%%s\n", b);
        if (a_phys != X86_EAX)
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%eax\n", a);
        anvil_strbuf_append(&emit->code, is_signed ? "\tcltd\n" : "\txorl %edx, %edx\n");
        if (divisor_in_pair)
            anvil_strbuf_appendf(&emit->code, "\t%sl (%%esp)\n\taddl $4, %%esp\n", is_signed ? "idiv" : "div");
        else
            anvil_strbuf_appendf(&emit->code, "\t%sl %%%s\n", is_signed ? "idiv" : "div", b);
        int result = is_mod ? X86_EDX : X86_EAX;
        if (dst_phys != result)
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x86_reg32_names[result], dst);
        break;
    }
    default:
        emit->failed = true;
        break;
    }
}

/* A scalar operation the table prices: an FP unit instruction, or the high
 * half of a product (EDX:EAX, which the instruction clobbers). */
static void x86_emit_scalar(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    const anvil_vector_isa_t *isa = anvil_vector_isa_for_mir(x86_scalar_isa, x86_scalar_isa_count, emit->mir, instr_index, info, emit->features);
    anvil_mir_vreg_t a = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (!isa || a == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    if (info->ir_op != ANVIL_OP_UMULH && info->ir_op != ANVIL_OP_SMULH) {
        emit->fp->apply(emit, isa->mnemonic, info->def, a);
        return;
    }
    anvil_mir_vreg_t b = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    int a_phys = x86_phys_of(emit, a);
    int b_phys = b == ANVIL_MIR_NO_VREG ? -1 : x86_phys_of(emit, b);
    int dst_phys = x86_phys_of(emit, info->def);
    if (b_phys < 0 || emit->failed) {
        emit->failed = true;
        return;
    }
    /* The product commutes: a multiplier already in EAX is the one kept
     * there (MUL reads the other before it writes EDX). */
    if (b_phys == X86_EAX) {
        b_phys = a_phys;
        a_phys = X86_EAX;
    }
    if (a_phys != X86_EAX)
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%eax\n", x86_reg32_names[a_phys]);
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n", isa->mnemonic, x86_reg32_names[b_phys]);
    if (dst_phys != X86_EDX)
        anvil_strbuf_appendf(&emit->code, "\tmovl %%edx, %%%s\n", x86_reg32_names[dst_phys]);
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
        emit->fp->binary(emit, info->op, info->def, lhs, rhs);
        return;
    }

    int size = x86_size_bytes(def_info->size_bits);
    int dst_phys = x86_phys_of(emit, info->def);
    int a_phys = x86_phys_of(emit, lhs);
    int b_phys = x86_phys_of(emit, rhs);
    if (emit->failed)
        return;
    x86_emit_gpr_binary(emit, instr_index, info, dst_phys, a_phys, b_phys, size);
}

/* Whether the compare at `instr_index` hands its flags to the BR_COND right
 * after it (fused_jcc) instead of materializing a boolean. */
static bool x86_fuses(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    return anvil_mir_isel_feeds_next_branch(&emit->isel, instr_index, info);
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

    /* Booleans live in the byte registers (the allocator's narrow ones). */
    const char *dst8 = x86_byte_reg_name(dst_phys);
    const char *dst32 = x86_gpr_name(dst_phys, 4);
    bool fuses = x86_fuses(emit, instr_index, info);
    if (!dst8 && !fuses) {
        emit->failed = true;
        return;
    }

    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *branch_cc;
        bool swap;
        if (info->op == ANVIL_MIR_OP_FCMP && fuses && x86_gas_fcmp_branch((anvil_fcmp_pred_t)info->imm, &branch_cc, &swap)) {
            emit->fp->compare(emit, swap ? rhs : lhs, swap ? lhs : rhs);
            emit->fused_jcc = branch_cc;
            return;
        }
        if (!dst8) {
            emit->failed = true;
            return;
        }
        emit->fp->compare(emit, lhs, rhs);
        if (emit->failed)
            return;
        if (info->op == ANVIL_MIR_OP_FCMP)
            x86_gas_emit_fcmp_bool(&emit->code, emit->plat->format->local_label_prefix, anvil_mir_func_name(emit->mir), instr_index,
                                   (anvil_fcmp_pred_t)info->imm, dst8);
        else
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n", x86_gas_setcc(info->op), dst8);
        anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", dst8, dst32);
        return;
    }

    int size = x86_size_bytes(lhs_info->size_bits);
    const char *suf = x86_size_suffix(size);
    int a_phys = x86_phys_of(emit, lhs);
    int b_phys = x86_phys_of(emit, rhs);
    const char *a = x86_gpr_name(a_phys, size);
    if (emit->failed)
        return;
    if (size == 1 && !x86_reg_has_byte(a_phys)) {
        emit->failed = true;
        return;
    }
    int64_t imm = 0;
    if (x86_known_imm(&emit->isel, rhs, &imm)) {
        imm = size == 1 ? (int8_t)imm : size == 2 ? (int16_t)imm : imm;
        if (imm == 0)
            anvil_strbuf_appendf(&emit->code, "\ttest%s %%%s, %%%s\n", suf, a, a);
        else
            anvil_strbuf_appendf(&emit->code, "\tcmp%s $%lld, %%%s\n", suf, (long long)imm, a);
    } else {
        if (size == 1 && !x86_reg_has_byte(b_phys)) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\tcmp%s %%%s, %%%s\n", suf, x86_gpr_name(b_phys, size), a);
    }

    if (fuses) {
        emit->fused_jcc = x86_gas_jcc(info->op);
        return;
    }
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s\n", x86_gas_setcc(info->op), dst8);
    anvil_strbuf_appendf(&emit->code, "\tmovzbl %%%s, %%%s\n", dst8, dst32);
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
            emit->fp->unary(emit, info->op, info->def, src);
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
        emit->fp->unary(emit, info->op, info->def, src);
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

    /* A 32-bit source, or a narrow load of the same signedness, which
     * already extended the value (x86_emit_load). */
    if (src_size >= 4 || (src_info->is_signed == sign_extend && anvil_mir_isel_def_op(&emit->isel, src) == ANVIL_MIR_OP_LOAD)) {
        if (src_phys != dst_phys)
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x86_gpr_name(src_phys, 4), x86_gpr_name(dst_phys, 4));
        return;
    }

    const char *zs = sign_extend ? "movs" : "movz";
    if (src_size == 1 && !x86_reg_has_byte(src_phys)) {
        emit->failed = true;
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
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP:
        if (dst_info->reg_class != ANVIL_MIR_REG_FPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
            emit->failed = true;
            return;
        }
        emit->fp->convert(emit, info->op, info->def, src);
        break;
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI:
        if (dst_info->reg_class != ANVIL_MIR_REG_GPR || src_info->reg_class != ANVIL_MIR_REG_FPR) {
            emit->failed = true;
            return;
        }
        emit->fp->convert(emit, info->op, info->def, src);
        break;
    case ANVIL_MIR_OP_FPEXT:
    case ANVIL_MIR_OP_FPTRUNC:
        if (dst_info->reg_class != ANVIL_MIR_REG_FPR || src_info->reg_class != ANVIL_MIR_REG_FPR) {
            emit->failed = true;
            return;
        }
        emit->fp->convert(emit, info->op, info->def, src);
        break;
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

    /* EAX is the instruction's clobber: the count is read first. */
    anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%eax\n", count_reg);
    if (info->imm != 1) {
        anvil_strbuf_appendf(&emit->code, "\timull $%lld, %%eax, %%eax\n", (long long)info->imm);
    }
    anvil_strbuf_append(&emit->code, "\taddl $15, %eax\n");
    anvil_strbuf_append(&emit->code, "\tandl $-16, %eax\n");
    /* Relocate the outgoing area below each dynamic allocation; otherwise
     * CALL_STACK_ARG overwrites the buffer. */
    if (emit->outgoing_size)
        anvil_strbuf_appendf(&emit->code, "\taddl $%d, %%eax\n", emit->outgoing_size);
    anvil_strbuf_append(&emit->code, "\tsubl %eax, %esp\n");
    anvil_strbuf_appendf(&emit->code, "\tleal %d(%%esp), %%%s\n", emit->outgoing_size, x86_gpr_name(dst_phys, 4));
}

void x86_emit_got_pointer(x86_mir_emit_t *emit, const char *reg)
{
    const char *function = anvil_mir_func_name(emit->mir);
    unsigned label = emit->pic_labels++;
    anvil_strbuf_appendf(&emit->code, "\tcall .L%s_pic%u\n.L%s_pic%u:\n\tpopl %%%s\n", function, label, function, label, reg);
    anvil_strbuf_appendf(&emit->code, "\taddl $_GLOBAL_OFFSET_TABLE_+(.-.L%s_pic%u), %%%s\n", function, label, reg);
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
    if (emit->reloc != ANVIL_RELOC_STATIC) {
        /* From the GOT, or GOT-relative for this module's own symbols. */
        x86_emit_got_pointer(emit, dst);
        if (info->symbol_got) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %s%s@GOT(%%%s), %%%s\n", prefix, info->symbol, dst, dst);
            if (info->has_imm && info->imm != 0)
                anvil_strbuf_appendf(&emit->code, "\taddl $%lld, %%%s\n", (long long)info->imm, dst);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tleal %s%s@GOTOFF", prefix, info->symbol);
            if (info->has_imm && info->imm != 0)
                anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
            anvil_strbuf_appendf(&emit->code, "(%%%s), %%%s\n", dst, dst);
        }
        return;
    }
    anvil_strbuf_appendf(&emit->code, "\tmovl $%s%s", prefix, info->symbol);
    if (info->has_imm && info->imm != 0)
        anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
    anvil_strbuf_appendf(&emit->code, ", %%%s\n", dst);
}

/* disp(%base) or disp(%base,%index,scale) of a LOAD/STORE whose base is use
 * `base_use` (an index, when present, follows it). */
static bool x86_memory_operand(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info, size_t base_use, char *out,
                               size_t out_len)
{
    anvil_mir_vreg_t base = anvil_mir_get_instr_use(emit->mir, instr_index, base_use);
    anvil_mir_vreg_t index = info->index_scale ? anvil_mir_get_instr_use(emit->mir, instr_index, base_use + 1) : ANVIL_MIR_NO_VREG;
    if (base == ANVIL_MIR_NO_VREG || (info->index_scale && index == ANVIL_MIR_NO_VREG)) {
        emit->failed = true;
        return false;
    }
    const char *base_name = x86_gpr_name(x86_phys_of(emit, base), 4);
    char disp[24] = "";
    if (info->has_imm && info->imm != 0)
        snprintf(disp, sizeof(disp), "%lld", (long long)info->imm);
    if (info->index_scale)
        snprintf(out, out_len, "%s(%%%s,%%%s,%u)", disp, base_name, x86_gpr_name(x86_phys_of(emit, index), 4), info->index_scale);
    else
        snprintf(out, out_len, "%s(%%%s)", disp, base_name);
    return !emit->failed;
}

static void x86_emit_load(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def_info = x86_vreg_info_checked(emit, info->def);
    int def_phys = x86_phys_of(emit, info->def);
    char address[96];
    if (!def_info || emit->failed || !x86_memory_operand(emit, instr_index, info, 0, address, sizeof(address)))
        return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        emit->fp->load(emit, info->def, address);
        return;
    }

    /* Narrow values load extended to 32 bits. */
    int size = x86_size_bytes(def_info->size_bits);
    const char *op = size == 1 ? (def_info->is_signed ? "movsbl" : "movzbl") : size == 2 ? (def_info->is_signed ? "movswl" : "movzwl") : "movl";
    anvil_strbuf_appendf(&emit->code, "\t%s %s, %%%s\n", op, address, x86_gpr_name(def_phys, 4));
}

static void x86_emit_store(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    const anvil_mir_vreg_info_t *value_info = value == ANVIL_MIR_NO_VREG ? NULL : x86_vreg_info_checked(emit, value);
    char address[96];
    if (!value_info || emit->failed || !x86_memory_operand(emit, instr_index, info, 1, address, sizeof(address))) {
        emit->failed = true;
        return;
    }

    if (value_info->reg_class == ANVIL_MIR_REG_FPR) {
        emit->fp->store(emit, value, address);
        return;
    }

    int size = x86_size_bytes(value_info->size_bits);
    int value_phys = x86_phys_of(emit, value);
    if (emit->failed || (size == 1 && !x86_reg_has_byte(value_phys))) {
        emit->failed = true;
        return;
    }
    anvil_strbuf_appendf(&emit->code, "\tmov%s %%%s, %s\n", x86_size_suffix(size), x86_gpr_name(value_phys, size), address);
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
        char mem[32];
        snprintf(mem, sizeof(mem), "%d(%%ebp)", frame_offset);
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

    int slot = (int)info->imm;
    if (value_info->reg_class == ANVIL_MIR_REG_FPR) {
        char mem[32];
        snprintf(mem, sizeof(mem), "%d(%%esp)", slot);
        emit->fp->store(emit, value, mem);
        return;
    }
    const char *src = x86_gpr_name(x86_phys_of(emit, value), 4);
    if (emit->failed)
        return;
    anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %d(%%esp)\n", src, slot);
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
        move_src[num_moves] = x86_phys_of(emit, anvil_mir_get_instr_use(emit->mir, instr_index, u));
        move_dst[num_moves++] = call_desc->reg_int_args[idx];
        if (emit->failed)
            return;
    }
    int target_phys = direct ? -1 : x86_phys_of(emit, anvil_mir_get_instr_use(emit->mir, instr_index, 0));
    for (size_t m = 0; target_phys >= 0 && m < num_moves; m++) {
        if (move_dst[m] == target_phys) {
            move_src[num_moves] = target_phys;
            move_dst[num_moves++] = X86_EAX;
            target_phys = X86_EAX;
            break;
        }
    }
    if (emit->failed)
        return;
    x86_emit_parallel_move(emit, move_dst, move_src, num_moves);

    if (direct) {
        /* Lowering already spelled COFF stdcall/fastcall names in full. */
        bool decorated = emit->plat->is_coff && call_desc->decor != X86_DECOR_NONE;
        const char *prefix = decorated ? "" : x86_symbol_ref_prefix(emit, info->symbol);
        if (info->symbol_got && emit->reloc != ANVIL_RELOC_STATIC) {
            /* Through the GOT entry (EAX carries no argument here). */
            x86_emit_got_pointer(emit, "eax");
            anvil_strbuf_appendf(&emit->code, "\tcall *%s%s@GOT(%%eax)\n", prefix, info->symbol);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tcall %s%s\n", prefix, info->symbol);
        }
    } else {
        anvil_strbuf_appendf(&emit->code, "\tcall *%%%s\n", x86_gpr_name(target_phys, 4));
    }

    /* A callee that dropped stack bytes leaves ESP above the outgoing
     * argument area the frame reserved. */
    if (info->has_imm && info->imm > 0)
        anvil_strbuf_appendf(&emit->code, "\tsubl $%lld, %%esp\n", (long long)info->imm);

    /* A 32-bit CALL_RESULT right after the call takes EDX; both moves
     * happen together. */
    int def_phys = info->def != ANVIL_MIR_NO_VREG ? x86_phys_of(emit, info->def) : -1;
    int hi_phys = -1;
    anvil_mir_instr_info_t next;
    if (anvil_mir_get_instr_info(emit->mir, instr_index + 1, &next) && next.op == ANVIL_MIR_OP_CALL_RESULT &&
        next.block == info->block) {
        const anvil_mir_vreg_info_t *next_info = x86_vreg_info_checked(emit, next.def);
        if (next_info && next_info->reg_class == ANVIL_MIR_REG_GPR) {
            hi_phys = x86_phys_of(emit, next.def);
            emit->call_hi_moved = true;
        }
    }
    if (!emit->failed)
        x86_emit_call_pair_move(emit, def_phys, hi_phys, call_desc->int_ret_reg, call_desc->int_ret_hi_reg);
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
        char mem[32];
        snprintf(mem, sizeof(mem), "-%d(%%ebp)", offset);
        emit->fp->load(emit, info->def, mem);
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
        char mem[32];
        snprintf(mem, sizeof(mem), "-%d(%%ebp)", offset);
        emit->fp->store(emit, src_vreg, mem);
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
            emit->fp->push_result(emit, ret);
        } else {
            x86_emit_result_pair_move(emit, ret_phys, emit->ret_hi_phys, emit->desc->int_ret_reg, emit->desc->int_ret_hi_reg);
        }
    }
    emit->ret_hi_phys = -1;
    if (!emit->failed)
        x86_emit_epilogue(emit, info->has_imm ? info->imm : 0);
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
        if (emit->call_hi_moved) {
            emit->call_hi_moved = false;
            return;
        }
        if (phys != emit->desc->int_ret_hi_reg) {
            anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %%%s\n", x86_reg32_names[emit->desc->int_ret_hi_reg], x86_gpr_name(phys, 4));
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
            anvil_strbuf_append(&emit->code, (emit->features & ANVIL_FEATURE_X86_SSE2) ? "\tmfence\n" : "\tlock orl $0, (%esp)\n");
        return;
    }

    anvil_mir_vreg_t value = info->def != ANVIL_MIR_NO_VREG ? info->def : anvil_mir_get_instr_use(emit->mir, index, 1);
    const anvil_mir_vreg_info_t *type = x86_vreg_info_checked(emit, value);
    if (!type || emit->failed)
        return;

    int dst[3] = {X86_EDX, X86_ECX, X86_EAX};
    if (info->ir_op == ANVIL_OP_ATOMIC_CMPXCHG) {
        dst[1] = X86_EAX;
        dst[2] = X86_ECX;
    }
    int src[3];
    size_t count = info->num_uses < 3 ? info->num_uses : 3;
    for (size_t operand = 0; operand < count; operand++)
        src[operand] = x86_phys_of(emit, anvil_mir_get_instr_use(emit->mir, index, operand));
    if (emit->failed)
        return;
    x86_emit_parallel_move(emit, dst, src, count);

    int size = x86_size_bytes(type->size_bits);
    const char *suffix = x86_size_suffix(size);
    const char *accumulator = x86_gpr_name(X86_EAX, size);
    const char *operand = x86_gpr_name(X86_ECX, size);
    switch (info->ir_op) {
    case ANVIL_OP_ATOMIC_LOAD:
        anvil_strbuf_appendf(&emit->code, "\tmov%s (%%edx), %%%s\n", suffix, accumulator);
        break;
    case ANVIL_OP_ATOMIC_STORE:
        anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, (%%edx)\n", info->atomic.order == ANVIL_ORDER_SEQ_CST ? "xchg" : "mov", suffix, operand);
        return;
    case ANVIL_OP_ATOMIC_CMPXCHG:
        anvil_strbuf_appendf(&emit->code, "\tlock cmpxchg%s %%%s, (%%edx)\n", suffix, operand);
        break;
    case ANVIL_OP_ATOMIC_RMW:
        if (info->atomic.rmw <= ANVIL_ATOMIC_SUB) {
            if (info->atomic.rmw == ANVIL_ATOMIC_SUB)
                anvil_strbuf_appendf(&emit->code, "\tneg%s %%%s\n", suffix, operand);
            const char *operation = info->atomic.rmw == ANVIL_ATOMIC_EXCHANGE ? "xchg" : "lock xadd";
            anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, (%%edx)\n\tmovl %%ecx, %%eax\n", operation, suffix, operand);
        } else {
            /* EBX (callee-saved, allocatable) is borrowed around the loop. */
            const char *operation = info->atomic.rmw == ANVIL_ATOMIC_AND ? "and" : (info->atomic.rmw == ANVIL_ATOMIC_OR ? "or" : "xor");
            const char *temporary = x86_gpr_name(X86_EBX, size);
            const char *prefix = emit->plat->format->local_label_prefix;
            const char *function = anvil_mir_func_name(emit->mir);
            anvil_strbuf_appendf(&emit->code, "\tpushl %%ebx\n\tmov%s (%%edx), %%%s\n%s%s_atomic_%zu:\n", suffix, accumulator, prefix, function, index);
            anvil_strbuf_appendf(&emit->code, "\tmovl %%eax, %%ebx\n\t%s%s %%%s, %%%s\n", operation, suffix, operand, temporary);
            anvil_strbuf_appendf(&emit->code, "\tlock cmpxchg%s %%%s, (%%edx)\n\tjne %s%s_atomic_%zu\n\tpopl %%ebx\n", suffix, temporary, prefix, function, index);
        }
        break;
    default:
        emit->failed = true;
        return;
    }

    /* Narrow results are kept extended, as loads leave them. */
    if (info->def != ANVIL_MIR_NO_VREG) {
        int def_phys = x86_phys_of(emit, info->def);
        if (emit->failed)
            return;
        if (size < 4)
            anvil_strbuf_appendf(&emit->code, "\tmov%c%cl %%%s, %%%s\n", type->is_signed ? 's' : 'z', size == 1 ? 'b' : 'w',
                                 accumulator, x86_reg32_names[def_phys]);
        else if (def_phys != X86_EAX)
            anvil_strbuf_appendf(&emit->code, "\tmovl %%eax, %%%s\n", x86_reg32_names[def_phys]);
    }
}

static void x86_emit_instr(x86_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    switch (info->op) {
    case ANVIL_MIR_OP_ATOMIC:
        x86_emit_atomic(emit, instr_index, info);
        return;
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
    case ANVIL_MIR_OP_SCALAR:
        x86_emit_scalar(emit, instr_index, info);
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
        x86_emit_store(emit, instr_index, info);
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
    case ANVIL_MIR_OP_VA_START:
        /* Address of the first unnamed stack argument. */
        anvil_strbuf_appendf(&emit->code, "\tleal %lld(%%ebp), %%%s\n", (long long)(8 + info->imm),
                             x86_gpr_name(x86_phys_of(emit, info->def), 4));
        break;
    case ANVIL_MIR_OP_CALL:
        x86_emit_call(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        x86_emit_call_stack_arg(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_BR:
        emit->failed |= !x86_gas_emit_jump(&emit->code, emit->plat->format->local_label_prefix, emit->mir, emit->block, info->true_block);
        break;
    case ANVIL_MIR_OP_BR_COND: {
        const char *cc = emit->fused_jcc;
        emit->fused_jcc = NULL;
        if (!cc) {
            anvil_mir_vreg_t cond = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
            const anvil_mir_vreg_info_t *cond_info = cond == ANVIL_MIR_NO_VREG ? NULL : x86_vreg_info_checked(emit, cond);
            int cond_phys = cond_info ? x86_phys_of(emit, cond) : -1;
            if (!cond_info || emit->failed) {
                emit->failed = true;
                break;
            }
            /* A boolean tests in its byte register. */
            int size = x86_reg_has_byte(cond_phys) ? x86_size_bytes(cond_info->size_bits) : 4;
            const char *reg = x86_gpr_name(cond_phys, size);
            anvil_strbuf_appendf(&emit->code, "\ttest%s %%%s, %%%s\n", x86_size_suffix(size), reg, reg);
            cc = "ne";
        }
        emit->failed |= !x86_gas_emit_cond_branch(&emit->code, emit->plat->format->local_label_prefix, emit->mir, emit->block, cc,
                                                  info->true_block, info->false_block);
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
    case ANVIL_MIR_OP_RET_VALUE_PART:
        /* Moved together with RET's value. */
        emit->ret_hi_phys = x86_phys_of(emit, anvil_mir_get_instr_use(emit->mir, instr_index, 0));
        break;
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
    const anvil_obj_format_t *format = emit->plat->format;
    const char *name = anvil_mir_func_name(emit->mir);
    size_t count = anvil_mir_num_string_literals(emit->mir);
    if (count > 0)
        anvil_strbuf_append(&emit->code, format->cstring_section);
    for (size_t i = 0; i < count; i++) {
        anvil_mir_string_literal_info_t info;
        if (!anvil_mir_get_string_literal_info(emit->mir, i, &info) || !info.label) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "%s:\n", info.label);
        x86_emit_escaped_string(&emit->code, info.value);
    }

    /* Sign masks, per function so several functions can use them. */
    if (emit->emitted_fneg_mask || emit->emitted_fabs_mask) {
        anvil_strbuf_append(&emit->code, format->literal16_section);
        anvil_obj_emit_align(&emit->code, 16);
    }
    if (emit->emitted_fneg_mask) {
        anvil_strbuf_appendf(&emit->code, "%s%s_fneg_mask32:\n\t.long 0x80000000, 0, 0, 0\n", emit->plat->format->local_label_prefix, name);
        anvil_strbuf_appendf(&emit->code, "%s%s_fneg_mask64:\n\t.long 0, 0x80000000, 0, 0\n", emit->plat->format->local_label_prefix, name);
    }
    if (emit->emitted_fabs_mask) {
        anvil_strbuf_appendf(&emit->code, "%s%s_fabs_mask32:\n\t.long 0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff\n", emit->plat->format->local_label_prefix, name);
        anvil_strbuf_appendf(&emit->code, "%s%s_fabs_mask64:\n\t.long 0xffffffff, 0x7fffffff, 0xffffffff, 0xffffffff\n", emit->plat->format->local_label_prefix, name);
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
    emit.mir = mir;
    emit.source_func = func;
    emit.desc = desc;
    emit.plat = plat;
    emit.syntax = syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : syntax;
    emit.features = features;
    emit.reloc = reloc == ANVIL_RELOC_DEFAULT || !plat->format->got_relocations ? ANVIL_RELOC_STATIC : reloc;
    /* Floating point in SSE2 registers where the CPU model has them,
     * otherwise on the x87 FPU. */
    emit.fp = (features & ANVIL_FEATURE_X86_SSE2) ? &x86_fp_sse2_unit : &x86_fp_x87_unit;
    emit.ret_hi_phys = -1;
    anvil_strbuf_init(&emit.code);
    if (!emit.code.data)
        return false;

    if (!x86_prepare_frame(&emit)) {
        anvil_strbuf_destroy(&emit.code);
        free(emit.spill_offsets);
        free(emit.frame_slot_offsets);
        return false;
    }

    if (!anvil_mir_isel_facts_build(&emit.isel, mir, x86_use_takes_immediate, &emit)) {
        anvil_strbuf_destroy(&emit.code);
        free(emit.spill_offsets);
        free(emit.frame_slot_offsets);
        return false;
    }

    x86_emit_func_header(&emit);
    x86_emit_prologue(&emit);

    size_t num_blocks = anvil_mir_num_blocks(mir);
    size_t num_instrs = anvil_mir_num_instrs(mir);
    /* Loop headers start aligned (padding outside the loop is cheap). */
    bool *loop_headers = calloc(num_blocks ? num_blocks : 1, sizeof(*loop_headers));
    if (!loop_headers || !anvil_mir_loop_header_blocks(mir, loop_headers))
        emit.failed = true;
    for (size_t b = 0; b < num_blocks && !emit.failed && !emit.code.failed; b++) {
        if (loop_headers[b])
            anvil_strbuf_append(&emit.code, "\t.p2align 4\n");
        if (!x86_gas_emit_label(&emit.code, emit.plat->format->local_label_prefix, mir, (anvil_mir_block_t)b)) {
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
            x86_emit_instr(&emit, i, &info);
        }
    }

    if (!emit.failed && !emit.code.failed && emit.plat->format->cfi)
        anvil_strbuf_append(&emit.code, "\t.cfi_endproc\n");
    if (!emit.failed && !emit.code.failed && !emit.plat->is_macho && !emit.plat->is_coff) {
        const char *prefix = x86_symbol_prefix(&emit);
        const char *name = anvil_mir_func_name(mir);
        anvil_strbuf_appendf(&emit.code, "\t.size %s%s, .-%s%s\n", prefix, name, prefix, name);
    }
    if (!emit.failed && !emit.code.failed) {
        x86_emit_rodata(&emit);
    }

    free(loop_headers);
    anvil_mir_isel_facts_destroy(&emit.isel);
    free(emit.spill_offsets);
    free(emit.frame_slot_offsets);
    if (emit.failed || emit.code.failed) {
        anvil_strbuf_destroy(&emit.code);
        return false;
    }

    *output = anvil_strbuf_detach(&emit.code, len);
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
