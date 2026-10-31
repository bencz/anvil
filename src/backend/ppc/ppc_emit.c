#include "ppc_internal.h"
#include "../common/parallel_move.h"

const char *const ppc_gpr_names[32] = {"%r0",  "%r1",  "%r2",  "%r3",  "%r4",  "%r5",  "%r6",  "%r7",  "%r8",  "%r9",  "%r10", "%r11", "%r12", "%r13", "%r14", "%r15",
                                      "%r16", "%r17", "%r18", "%r19", "%r20", "%r21", "%r22", "%r23", "%r24", "%r25", "%r26", "%r27", "%r28", "%r29", "%r30", "%r31"};

const char *const ppc_fpr_names[32] = {"%f0",  "%f1",  "%f2",  "%f3",  "%f4",  "%f5",  "%f6",  "%f7",  "%f8",  "%f9",  "%f10", "%f11", "%f12", "%f13", "%f14", "%f15",
                                      "%f16", "%f17", "%f18", "%f19", "%f20", "%f21", "%f22", "%f23", "%f24", "%f25", "%f26", "%f27", "%f28", "%f29", "%f30", "%f31"};

/* The 64 VSX registers: vs0-vs31 widen the FPRs, vs32-vs63 are the
 * AltiVec registers v0-v31 (the vector register class). */
static const char *const ppc_vsr_names[64] = {"%vs0", "%vs1", "%vs2", "%vs3", "%vs4", "%vs5", "%vs6", "%vs7", "%vs8", "%vs9", "%vs10", "%vs11", "%vs12", "%vs13", "%vs14", "%vs15", "%vs16", "%vs17", "%vs18", "%vs19", "%vs20", "%vs21", "%vs22", "%vs23", "%vs24", "%vs25", "%vs26", "%vs27", "%vs28", "%vs29", "%vs30", "%vs31", "%vs32", "%vs33", "%vs34", "%vs35", "%vs36", "%vs37", "%vs38", "%vs39", "%vs40", "%vs41", "%vs42", "%vs43", "%vs44", "%vs45", "%vs46", "%vs47", "%vs48", "%vs49", "%vs50", "%vs51", "%vs52", "%vs53", "%vs54", "%vs55", "%vs56", "%vs57", "%vs58", "%vs59", "%vs60", "%vs61", "%vs62", "%vs63"};
static const char *const ppc_vr_names[32] = {"%v0", "%v1", "%v2", "%v3", "%v4", "%v5", "%v6", "%v7", "%v8", "%v9", "%v10", "%v11", "%v12", "%v13", "%v14", "%v15", "%v16", "%v17", "%v18", "%v19", "%v20", "%v21", "%v22", "%v23", "%v24", "%v25", "%v26", "%v27", "%v28", "%v29", "%v30", "%v31"};

/* A vector-class register as a VSX instruction names it. */
static const char *ppc_vector_vsr(int vr)
{
    return ppc_vsr_names[32 + vr];
}

static int align_int(int value, int align)
{
    return (value + align - 1) & ~(align - 1);
}

static int ppc_mir_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0)
        return 8;
    int size = (int)((size_bits + 7) / 8);
    if (size <= 0)
        return 8;
    if (size > 8)
        return size > 16 ? 8 : 16;
    return size;
}

static bool ppc_is_64(const ppc_mir_emit_t *emit)
{
    return emit->desc->word_size == 8;
}

static const char *ppc_store_word_op(const ppc_mir_emit_t *emit)
{
    return ppc_is_64(emit) ? "std" : "stw";
}

static const char *ppc_load_word_op(const ppc_mir_emit_t *emit)
{
    return ppc_is_64(emit) ? "ld" : "lwz";
}

static const anvil_mir_vreg_info_t *ppc_vreg_info_checked(ppc_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->mir, vreg);
    if (!info)
        emit->failed = true;
    return info;
}

static const anvil_regalloc_assignment_t *ppc_assignment_checked(ppc_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(emit->mir, vreg);
    if (!assignment || assignment->spilled || assignment->phys_reg < 0 || assignment->phys_reg >= 32) {
        emit->failed = true;
        return NULL;
    }
    return assignment;
}

static const char *ppc_reg_name(ppc_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = ppc_vreg_info_checked(emit, vreg);
    const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, vreg);
    if (!info || !assignment)
        return "?";
    if (info->reg_class == ANVIL_MIR_REG_VECTOR)
        return ppc_vector_vsr(assignment->phys_reg);
    if (info->reg_class == ANVIL_MIR_REG_FPR)
        return ppc_fpr_names[assignment->phys_reg];
    return ppc_gpr_names[assignment->phys_reg];
}

static bool ppc_offset_fits_dform(int64_t offset)
{
    return offset >= -32768 && offset <= 32767;
}

static void ppc_emit_load_imm(ppc_mir_emit_t *emit, int reg, int64_t imm)
{
    if (reg < 0 || reg >= 32) {
        emit->failed = true;
        return;
    }

    const char *r = ppc_gpr_names[reg];
    if (imm >= -32768 && imm <= 32767) {
        anvil_strbuf_appendf(&emit->code, "\tli %s, %lld\n", r, (long long)imm);
        return;
    }

    if (!ppc_is_64(emit) || (imm >= INT32_MIN && imm <= INT32_MAX)) {
        uint32_t raw = (uint32_t)imm;
        uint32_t hi = (raw >> 16) & 0xffffu;
        uint32_t lo = raw & 0xffffu;
        anvil_strbuf_appendf(&emit->code, "\tlis %s, %u\n", r, hi);
        if (lo != 0) {
            anvil_strbuf_appendf(&emit->code, "\tori %s, %s, %u\n", r, r, lo);
        }
        return;
    }

    uint64_t raw = (uint64_t)imm;
    uint16_t c0 = (uint16_t)((raw >> 48) & 0xffffu);
    uint16_t c1 = (uint16_t)((raw >> 32) & 0xffffu);
    uint16_t c2 = (uint16_t)((raw >> 16) & 0xffffu);
    uint16_t c3 = (uint16_t)(raw & 0xffffu);
    anvil_strbuf_appendf(&emit->code, "\tlis %s, %u\n", r, (unsigned)c0);
    anvil_strbuf_appendf(&emit->code, "\tori %s, %s, %u\n", r, r, (unsigned)c1);
    anvil_strbuf_appendf(&emit->code, "\tsldi %s, %s, 32\n", r, r);
    if (c2 != 0) {
        anvil_strbuf_appendf(&emit->code, "\toris %s, %s, %u\n", r, r, (unsigned)c2);
    }
    if (c3 != 0) {
        anvil_strbuf_appendf(&emit->code, "\tori %s, %s, %u\n", r, r, (unsigned)c3);
    }
}

/* DS-form instructions encode displacement/4. */
static bool ppc_op_is_ds_form(const char *op)
{
    return strcmp(op, "ld") == 0 || strcmp(op, "std") == 0 || strcmp(op, "lwa") == 0;
}

/* `op reg, offset(base)`: D/DS form when the displacement fits, otherwise
 * the indexed form with the offset in r0 (r0 is never allocated, and as the
 * index operand it is an ordinary register).  `base` must not be r0. */
/* VSX vector loads and stores have only the indexed form. */
static bool ppc_op_is_x_only(const char *op)
{
    return strncmp(op, "lxv", 3) == 0 || strncmp(op, "stxv", 4) == 0;
}

void ppc_emit_mem_access(ppc_mir_emit_t *emit, const char *op, const char *reg, const char *base, int64_t offset)
{
    if (ppc_op_is_x_only(op) && offset == 0) {
        anvil_strbuf_appendf(&emit->code, "\t%sx %s, 0, %s\n", op, reg, base);
        return;
    }
    if (!ppc_op_is_x_only(op) && ppc_offset_fits_dform(offset) && (!ppc_op_is_ds_form(op) || (offset & 3) == 0)) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %lld(%s)\n", op, reg, (long long)offset, base);
        return;
    }
    ppc_emit_load_imm(emit, 0, offset);
    anvil_strbuf_appendf(&emit->code, "\t%sx %s, %s, %%r0\n", op, reg, base);
}

void ppc_emit_addi_large(ppc_mir_emit_t *emit, int dst, int base, int64_t offset)
{
    if (ppc_offset_fits_dform(offset)) {
        anvil_strbuf_appendf(&emit->code, "\taddi %s, %s, %lld\n", ppc_gpr_names[dst], ppc_gpr_names[base], (long long)offset);
        return;
    }

    ppc_emit_load_imm(emit, 0, offset);
    anvil_strbuf_appendf(&emit->code, "\tadd %s, %s, %%r0\n", ppc_gpr_names[dst], ppc_gpr_names[base]);
}

static void ppc_emit_local_access(ppc_mir_emit_t *emit, const char *op, const char *reg, int offset)
{
    if (offset < 0) {
        emit->failed = true;
        return;
    }

    ppc_emit_mem_access(emit, op, reg, "%r31", -(int64_t)offset);
}

static void ppc_emit_sp_access(ppc_mir_emit_t *emit, const char *op, const char *reg, int offset)
{
    if (offset < 0) {
        emit->failed = true;
        return;
    }

    ppc_emit_mem_access(emit, op, reg, "%r1", offset);
}

static void ppc_emit_incoming_stack_access(ppc_mir_emit_t *emit, const char *op, const char *reg, int offset)
{
    if (offset < 0) {
        emit->failed = true;
        return;
    }

    ppc_emit_mem_access(emit, op, reg, "%r31", offset);
}

static const char *ppc_load_op(const anvil_ppc_target_desc_t *desc, anvil_mir_reg_class_t reg_class, int size, bool is_signed)
{
    if (reg_class == ANVIL_MIR_REG_VECTOR)
        return "lxvd2";
    if (reg_class == ANVIL_MIR_REG_FPR)
        return size <= 4 ? "lfs" : "lfd";
    if (is_signed) {
        switch (size) {
        case 1:
            return "lbz";
        case 2:
            return "lha";
        case 4:
            return desc->word_size == 8 ? "lwa" : "lwz";
        default:
            return "ld";
        }
    }
    switch (size) {
    case 1:
        return "lbz";
    case 2:
        return "lhz";
    case 4:
        return "lwz";
    default:
        return "ld";
    }
}

static const char *ppc_store_op(const anvil_ppc_target_desc_t *desc, anvil_mir_reg_class_t reg_class, int size)
{
    (void)desc;
    if (reg_class == ANVIL_MIR_REG_VECTOR)
        return "stxvd2";
    if (reg_class == ANVIL_MIR_REG_FPR)
        return size <= 4 ? "stfs" : "stfd";
    switch (size) {
    case 1:
        return "stb";
    case 2:
        return "sth";
    case 4:
        return "stw";
    default:
        return "std";
    }
}

static void ppc_emit_sign_extend_loaded_byte(ppc_mir_emit_t *emit, const anvil_mir_vreg_info_t *info, const char *reg, int size)
{
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR || !info->is_signed)
        return;
    if (size == 1) {
        anvil_strbuf_appendf(&emit->code, "\textsb %s, %s\n", reg, reg);
    }
}

static void ppc_emit_base_offset_access(ppc_mir_emit_t *emit, const char *op, const char *reg, const char *base, int64_t offset)
{
    ppc_emit_mem_access(emit, op, reg, base, offset);
}

static bool ppc_emit_label(ppc_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info))
        return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s:\n", anvil_mir_func_name(emit->mir), name);
    return true;
}

static bool ppc_emit_branch_target(ppc_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info))
        return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s", anvil_mir_func_name(emit->mir), name);
    return true;
}

/* Integer <-> floating conversions the CPU performs inline (64-bit
 * implementations: fcfid/fctidz; ISA 2.06: the unsigned and single forms);
 * the rest call a run-time helper. */
static bool ppc_conversion_is_native(anvil_cpu_features_t features, anvil_mir_opcode_t op, uint16_t int_bits, bool single)
{
    bool wide = int_bits > 32;
    bool int64 = (features & ANVIL_FEATURE_PPC_64BIT) != 0;
    bool fpcvt = (features & ANVIL_FEATURE_PPC_FPCVT) != 0;
    switch (op) {
    case ANVIL_MIR_OP_FPTOSI:
        return !wide || int64;
    case ANVIL_MIR_OP_FPTOUI:
        return fpcvt || (!wide && int64);
    case ANVIL_MIR_OP_SITOFP:
        /* fcfid then frsp would round twice for a wide integer. */
        return !wide || (int64 && (!single || fpcvt));
    case ANVIL_MIR_OP_UITOFP:
        return !wide || fpcvt;
    default:
        return false;
    }
}

static bool ppc_is_conversion(anvil_mir_opcode_t op)
{
    return op == ANVIL_MIR_OP_SITOFP || op == ANVIL_MIR_OP_UITOFP || op == ANVIL_MIR_OP_FPTOSI || op == ANVIL_MIR_OP_FPTOUI;
}

/* Conversion operand shape: the integer side's width and whether the
 * floating side is single precision. */
static bool ppc_conversion_shape(const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *info, uint16_t *int_bits, bool *single)
{
    const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, info->def);
    const anvil_mir_vreg_info_t *src = info->num_uses == 1 ? anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, 0)) : NULL;
    if (!def || !src)
        return false;
    bool to_fp = info->op == ANVIL_MIR_OP_SITOFP || info->op == ANVIL_MIR_OP_UITOFP;
    *int_bits = to_fp ? src->size_bits : def->size_bits;
    *single = (to_fp ? def->size_bits : src->size_bits) <= 32;
    return true;
}

static bool ppc_conversion_native_at(const ppc_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info)
{
    uint16_t int_bits;
    bool single;
    return ppc_conversion_shape(emit->mir, index, info, &int_bits, &single) && ppc_conversion_is_native(emit->features, info->op, int_bits, single);
}

bool ppc_conversion_calls_helper(const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *info, anvil_cpu_features_t features)
{
    uint16_t int_bits;
    bool single;
    return ppc_is_conversion(info->op) &&
           !(ppc_conversion_shape(mir, index, info, &int_bits, &single) && ppc_conversion_is_native(features, info->op, int_bits, single));
}

/* What the printer itself adds that needs a frame record beyond
 * anvil_mir_needs_frame_record: calls to conversion helpers, and the
 * bcl/mflr of 32-bit position-independent addresses (they change LR). */
static bool ppc_printer_needs_frame_record(const ppc_mir_emit_t *emit)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(emit->mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->mir, i, &info))
            return true;
        if (ppc_is_conversion(info.op) && !ppc_conversion_native_at(emit, i, &info))
            return true;
        if (info.op == ANVIL_MIR_OP_SYMBOL_ADDR && emit->reloc != ANVIL_RELOC_STATIC && !ppc_is_64(emit))
            return true;
    }
    return false;
}

static bool ppc_instr_needs_fp_scratch(const ppc_mir_emit_t *emit)
{
    const anvil_mir_func_t *mir = emit->mir;
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr))
            return true;

        if (instr.def == ANVIL_MIR_NO_VREG) {
            continue;
        }
        const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(mir, instr.def);
        if (!def_info)
            return true;

        /* Inline conversions move values through memory. */
        if (ppc_is_conversion(instr.op) && ppc_conversion_native_at(emit, i, &instr))
            return true;

        if (instr.op == ANVIL_MIR_OP_MOV && instr.has_imm && def_info->reg_class == ANVIL_MIR_REG_FPR) {
            return true;
        }

        if ((instr.op == ANVIL_MIR_OP_COPY || instr.op == ANVIL_MIR_OP_BITCAST) && instr.num_uses == 1) {
            anvil_mir_vreg_t src = anvil_mir_get_instr_use(mir, i, 0);
            const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(mir, src);
            if (!src_info)
                return true;
            if (src_info->reg_class != def_info->reg_class && (src_info->reg_class == ANVIL_MIR_REG_FPR || def_info->reg_class == ANVIL_MIR_REG_FPR)) {
                return true;
            }
        }
    }
    return false;
}

static bool ppc_scan_outgoing_stack_args(ppc_mir_emit_t *emit)
{
    int outgoing_size = 0;

    for (size_t i = 0; i < anvil_mir_num_instrs(emit->mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->mir, i, &info))
            return false;
        /* A variadic callee may dump its argument registers into the
         * caller's parameter save area. */
        if (info.op == ANVIL_MIR_OP_CALL && info.has_imm && outgoing_size < (int)emit->desc->abi_ops->variadic_save_area)
            outgoing_size = (int)emit->desc->abi_ops->variadic_save_area;
        if (info.op != ANVIL_MIR_OP_CALL_STACK_ARG)
            continue;
        if (!info.has_imm || info.imm < 0 || info.num_uses != 1)
            return false;
        if (info.imm > INT32_MAX - 8)
            return false;

        anvil_mir_vreg_t arg = anvil_mir_get_instr_use(emit->mir, i, 0);
        const anvil_mir_vreg_info_t *arg_info = anvil_mir_get_vreg_info(emit->mir, arg);
        if (!arg_info)
            return false;

        int slot_size = align_int(ppc_mir_size_bytes(arg_info->size_bits), (int)emit->desc->word_size);
        if (slot_size < (int)emit->desc->word_size) {
            slot_size = (int)emit->desc->word_size;
        }
        int end = (int)info.imm + slot_size;
        if (end > outgoing_size)
            outgoing_size = end;
    }

    emit->outgoing_size = align_int(outgoing_size, 16);
    return true;
}

static bool ppc_prepare_frame(ppc_mir_emit_t *emit)
{
    for (size_t i = 0; i < 32; i++) {
        emit->gpr_save_offsets[i] = -1;
        emit->fpr_save_offsets[i] = -1;
    }
    emit->fp_const_scratch_offset = -1;

    if (!ppc_scan_outgoing_stack_args(emit))
        return false;

    int word = (int)emit->desc->word_size;
    int offset = word;
    emit->gpr_save_offsets[31] = offset;

    for (size_t i = 0; i < anvil_mir_num_vregs(emit->mir); i++) {
        const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(emit->mir, (anvil_mir_vreg_t)i);
        if (!assignment || assignment->spilled)
            continue;

        if (assignment->reg_class == ANVIL_MIR_REG_GPR && assignment->phys_reg >= 14 && assignment->phys_reg <= 30 && emit->gpr_save_offsets[assignment->phys_reg] < 0) {
            offset += word;
            emit->gpr_save_offsets[assignment->phys_reg] = offset;
        } else if (assignment->reg_class == ANVIL_MIR_REG_FPR && assignment->phys_reg >= 14 && assignment->phys_reg <= 31 && emit->fpr_save_offsets[assignment->phys_reg] < 0) {
            offset = align_int(offset, 8);
            offset += 8;
            emit->fpr_save_offsets[assignment->phys_reg] = offset;
        }
    }

    emit->num_frame_slot_offsets = anvil_mir_num_frame_slots(emit->mir);
    if (emit->num_frame_slot_offsets > 0) {
        emit->frame_slot_offsets = calloc(emit->num_frame_slot_offsets, sizeof(*emit->frame_slot_offsets));
        if (!emit->frame_slot_offsets)
            return false;
    }

    /* The ABIs keep the stack 16-byte aligned. */
    if (!anvil_mir_layout_frame_slots(emit->mir, offset, (uint32_t)word, 16, true, emit->frame_slot_offsets, &offset))
        return false;

    emit->num_spill_offsets = anvil_mir_num_spills(emit->mir);
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
        offset = align_int(offset, ppc_mir_size_bytes(slot.size_bits));
        offset += ppc_mir_size_bytes(slot.size_bits);
        emit->spill_offsets[i] = offset;
    }

    if (ppc_instr_needs_fp_scratch(emit)) {
        offset = align_int(offset, 8);
        offset += 8;
        emit->fp_const_scratch_offset = offset;
    }

    int outgoing_end = (int)emit->desc->outgoing_arg_offset + emit->outgoing_size;
    int needed = offset + outgoing_end;
    if (needed < outgoing_end)
        return false;

    /* Own bytes beyond r31's slot (the linkage area of outgoing_end only
     * matters to a function that calls). */
    emit->has_frame = offset > word || emit->outgoing_size > 0 || anvil_mir_needs_frame_record(emit->mir) || ppc_printer_needs_frame_record(emit) ||
                      ppc_instr_needs_fp_scratch(emit);
    if (!emit->has_frame) {
        emit->frame_size = 0;
        return true;
    }

    if (needed < (int)emit->desc->min_frame_size) {
        needed = (int)emit->desc->min_frame_size;
    }
    emit->frame_size = align_int(needed, 16);
    return true;
}

static void ppc_emit_frame_adjust(ppc_mir_emit_t *emit, const char *op, int frame_size)
{
    if (frame_size <= 32767) {
        anvil_strbuf_appendf(&emit->code, "\t%s %%r1, %s%d(%%r1)\n", ppc_is_64(emit) ? "stdu" : "stwu", op[0] == '-' ? "-" : "", frame_size);
        return;
    }

    ppc_emit_load_imm(emit, 11, frame_size);
    anvil_strbuf_append(&emit->code, "\tneg %r11, %r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s %%r1, %%r1, %%r11\n", ppc_is_64(emit) ? "stdux" : "stwux");
}

static void ppc_emit_prologue(ppc_mir_emit_t *emit)
{
    emit->desc->abi_ops->emit_function_header(emit);
    /* DWARF numbers: r0-r31 0-31, f0-f31 32-63, LR 65. The CFA is the
     * caller's SP, which r12 and then r31 hold once the frame exists. */
    anvil_strbuf_append(&emit->code, "\t.cfi_startproc\n");
    if (!emit->has_frame)
        return;

    anvil_strbuf_append(&emit->code, "\tmflr %r0\n");
    anvil_strbuf_appendf(&emit->code, "\t%s %%r0, %u(%%r1)\n", ppc_store_word_op(emit), emit->desc->lr_save_offset);
    anvil_strbuf_appendf(&emit->code, "\t.cfi_offset 65, %u\n", emit->desc->lr_save_offset);

    /* r12 (dead at entry once the TOC is set) keeps the old SP, so r31 is
     * saved inside the new frame with a short displacement for any frame
     * size, and nothing is stored below the stack pointer. */
    anvil_strbuf_append(&emit->code, "\tmr %r12, %r1\n\t.cfi_def_cfa_register 12\n");
    ppc_emit_frame_adjust(emit, "-", emit->frame_size);
    ppc_emit_mem_access(emit, ppc_store_word_op(emit), "%r31", "%r12", -(int64_t)emit->gpr_save_offsets[31]);
    anvil_strbuf_appendf(&emit->code, "\t.cfi_offset 31, %d\n", -emit->gpr_save_offsets[31]);
    anvil_strbuf_append(&emit->code, "\tmr %r31, %r12\n\t.cfi_def_cfa_register 31\n");

    for (int reg = 14; reg <= 30; reg++) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            ppc_emit_local_access(emit, ppc_store_word_op(emit), ppc_gpr_names[reg], emit->gpr_save_offsets[reg]);
            anvil_strbuf_appendf(&emit->code, "\t.cfi_offset %d, %d\n", reg, -emit->gpr_save_offsets[reg]);
        }
    }
    for (int reg = 14; reg <= 31; reg++) {
        if (emit->fpr_save_offsets[reg] >= 0) {
            ppc_emit_local_access(emit, "stfd", ppc_fpr_names[reg], emit->fpr_save_offsets[reg]);
            anvil_strbuf_appendf(&emit->code, "\t.cfi_offset %d, %d\n", 32 + reg, -emit->fpr_save_offsets[reg]);
        }
    }

    /* A variadic callee homes its argument registers for va_arg before
     * anything can overwrite them. */
    for (size_t index = 0; index < anvil_mir_num_instrs(emit->mir); index++) {
        anvil_mir_instr_info_t info;
        if (anvil_mir_get_instr_info(emit->mir, index, &info) && info.op == ANVIL_MIR_OP_VA_START) {
            if (!emit->desc->abi_ops->emit_va_register_save)
                emit->failed = true;
            else
                emit->desc->abi_ops->emit_va_register_save(emit, &info);
            break;
        }
    }
}

static void ppc_emit_epilogue(ppc_mir_emit_t *emit)
{
    if (emit->has_frame)
        anvil_strbuf_append(&emit->code, "\t.cfi_remember_state\n");
    if (emit->has_frame) {
        for (int reg = 14; reg <= 31; reg++) {
            if (emit->fpr_save_offsets[reg] >= 0) {
                ppc_emit_local_access(emit, "lfd", ppc_fpr_names[reg], emit->fpr_save_offsets[reg]);
            }
        }
        for (int reg = 30; reg >= 14; reg--) {
            if (emit->gpr_save_offsets[reg] >= 0) {
                ppc_emit_local_access(emit, ppc_load_word_op(emit), ppc_gpr_names[reg], emit->gpr_save_offsets[reg]);
            }
        }

        anvil_strbuf_appendf(&emit->code, "\t%s %%r0, %u(%%r31)\n", ppc_load_word_op(emit), emit->desc->lr_save_offset);
        anvil_strbuf_append(&emit->code, "\tmtlr %r0\n\t.cfi_restore 65\n");
        /* r31 holds the caller's SP: reload r31 while the frame is still
         * allocated, then release it (also after dynamic allocation). */
        anvil_strbuf_append(&emit->code, "\tmr %r12, %r31\n\t.cfi_def_cfa_register 12\n");
        ppc_emit_mem_access(emit, ppc_load_word_op(emit), "%r31", "%r12", -(int64_t)emit->gpr_save_offsets[31]);
        anvil_strbuf_append(&emit->code, "\t.cfi_restore 31\n\tmr %r1, %r12\n\t.cfi_def_cfa_register 1\n");
    }
    anvil_strbuf_append(&emit->code, "\tblr\n");
    if (emit->has_frame)
        anvil_strbuf_append(&emit->code, "\t.cfi_restore_state\n");
}

static bool ppc_get_uses2(const anvil_mir_func_t *mir, size_t instr_index, anvil_mir_vreg_t *lhs, anvil_mir_vreg_t *rhs)
{
    *lhs = anvil_mir_get_instr_use(mir, instr_index, 0);
    *rhs = anvil_mir_get_instr_use(mir, instr_index, 1);
    return *lhs != ANVIL_MIR_NO_VREG && *rhs != ANVIL_MIR_NO_VREG;
}

static void ppc_emit_copy(ppc_mir_emit_t *emit, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = ppc_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = ppc_vreg_info_checked(emit, src);
    const anvil_regalloc_assignment_t *dst_assignment = ppc_assignment_checked(emit, dst);
    const anvil_regalloc_assignment_t *src_assignment = ppc_assignment_checked(emit, src);
    if (!dst_info || !src_info || !dst_assignment || !src_assignment)
        return;

    const char *dst_reg = ppc_reg_name(emit, dst);
    const char *src_reg = ppc_reg_name(emit, src);
    if (emit->failed)
        return;
    if (dst_assignment->phys_reg == src_assignment->phys_reg && dst_info->reg_class == src_info->reg_class) {
        return;
    }

    if (dst_info->reg_class == ANVIL_MIR_REG_GPR && src_info->reg_class == ANVIL_MIR_REG_GPR) {
        anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", dst_reg, src_reg);
        return;
    }
    if (dst_info->reg_class == ANVIL_MIR_REG_FPR && src_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tfmr %s, %s\n", dst_reg, src_reg);
        return;
    }
    if (dst_info->reg_class == ANVIL_MIR_REG_VECTOR || src_info->reg_class == ANVIL_MIR_REG_VECTOR) {
        if (dst_info->reg_class != src_info->reg_class) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\txxlor %s, %s, %s\n", dst_reg, src_reg, src_reg);
        return;
    }

    if (!emit->has_frame || emit->fp_const_scratch_offset < 0) {
        emit->failed = true;
        return;
    }

    if (src_info->reg_class == ANVIL_MIR_REG_GPR && dst_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = ppc_mir_size_bytes(dst_info->size_bits);
        ppc_emit_local_access(emit, size <= 4 ? "stw" : ppc_store_word_op(emit), src_reg, emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit, size <= 4 ? "lfs" : "lfd", dst_reg, emit->fp_const_scratch_offset);
        return;
    }

    if (src_info->reg_class == ANVIL_MIR_REG_FPR && dst_info->reg_class == ANVIL_MIR_REG_GPR) {
        int size = ppc_mir_size_bytes(src_info->size_bits);
        ppc_emit_local_access(emit, size <= 4 ? "stfs" : "stfd", src_reg, emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit, size <= 4 ? "lwz" : ppc_load_word_op(emit), dst_reg, emit->fp_const_scratch_offset);
        return;
    }

    emit->failed = true;
}

/* Immediate operand forms (D-form arithmetic, logical and compare
 * instructions, rotate-and-mask shifts). */
static bool ppc_operand_wide(const ppc_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->mir, vreg);
    return ppc_is_64(emit) && info && info->size_bits > 32;
}

static int64_t ppc_imm_in_width(int64_t value, bool wide)
{
    return wide ? value : (int64_t)(int32_t)value;
}

static bool ppc_simm16(int64_t value)
{
    return value >= -32768 && value <= 32767;
}

static bool ppc_uimm16(int64_t value)
{
    return value >= 0 && value <= 65535;
}

static int ppc_pow2_shift(int64_t value, bool wide)
{
    if (value <= 0 || (value & (value - 1)) != 0)
        return -1;
    int shift = 0;
    while ((INT64_C(1) << shift) != value)
        shift++;
    return shift < (wide ? 64 : 32) ? shift : -1;
}

/* Mirrors the forms chosen by ppc_emit_binary and ppc_emit_cmp. */
static bool ppc_takes_immediate(void *ctx, size_t index, const anvil_mir_instr_info_t *info, size_t use, int64_t value)
{
    const ppc_mir_emit_t *emit = ctx;
    const anvil_mir_vreg_info_t *def = info->def != ANVIL_MIR_NO_VREG ? anvil_mir_get_vreg_info(emit->mir, info->def) : NULL;
    if (!def || def->reg_class != ANVIL_MIR_REG_GPR || info->num_uses != 2 || use != 1)
        return false;
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, index, 0);
    bool wide = ppc_operand_wide(emit, lhs);
    int64_t v = ppc_imm_in_width(value, wide);
    switch (info->op) {
    case ANVIL_MIR_OP_ADD:
        return ppc_simm16(v);
    case ANVIL_MIR_OP_SUB:
        return v != INT64_MIN && ppc_simm16(-v);
    case ANVIL_MIR_OP_MUL:
        return ppc_pow2_shift(v, wide) >= 0 || ppc_simm16(v);
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR:
        return ppc_uimm16(v);
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
        return true;
    case ANVIL_MIR_OP_SCALAR:
        return info->ir_op == ANVIL_OP_ROTL;
    default: {
        const anvil_mir_vreg_info_t *lhs_info = anvil_mir_get_vreg_info(emit->mir, lhs);
        if (!anvil_mir_isel_is_compare(info->op) || !lhs_info || lhs_info->reg_class != ANVIL_MIR_REG_GPR)
            return false;
        bool unsigned_cmp = info->op == ANVIL_MIR_OP_CMP_ULT || info->op == ANVIL_MIR_OP_CMP_ULE || info->op == ANVIL_MIR_OP_CMP_UGT ||
                            info->op == ANVIL_MIR_OP_CMP_UGE;
        return unsigned_cmp ? ppc_uimm16(v) : ppc_simm16(v);
    }
    }
}

static bool ppc_rhs_immediate(const ppc_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info, int64_t *out)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, index, 1);
    int64_t value;
    if (!anvil_mir_isel_constant(&emit->isel, rhs, &value) || !ppc_takes_immediate((void *)emit, index, info, 1, value))
        return false;
    *out = ppc_imm_in_width(value, ppc_operand_wide(emit, lhs));
    return true;
}

static void ppc_emit_binary(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs;
    anvil_mir_vreg_t rhs;
    if (!ppc_get_uses2(emit->mir, instr_index, &lhs, &rhs)) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    if (!def_info)
        return;

    const char *dst = ppc_reg_name(emit, info->def);
    const char *a = ppc_reg_name(emit, lhs);
    const char *b = ppc_reg_name(emit, rhs);
    if (emit->failed)
        return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *op = NULL;
        bool single = def_info->size_bits == 32;
        switch (info->op) {
        case ANVIL_MIR_OP_ADD:
            op = single ? "fadds" : "fadd";
            break;
        case ANVIL_MIR_OP_SUB:
            op = single ? "fsubs" : "fsub";
            break;
        case ANVIL_MIR_OP_MUL:
            op = single ? "fmuls" : "fmul";
            break;
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_FDIV:
            op = single ? "fdivs" : "fdiv";
            break;
        default:
            break;
        }
        if (!op) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", op, dst, a, b);
        return;
    }

    bool wide = ppc_is_64(emit) && def_info->size_bits > 32;
    int64_t imm;
    const anvil_regalloc_assignment_t *base = ppc_assignment_checked(emit, lhs);
    if (base && ppc_rhs_immediate(emit, instr_index, info, &imm)) {
        bool operand_wide = ppc_operand_wide(emit, lhs);
        int bits = operand_wide ? 64 : 32;
        switch (info->op) {
        case ANVIL_MIR_OP_ADD:
        case ANVIL_MIR_OP_SUB:
            /* addi reads register 0 as the literal zero. */
            if (base->phys_reg == 0)
                break;
            anvil_strbuf_appendf(&emit->code, "\taddi %s, %s, %lld\n", dst, a, (long long)(info->op == ANVIL_MIR_OP_ADD ? imm : -imm));
            return;
        case ANVIL_MIR_OP_MUL: {
            int shift = ppc_pow2_shift(imm, operand_wide);
            if (shift >= 0)
                anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %d\n", operand_wide ? "sldi" : "slwi", dst, a, shift);
            else
                anvil_strbuf_appendf(&emit->code, "\tmulli %s, %s, %lld\n", dst, a, (long long)imm);
            return;
        }
        case ANVIL_MIR_OP_AND:
            /* andi. also records CR0, which carries nothing here. */
            anvil_strbuf_appendf(&emit->code, "\tandi. %s, %s, %lld\n", dst, a, (long long)imm);
            return;
        case ANVIL_MIR_OP_OR:
            anvil_strbuf_appendf(&emit->code, "\tori %s, %s, %lld\n", dst, a, (long long)imm);
            return;
        case ANVIL_MIR_OP_XOR:
            anvil_strbuf_appendf(&emit->code, "\txori %s, %s, %lld\n", dst, a, (long long)imm);
            return;
        case ANVIL_MIR_OP_SHL:
        case ANVIL_MIR_OP_SHR:
        case ANVIL_MIR_OP_SAR: {
            int count = (int)(imm & (bits - 1));
            const char *op = info->op == ANVIL_MIR_OP_SHL ? (operand_wide ? "sldi" : "slwi")
                           : info->op == ANVIL_MIR_OP_SHR ? (operand_wide ? "srdi" : "srwi") : (operand_wide ? "sradi" : "srawi");
            anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %d\n", op, dst, a, count);
            return;
        }
        default:
            break;
        }
    }
    switch (info->op) {
    case ANVIL_MIR_OP_ADD:
        anvil_strbuf_appendf(&emit->code, "\tadd %s, %s, %s\n", dst, a, b);
        break;
    case ANVIL_MIR_OP_SUB:
        anvil_strbuf_appendf(&emit->code, "\tsubf %s, %s, %s\n", dst, b, a);
        break;
    case ANVIL_MIR_OP_MUL:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "mulld" : "mullw", dst, a, b);
        break;
    case ANVIL_MIR_OP_DIV:
    case ANVIL_MIR_OP_SDIV:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "divd" : "divw", dst, a, b);
        break;
    case ANVIL_MIR_OP_UDIV:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "divdu" : "divwu", dst, a, b);
        break;
    case ANVIL_MIR_OP_SMOD:
        /* r0: the operands may sit in the spill scratch registers. */
        anvil_strbuf_appendf(&emit->code, "\t%s %%r0, %s, %s\n", wide ? "divd" : "divw", a, b);
        anvil_strbuf_appendf(&emit->code, "\t%s %%r0, %%r0, %s\n", wide ? "mulld" : "mullw", b);
        anvil_strbuf_appendf(&emit->code, "\tsubf %s, %%r0, %s\n", dst, a);
        break;
    case ANVIL_MIR_OP_UMOD:
        /* r0: the operands may sit in the spill scratch registers. */
        anvil_strbuf_appendf(&emit->code, "\t%s %%r0, %s, %s\n", wide ? "divdu" : "divwu", a, b);
        anvil_strbuf_appendf(&emit->code, "\t%s %%r0, %%r0, %s\n", wide ? "mulld" : "mullw", b);
        anvil_strbuf_appendf(&emit->code, "\tsubf %s, %%r0, %s\n", dst, a);
        break;
    case ANVIL_MIR_OP_AND:
        anvil_strbuf_appendf(&emit->code, "\tand %s, %s, %s\n", dst, a, b);
        break;
    case ANVIL_MIR_OP_OR:
        anvil_strbuf_appendf(&emit->code, "\tor %s, %s, %s\n", dst, a, b);
        break;
    case ANVIL_MIR_OP_XOR:
        anvil_strbuf_appendf(&emit->code, "\txor %s, %s, %s\n", dst, a, b);
        break;
    case ANVIL_MIR_OP_SHL:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "sld" : "slw", dst, a, b);
        break;
    case ANVIL_MIR_OP_SHR:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "srd" : "srw", dst, a, b);
        break;
    case ANVIL_MIR_OP_SAR:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "srad" : "sraw", dst, a, b);
        break;
    default:
        emit->failed = true;
        break;
    }
}

static const char *ppc_cmp_branch(anvil_mir_opcode_t op)
{
    switch (op) {
    case ANVIL_MIR_OP_CMP_EQ:
        return "beq";
    case ANVIL_MIR_OP_CMP_NE:
    case ANVIL_MIR_OP_CMP:
        return "bne";
    case ANVIL_MIR_OP_CMP_LT:
    case ANVIL_MIR_OP_CMP_ULT:
        return "blt";
    case ANVIL_MIR_OP_CMP_LE:
    case ANVIL_MIR_OP_CMP_ULE:
        return "ble";
    case ANVIL_MIR_OP_CMP_GT:
    case ANVIL_MIR_OP_CMP_UGT:
        return "bgt";
    case ANVIL_MIR_OP_CMP_GE:
    case ANVIL_MIR_OP_CMP_UGE:
        return "bge";
    default:
        return "bne";
    }
}

/* The CR0 bit a compare's condition reads (LT 0, GT 1, EQ 2), and whether
 * the condition is that bit clear. */
static void ppc_cmp_select_bit(anvil_mir_opcode_t op, int *bit, bool *negated)
{
    switch (op) {
    case ANVIL_MIR_OP_CMP_LT: case ANVIL_MIR_OP_CMP_ULT: *bit = 0; *negated = false; break;
    case ANVIL_MIR_OP_CMP_GE: case ANVIL_MIR_OP_CMP_UGE: *bit = 0; *negated = true; break;
    case ANVIL_MIR_OP_CMP_GT: case ANVIL_MIR_OP_CMP_UGT: *bit = 1; *negated = false; break;
    case ANVIL_MIR_OP_CMP_LE: case ANVIL_MIR_OP_CMP_ULE: *bit = 1; *negated = true; break;
    case ANVIL_MIR_OP_CMP_EQ: *bit = 2; *negated = false; break;
    default: *bit = 2; *negated = true; break;
    }
}

static bool ppc_cmp_is_unsigned(anvil_mir_opcode_t op)
{
    return op == ANVIL_MIR_OP_CMP_ULT || op == ANVIL_MIR_OP_CMP_ULE || op == ANVIL_MIR_OP_CMP_UGT || op == ANVIL_MIR_OP_CMP_UGE;
}

static void ppc_emit_cmp(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs;
    anvil_mir_vreg_t rhs;
    if (!ppc_get_uses2(emit->mir, instr_index, &lhs, &rhs)) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *lhs_info = ppc_vreg_info_checked(emit, lhs);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *a = ppc_reg_name(emit, lhs);
    const char *b = ppc_reg_name(emit, rhs);
    if (!lhs_info || emit->failed)
        return;

    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tfcmpu 0, %s, %s\n", a, b);
    } else {
        bool wide = ppc_is_64(emit) && lhs_info->size_bits > 32;
        bool unsigned_cmp = ppc_cmp_is_unsigned(info->op);
        const char *op = unsigned_cmp ? (wide ? "cmpld" : "cmplw") : (wide ? "cmpd" : "cmpw");
        int64_t imm;
        if (ppc_rhs_immediate(emit, instr_index, info, &imm))
            anvil_strbuf_appendf(&emit->code, "\t%si 0, %s, %lld\n", op, a, (long long)imm);
        else
            anvil_strbuf_appendf(&emit->code, "\t%s 0, %s, %s\n", op, a, b);
        if (anvil_mir_isel_feeds_next_branch(&emit->isel, instr_index, info)) {
            emit->fused_branch = ppc_cmp_branch(info->op);
            return;
        }
        /* A select on the flags: isel, or a branch on the CR bit. */
        if (anvil_mir_isel_feeds_next_select(&emit->isel, instr_index, info)) {
            ppc_cmp_select_bit(info->op, &emit->fused_select_bit, &emit->fused_select_negated);
            return;
        }
    }

    size_t id = emit->label_counter++;
    anvil_strbuf_appendf(&emit->code, "\tli %s, 0\n", dst);
    if (info->op == ANVIL_MIR_OP_FCMP) {
        unsigned mask = 0;
        switch ((anvil_fcmp_pred_t)info->imm) {
        case ANVIL_FCMP_FALSE:
            mask = 0;
            break;
        case ANVIL_FCMP_OEQ:
            mask = 4;
            break;
        case ANVIL_FCMP_OGT:
            mask = 2;
            break;
        case ANVIL_FCMP_OGE:
            mask = 2 | 4;
            break;
        case ANVIL_FCMP_OLT:
            mask = 1;
            break;
        case ANVIL_FCMP_OLE:
            mask = 1 | 4;
            break;
        case ANVIL_FCMP_ONE:
            mask = 1 | 2;
            break;
        case ANVIL_FCMP_ORD:
            mask = 1 | 2 | 4;
            break;
        case ANVIL_FCMP_UEQ:
            mask = 4 | 8;
            break;
        case ANVIL_FCMP_UGT:
            mask = 2 | 8;
            break;
        case ANVIL_FCMP_UGE:
            mask = 2 | 4 | 8;
            break;
        case ANVIL_FCMP_ULT:
            mask = 1 | 8;
            break;
        case ANVIL_FCMP_ULE:
            mask = 1 | 4 | 8;
            break;
        case ANVIL_FCMP_UNE:
            mask = 1 | 2 | 8;
            break;
        case ANVIL_FCMP_UNO:
            mask = 8;
            break;
        case ANVIL_FCMP_TRUE:
            mask = 15;
            break;
        }
        const char *branches[] = {"blt", "bgt", "beq", "bun"};
        for (unsigned bit = 0; bit < 4; bit++)
            if (mask & (1u << bit))
                anvil_strbuf_appendf(&emit->code, "\t%s .L%s_cmp_true_%zu\n", branches[bit], anvil_mir_func_name(emit->mir), id);
    } else {
        anvil_strbuf_appendf(&emit->code, "\t%s .L%s_cmp_true_%zu\n", ppc_cmp_branch(info->op), anvil_mir_func_name(emit->mir), id);
    }
    anvil_strbuf_appendf(&emit->code, "\tb .L%s_cmp_done_%zu\n", anvil_mir_func_name(emit->mir), id);
    anvil_strbuf_appendf(&emit->code, ".L%s_cmp_true_%zu:\n", anvil_mir_func_name(emit->mir), id);
    anvil_strbuf_appendf(&emit->code, "\tli %s, 1\n", dst);
    anvil_strbuf_appendf(&emit->code, ".L%s_cmp_done_%zu:\n", anvil_mir_func_name(emit->mir), id);
}

static const char *ppc_inverse_branch(const char *branch)
{
    static const char *const pairs[][2] = {{"beq", "bne"}, {"blt", "bge"}, {"ble", "bgt"}};
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        if (strcmp(branch, pairs[i][0]) == 0)
            return pairs[i][1];
        if (strcmp(branch, pairs[i][1]) == 0)
            return pairs[i][0];
    }
    return NULL;
}

/* Conditional branch on CR0 (`taken`, e.g. "blt"); a target that is the next
 * block falls through. */
static void ppc_emit_cond_branch(ppc_mir_emit_t *emit, const char *taken, anvil_mir_block_t true_block, anvil_mir_block_t false_block)
{
    const char *inverse = ppc_inverse_branch(taken);
    if (true_block == emit->block + 1 && inverse) {
        anvil_strbuf_appendf(&emit->code, "\t%s ", inverse);
        emit->failed |= !ppc_emit_branch_target(emit, false_block);
        anvil_strbuf_append(&emit->code, "\n");
        return;
    }
    anvil_strbuf_appendf(&emit->code, "\t%s ", taken);
    emit->failed |= !ppc_emit_branch_target(emit, true_block);
    anvil_strbuf_append(&emit->code, "\n");
    if (false_block != emit->block + 1) {
        anvil_strbuf_append(&emit->code, "\tb ");
        emit->failed |= !ppc_emit_branch_target(emit, false_block);
        anvil_strbuf_append(&emit->code, "\n");
    }
}

static void ppc_emit_unary(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *s = ppc_reg_name(emit, src);
    if (!def_info || emit->failed)
        return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        switch (info->op) {
        case ANVIL_MIR_OP_NEG:
            anvil_strbuf_appendf(&emit->code, "\tfneg %s, %s\n", dst, s);
            break;
        case ANVIL_MIR_OP_FABS:
            anvil_strbuf_appendf(&emit->code, "\tfabs %s, %s\n", dst, s);
            break;
        default:
            emit->failed = true;
            break;
        }
        return;
    }

    switch (info->op) {
    case ANVIL_MIR_OP_NEG:
        anvil_strbuf_appendf(&emit->code, "\tneg %s, %s\n", dst, s);
        break;
    case ANVIL_MIR_OP_NOT:
        anvil_strbuf_appendf(&emit->code, "\tnor %s, %s, %s\n", dst, s, s);
        break;
    default:
        emit->failed = true;
        break;
    }
}

static void ppc_emit_frame_addr(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!emit->has_frame || info->frame_slot < 0 || (size_t)info->frame_slot >= emit->num_frame_slot_offsets) {
        emit->failed = true;
        return;
    }

    const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, info->def);
    if (!assignment)
        return;
    ppc_emit_addi_large(emit, assignment->phys_reg, 31, -emit->frame_slot_offsets[info->frame_slot]);
}

static void ppc_emit_dyn_alloca(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t count = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (count == ANVIL_MIR_NO_VREG || !info->has_imm || info->imm <= 0) {
        emit->failed = true;
        return;
    }

    const anvil_regalloc_assignment_t *dst_assignment = ppc_assignment_checked(emit, info->def);
    const char *count_reg = ppc_reg_name(emit, count);
    if (!dst_assignment || emit->failed)
        return;

    if (info->imm == 1) {
        anvil_strbuf_appendf(&emit->code, "\tmr %%r11, %s\n", count_reg);
    } else if (info->imm >= -32768 && info->imm <= 32767) {
        anvil_strbuf_appendf(&emit->code, "\tmulli %%r11, %s, %lld\n", count_reg, (long long)info->imm);
    } else {
        ppc_emit_load_imm(emit, 11, info->imm);
        anvil_strbuf_appendf(&emit->code, "\t%s %%r11, %%r11, %s\n", ppc_is_64(emit) ? "mulld" : "mullw", count_reg);
    }
    /* The dynamic frame must expose a complete ABI linkage/outgoing area to
       nested calls.  Keep user payload beyond that aligned prefix. */
    int prefix = (int)emit->desc->outgoing_arg_offset + emit->outgoing_size;
    if (prefix < (int)emit->desc->min_frame_size) {
        prefix = (int)emit->desc->min_frame_size;
    }
    prefix = align_int(prefix, 16);
    if (prefix <= 32752) {
        anvil_strbuf_appendf(&emit->code, "\taddi %%r11, %%r11, %d\n", prefix + 15);
    } else {
        ppc_emit_load_imm(emit, 0, prefix + 15);
        anvil_strbuf_append(&emit->code, "\tadd %r11, %r11, %r0\n");
    }
    anvil_strbuf_append(&emit->code, ppc_is_64(emit) ? "\tclrrdi %r11, %r11, 4\n" : "\trlwinm %r11, %r11, 0, 0, 27\n");
    anvil_strbuf_append(&emit->code, "\tneg %r11, %r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s %%r1, %%r1, %%r11\n", ppc_is_64(emit) ? "stdux" : "stwux");
    ppc_emit_addi_large(emit, dst_assignment->phys_reg, 1, prefix);
}

static void ppc_emit_mov_fpr_imm(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info, const anvil_mir_vreg_info_t *def_info, const char *dst)
{
    if (!emit->has_frame || emit->fp_const_scratch_offset < 0) {
        emit->failed = true;
        return;
    }

    uint64_t raw = (uint64_t)info->imm;
    int size = ppc_mir_size_bytes(def_info->size_bits);
    if (size <= 4) {
        ppc_emit_load_imm(emit, 11, (int32_t)(raw & 0xffffffffu));
        ppc_emit_local_access(emit, "stw", "%r11", emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit, "lfs", dst, emit->fp_const_scratch_offset);
        return;
    }

    if (ppc_is_64(emit)) {
        ppc_emit_load_imm(emit, 11, (int64_t)raw);
        ppc_emit_local_access(emit, "std", "%r11", emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit, "lfd", dst, emit->fp_const_scratch_offset);
        return;
    }

    uint32_t hi = (uint32_t)(raw >> 32);
    uint32_t lo = (uint32_t)(raw & 0xffffffffu);
    ppc_emit_load_imm(emit, 11, (int32_t)hi);
    ppc_emit_local_access(emit, "stw", "%r11", emit->fp_const_scratch_offset);
    ppc_emit_load_imm(emit, 11, (int32_t)lo);
    ppc_emit_local_access(emit, "stw", "%r11", emit->fp_const_scratch_offset - 4);
    ppc_emit_local_access(emit, "lfd", dst, emit->fp_const_scratch_offset);
}

static void ppc_emit_mov(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    if (!def_info || !assignment || emit->failed)
        return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        ppc_emit_mov_fpr_imm(emit, info, def_info, dst);
        return;
    }
    if (!anvil_mir_isel_materialize(&emit->isel, info->def))
        return;

    ppc_emit_load_imm(emit, assignment->phys_reg, info->imm);
}

/* A vector register as `mnemonic` names it: VSX instructions ('x...')
 * address all 64 VSX registers, AltiVec ones ('v...') the AltiVec file. */
static const char *ppc_vector_operand(const char *mnemonic, int vr)
{
    return mnemonic[0] == 'x' ? ppc_vector_vsr(vr) : ppc_vr_names[vr];
}

/* Broadcast of a scalar.  An FPR holds doubleword 0 of its VSR in double
 * format: f64 lanes take that doubleword twice, f32 lanes convert it to
 * single (words 0 and 2) and splat word 0.  A GPR moves into v0 (word 1 or
 * doubleword 0, big-endian element numbering on both endiannesses) and
 * splats from there. */
static void ppc_emit_vector_splat(ppc_mir_emit_t *emit, const anvil_vector_isa_t *isa, const anvil_mir_instr_info_t *info,
                                  anvil_mir_vreg_t scalar)
{
    const anvil_regalloc_assignment_t *d = ppc_assignment_checked(emit, info->def);
    const anvil_regalloc_assignment_t *s = ppc_assignment_checked(emit, scalar);
    const anvil_mir_vreg_info_t *source = ppc_vreg_info_checked(emit, scalar);
    if (!d || !s || !source)
        return;
    const char *dst = ppc_vector_operand(isa->mnemonic, d->phys_reg);
    if (source->reg_class == ANVIL_MIR_REG_FPR) {
        const char *from = ppc_vsr_names[s->phys_reg];
        if (info->imm == 64) {
            anvil_strbuf_appendf(&emit->code, "\txxpermdi %s, %s, %s, 0\n", dst, from, from);
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\txvcvdpsp %s, %s\n", dst, from);
        anvil_strbuf_appendf(&emit->code, "\txxspltw %s, %s, 0\n", dst, dst);
        return;
    }
    const char *temp = ppc_vector_operand(isa->mnemonic, PPC_VECTOR_TEMP);
    const char *gpr = ppc_gpr_names[s->phys_reg];
    anvil_strbuf_appendf(&emit->code, "\t%s %s, %s\n", info->imm == 64 ? "mtvsrd" : "mtvsrwz", ppc_vector_vsr(PPC_VECTOR_TEMP), gpr);
    switch (info->imm) {
    case 64:
        anvil_strbuf_appendf(&emit->code, "\txxpermdi %s, %s, %s, 0\n", dst, temp, temp);
        break;
    case 32:
        anvil_strbuf_appendf(&emit->code, "\txxspltw %s, %s, 1\n", dst, temp);
        break;
    default:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %d\n", isa->mnemonic, dst, temp, info->imm == 16 ? 3 : 7);
        break;
    }
}

/* dst = op(a, b) on vector registers, named as `op` addresses them. */
static void ppc_vector_three(ppc_mir_emit_t *emit, const char *op, int dst, int a, int b)
{
    anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", op, ppc_vector_operand(op, dst), ppc_vector_operand(op, a),
                         ppc_vector_operand(op, b));
}

static void ppc_vector_not(ppc_mir_emit_t *emit, int reg)
{
    ppc_vector_three(emit, "xxlnor", reg, reg, reg);
}

/* Integer compares: equality and greater-than only, swapped for <, and
 * complemented for != (of ==), >= (of <) and <= (of >). */
static void ppc_vector_int_compare(ppc_mir_emit_t *emit, const anvil_vector_isa_t *isa, anvil_op_t op, int dst, int a, int b)
{
    bool swap = op == ANVIL_OP_CMP_LT || op == ANVIL_OP_CMP_ULT || op == ANVIL_OP_CMP_GE || op == ANVIL_OP_CMP_UGE;
    ppc_vector_three(emit, isa->mnemonic, dst, swap ? b : a, swap ? a : b);
    if (op == ANVIL_OP_CMP_NE || op == ANVIL_OP_CMP_GE || op == ANVIL_OP_CMP_LE || op == ANVIL_OP_CMP_UGE || op == ANVIL_OP_CMP_ULE)
        ppc_vector_not(emit, dst);
}

/* FP compares from the ordered xvcmpeq/xvcmpge/xvcmpgt (false on NaN), as
 * on every IEEE vector unit: an unordered predicate complements the
 * opposite ordered one; ord is a >= b or b > a, one is a > b or b > a. */
static void ppc_vector_fcmp(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info, int dst, int a, int b)
{
    const char *sfx = info->imm == 32 ? "sp" : "dp";
    char eq[16], ge[16], gt[16];
    snprintf(eq, sizeof(eq), "xvcmpeq%s", sfx);
    snprintf(ge, sizeof(ge), "xvcmpge%s", sfx);
    snprintf(gt, sizeof(gt), "xvcmpgt%s", sfx);
    switch (info->fcmp_pred) {
    case ANVIL_FCMP_OEQ: ppc_vector_three(emit, eq, dst, a, b); return;
    case ANVIL_FCMP_OGT: ppc_vector_three(emit, gt, dst, a, b); return;
    case ANVIL_FCMP_OGE: ppc_vector_three(emit, ge, dst, a, b); return;
    case ANVIL_FCMP_OLT: ppc_vector_three(emit, gt, dst, b, a); return;
    case ANVIL_FCMP_OLE: ppc_vector_three(emit, ge, dst, b, a); return;
    case ANVIL_FCMP_UNE: ppc_vector_three(emit, eq, dst, a, b); ppc_vector_not(emit, dst); return;
    case ANVIL_FCMP_ULE: ppc_vector_three(emit, gt, dst, a, b); ppc_vector_not(emit, dst); return;
    case ANVIL_FCMP_ULT: ppc_vector_three(emit, ge, dst, a, b); ppc_vector_not(emit, dst); return;
    case ANVIL_FCMP_UGE: ppc_vector_three(emit, gt, dst, b, a); ppc_vector_not(emit, dst); return;
    case ANVIL_FCMP_UGT: ppc_vector_three(emit, ge, dst, b, a); ppc_vector_not(emit, dst); return;
    case ANVIL_FCMP_ORD:
    case ANVIL_FCMP_UNO:
    case ANVIL_FCMP_ONE:
    case ANVIL_FCMP_UEQ: {
        bool ordered = info->fcmp_pred == ANVIL_FCMP_ORD || info->fcmp_pred == ANVIL_FCMP_UNO;
        ppc_vector_three(emit, ordered ? ge : gt, PPC_VECTOR_TEMP, a, b);
        ppc_vector_three(emit, gt, dst, b, a);
        ppc_vector_three(emit, "xxlor", dst, dst, PPC_VECTOR_TEMP);
        if (info->fcmp_pred == ANVIL_FCMP_UNO || info->fcmp_pred == ANVIL_FCMP_UEQ)
            ppc_vector_not(emit, dst);
        return;
    }
    case ANVIL_FCMP_FALSE:
    case ANVIL_FCMP_TRUE:
        ppc_vector_three(emit, info->fcmp_pred == ANVIL_FCMP_TRUE ? "vcmpequb" : "xxlxor", dst, dst, dst);
        return;
    }
    emit->failed = true;
}

/* A reduction: the whole vector rotated and combined with the table's lane
 * operation (doublewords swapped, then rotations by 4, 2 and 1 bytes down
 * to the lane width) in v0/v1, so every lane holds the result; the first
 * doubleword then moves to the result register.  A lane mask's OR needs
 * only the doubleword fold: x | -x has the sign bit exactly when x != 0. */
static void ppc_emit_vector_reduce(ppc_mir_emit_t *emit, size_t instr_index, const anvil_vector_isa_t *isa, const anvil_mir_instr_info_t *info,
                                   anvil_mir_vreg_t source)
{
    const anvil_regalloc_assignment_t *vector = ppc_assignment_checked(emit, source);
    const anvil_regalloc_assignment_t *result = ppc_assignment_checked(emit, info->def);
    const anvil_mir_vreg_info_t *result_info = ppc_vreg_info_checked(emit, info->def);
    if (!vector || !result || !result_info)
        return;
    const char *m = isa->mnemonic;
    int t = PPC_VECTOR_TEMP, t2 = PPC_VECTOR_TEMP2, v = vector->phys_reg;
    anvil_strbuf_appendf(&emit->code, "\txxpermdi %s, %s, %s, 2\n", ppc_vector_vsr(t), ppc_vector_vsr(v), ppc_vector_vsr(v));
    ppc_vector_three(emit, m, t, t, v);
    bool mask = info->ir_op == ANVIL_OP_REDUCE_OR && anvil_vector_mir_is_mask(emit->mir, instr_index, source);
    for (int64_t bytes = 4; !mask && bytes >= info->imm / 8; bytes /= 2) {
        anvil_strbuf_appendf(&emit->code, "\tvsldoi %s, %s, %s, %lld\n", ppc_vr_names[t2], ppc_vr_names[t], ppc_vr_names[t], (long long)bytes);
        ppc_vector_three(emit, m, t, t, t2);
    }
    if (result_info->reg_class == ANVIL_MIR_REG_FPR) {
        /* An FPR is the first doubleword, in double format. */
        if (info->imm == 64)
            anvil_strbuf_appendf(&emit->code, "\txxlor %s, %s, %s\n", ppc_vsr_names[result->phys_reg], ppc_vector_vsr(t), ppc_vector_vsr(t));
        else
            anvil_strbuf_appendf(&emit->code, "\txscvspdpn %s, %s\n", ppc_vsr_names[result->phys_reg], ppc_vector_vsr(t));
        return;
    }
    const char *dst = ppc_gpr_names[result->phys_reg];
    anvil_strbuf_appendf(&emit->code, "\tmfvsrd %s, %s\n", dst, ppc_vector_vsr(t));
    if (mask)
        anvil_strbuf_appendf(&emit->code, "\tneg %%r0, %s\n\tor %%r0, %%r0, %s\n\tsradi %s, %%r0, 63\n", dst, dst, dst);
}

/* A scalar operation the table prices: fmadd(s) d, a, b, c; fsqrt(s) d, a. */
static void ppc_emit_scalar(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    const anvil_vector_isa_t *isa =
        anvil_vector_isa_for_mir(ppc_vector_isa, ppc_vector_isa_count(emit->desc), emit->mir, instr_index, info, anvil_mir_func_cpu_features(emit->mir));
    const char *names[4] = {NULL};
    for (size_t operand = 0; isa && operand <= info->num_uses && operand < 4; operand++) {
        anvil_mir_vreg_t vreg = operand ? anvil_mir_get_instr_use(emit->mir, instr_index, operand - 1) : info->def;
        names[operand] = vreg == ANVIL_MIR_NO_VREG ? NULL : ppc_reg_name(emit, vreg);
    }
    int64_t count;
    if (isa && info->ir_op == ANVIL_OP_ROTL && names[0] && names[1] &&
        anvil_mir_isel_constant(&emit->isel, anvil_mir_get_instr_use(emit->mir, instr_index, 1), &count)) {
        /* rotlwi/rotldi: the table's mnemonic with an immediate count. */
        anvil_strbuf_appendf(&emit->code, "\t%si %s, %s, %d\n", isa->mnemonic, names[0], names[1], (int)(count & (info->imm - 1)));
        return;
    }
    if (!isa || info->num_uses > 3 || !anvil_vector_isa_print(&emit->code, isa->mnemonic, names, info->num_uses + 1))
        emit->failed = true;
}

/* A lane-wise operation, as the VSX/AltiVec table names it for this CPU. */
static void ppc_emit_vector(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    const anvil_vector_isa_t *isa =
        anvil_vector_isa_for_mir(ppc_vector_isa, ppc_vector_isa_count(emit->desc), emit->mir, instr_index, info, anvil_mir_func_cpu_features(emit->mir));
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (!isa || lhs == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }
    if (info->ir_op == ANVIL_OP_SPLAT) {
        ppc_emit_vector_splat(emit, isa, info, lhs);
        return;
    }
    if (anvil_op_is_reduction(info->ir_op)) {
        ppc_emit_vector_reduce(emit, instr_index, isa, info, lhs);
        return;
    }
    anvil_mir_vreg_t rhs = info->num_uses > 1 ? anvil_mir_get_instr_use(emit->mir, instr_index, 1) : ANVIL_MIR_NO_VREG;
    const anvil_regalloc_assignment_t *d = ppc_assignment_checked(emit, info->def);
    const anvil_regalloc_assignment_t *a = ppc_assignment_checked(emit, lhs);
    const anvil_regalloc_assignment_t *b = rhs == ANVIL_MIR_NO_VREG ? NULL : ppc_assignment_checked(emit, rhs);
    const char *m = isa->mnemonic;
    if (d && a && info->num_uses == 1) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s\n", m, ppc_vector_operand(m, d->phys_reg), ppc_vector_operand(m, a->phys_reg));
        return;
    }
    if (!d || !a || !b) {
        emit->failed = true;
        return;
    }
    if (info->ir_op == ANVIL_OP_SELECT) {
        /* xxsel picks its second source where the mask is set. */
        anvil_mir_vreg_t other = anvil_mir_get_instr_use(emit->mir, instr_index, 2);
        const anvil_regalloc_assignment_t *c = other == ANVIL_MIR_NO_VREG ? NULL : ppc_assignment_checked(emit, other);
        if (!c) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\txxsel %s, %s, %s, %s\n", ppc_vector_vsr(d->phys_reg), ppc_vector_vsr(c->phys_reg),
                             ppc_vector_vsr(b->phys_reg), ppc_vector_vsr(a->phys_reg));
        return;
    }
    if (info->ir_op == ANVIL_OP_FCMP) {
        ppc_vector_fcmp(emit, info, d->phys_reg, a->phys_reg, b->phys_reg);
        return;
    }
    if (info->widening) {
        /* The group sums of b (added to zero), then into a. */
        ppc_vector_three(emit, "vxor", PPC_VECTOR_TEMP, PPC_VECTOR_TEMP, PPC_VECTOR_TEMP);
        ppc_vector_three(emit, m, PPC_VECTOR_TEMP, b->phys_reg, PPC_VECTOR_TEMP);
        ppc_vector_three(emit, "vadduwm", d->phys_reg, a->phys_reg, PPC_VECTOR_TEMP);
        return;
    }
    if (info->ir_op == ANVIL_OP_FMA) {
        /* xvmadda accumulates into its target: c goes there first, through
         * v0 when the target holds a or b. */
        anvil_mir_vreg_t addend = anvil_mir_get_instr_use(emit->mir, instr_index, 2);
        const anvil_regalloc_assignment_t *c = addend == ANVIL_MIR_NO_VREG ? NULL : ppc_assignment_checked(emit, addend);
        if (!c) {
            emit->failed = true;
            return;
        }
        int work = d->phys_reg == c->phys_reg || (d->phys_reg != a->phys_reg && d->phys_reg != b->phys_reg) ? d->phys_reg : PPC_VECTOR_TEMP;
        if (work != c->phys_reg)
            ppc_vector_three(emit, "xxlor", work, c->phys_reg, c->phys_reg);
        ppc_vector_three(emit, m, work, a->phys_reg, b->phys_reg);
        if (work != d->phys_reg)
            ppc_vector_three(emit, "xxlor", d->phys_reg, work, work);
        return;
    }
    if (info->ir_op >= ANVIL_OP_CMP_EQ && info->ir_op <= ANVIL_OP_CMP_UGE) {
        ppc_vector_int_compare(emit, isa, info->ir_op, d->phys_reg, a->phys_reg, b->phys_reg);
        return;
    }
    if (info->ir_op == ANVIL_OP_MUL && info->imm == 16) {
        /* vmladduhm multiplies and adds a third vector: zero. */
        anvil_strbuf_appendf(&emit->code, "\tvxor %s, %s, %s\n", ppc_vr_names[PPC_VECTOR_TEMP], ppc_vr_names[PPC_VECTOR_TEMP],
                             ppc_vr_names[PPC_VECTOR_TEMP]);
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s, %s\n", m, ppc_vr_names[d->phys_reg], ppc_vr_names[a->phys_reg],
                             ppc_vr_names[b->phys_reg], ppc_vr_names[PPC_VECTOR_TEMP]);
        return;
    }
    anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", m, ppc_vector_operand(m, d->phys_reg), ppc_vector_operand(m, a->phys_reg),
                         ppc_vector_operand(m, b->phys_reg));
}

static void ppc_emit_symbol_addr(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->symbol || !info->symbol[0]) {
        emit->failed = true;
        return;
    }

    const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, info->def);
    if (!assignment)
        return;
    emit->desc->abi_ops->emit_symbol_address(emit, ppc_gpr_names[assignment->phys_reg], info->symbol, info->has_imm ? info->imm : 0,
                                             info->symbol_got);
}

/* The X-form of a D-form access: `opx reg, base, index`. */
static void ppc_emit_indexed_access(ppc_mir_emit_t *emit, size_t instr_index, size_t index_use, const char *op, const char *reg,
                                    const char *base)
{
    anvil_mir_vreg_t index = anvil_mir_get_instr_use(emit->mir, instr_index, index_use);
    const char *index_name = index == ANVIL_MIR_NO_VREG ? NULL : ppc_reg_name(emit, index);
    if (!index_name || emit->failed) {
        emit->failed = true;
        return;
    }
    anvil_strbuf_appendf(&emit->code, "\t%sx %s, %s, %s\n", op, reg, base, index_name);
}

static void ppc_emit_load(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *base = ppc_reg_name(emit, ptr);
    if (!def_info || emit->failed)
        return;

    int size = ppc_mir_size_bytes(def_info->size_bits);
    const char *op = ppc_load_op(emit->desc, def_info->reg_class, size, def_info->is_signed);
    if (info->index_scale)
        ppc_emit_indexed_access(emit, instr_index, 1, op, dst, base);
    else
        ppc_emit_base_offset_access(emit, op, dst, base, info->has_imm ? info->imm : 0);
    ppc_emit_sign_extend_loaded_byte(emit, def_info, dst, size);
}

static void ppc_emit_store(ppc_mir_emit_t *emit, size_t instr_index)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (value == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *value_info = ppc_vreg_info_checked(emit, value);
    const char *src = ppc_reg_name(emit, value);
    const char *base = ppc_reg_name(emit, ptr);
    if (!value_info || emit->failed)
        return;

    anvil_mir_instr_info_t info;
    int64_t offset = 0;
    if (anvil_mir_get_instr_info(emit->mir, instr_index, &info) && info.has_imm) {
        offset = info.imm;
    }

    int size = ppc_mir_size_bytes(value_info->size_bits);
    if (info.index_scale)
        ppc_emit_indexed_access(emit, instr_index, 2, ppc_store_op(emit->desc, value_info->reg_class, size), src, base);
    else
        ppc_emit_base_offset_access(emit, ppc_store_op(emit->desc, value_info->reg_class, size), src, base, offset);
}

static void ppc_emit_incoming_stack_arg(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!emit->has_frame || !info->has_imm || info->imm < 0 || info->imm > INT32_MAX) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    if (!def_info || emit->failed)
        return;

    int size = ppc_mir_size_bytes(def_info->size_bits);
    int frame_offset = (int)emit->desc->incoming_arg_offset + (int)info->imm;
    ppc_emit_incoming_stack_access(emit, ppc_load_op(emit->desc, def_info->reg_class, size, def_info->is_signed), dst, frame_offset);
    ppc_emit_sign_extend_loaded_byte(emit, def_info, dst, size);
}

static void ppc_emit_gpr_extend(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info, bool sign_extend)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = ppc_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = ppc_vreg_info_checked(emit, src);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *s = ppc_reg_name(emit, src);
    if (!dst_info || !src_info || emit->failed)
        return;
    if (dst_info->reg_class != ANVIL_MIR_REG_GPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        emit->failed = true;
        return;
    }

    int src_size = ppc_mir_size_bytes(src_info->size_bits);
    int dst_size = ppc_mir_size_bytes(dst_info->size_bits);
    /* A narrow load of the same signedness already extended the value to
     * the whole register (ppc_emit_load: lbz+extsb, lha, lhz, lwa, lwz). */
    if (src_size < (int)emit->desc->word_size && src_info->is_signed == sign_extend && anvil_mir_isel_def_op(&emit->isel, src) == ANVIL_MIR_OP_LOAD) {
        if (strcmp(dst, s) != 0)
            anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", dst, s);
        return;
    }
    if (sign_extend) {
        if (src_size <= 1) {
            anvil_strbuf_appendf(&emit->code, "\textsb %s, %s\n", dst, s);
        } else if (src_size <= 2) {
            anvil_strbuf_appendf(&emit->code, "\textsh %s, %s\n", dst, s);
        } else if (ppc_is_64(emit) && dst_size > 4 && src_size <= 4) {
            anvil_strbuf_appendf(&emit->code, "\textsw %s, %s\n", dst, s);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", dst, s);
        }
        return;
    }

    if (src_size <= 1) {
        anvil_strbuf_appendf(&emit->code, "\trlwinm %s, %s, 0, 24, 31\n", dst, s);
    } else if (src_size <= 2) {
        anvil_strbuf_appendf(&emit->code, "\trlwinm %s, %s, 0, 16, 31\n", dst, s);
    } else if (ppc_is_64(emit) && dst_size > 4 && src_size <= 4) {
        anvil_strbuf_appendf(&emit->code, "\trldicl %s, %s, 0, 32\n", dst, s);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", dst, s);
    }
}

/* Local frame offset (for ppc_emit_local_access) of the word holding the
 * low-order or high-order half of the 8-byte scratch slot. */
static int ppc_scratch_word(const ppc_mir_emit_t *emit, bool low_order)
{
    bool at_higher_address = low_order != emit->desc->little_endian;
    return emit->fp_const_scratch_offset - (at_higher_address ? 4 : 0);
}

/* Extends a narrow integer into r0 so it can be converted as a whole. */
static void ppc_emit_extend_to_r0(ppc_mir_emit_t *emit, const char *source, uint16_t bits, bool is_unsigned, bool to_doubleword)
{
    if (bits <= 8)
        anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm %%r0, %s, 0, 24, 31\n" : "\textsb %%r0, %s\n", source);
    else if (bits <= 16)
        anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm %%r0, %s, 0, 16, 31\n" : "\textsh %%r0, %s\n", source);
    else if (bits <= 32 && to_doubleword)
        anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trldicl %%r0, %s, 0, 32\n" : "\textsw %%r0, %s\n", source);
    else
        anvil_strbuf_appendf(&emit->code, "\tmr %%r0, %s\n", source);
}

/* Inline integer <-> floating conversion through the scratch slot, f0 or
 * f13 and r0 (none of them allocatable to live values here). */
static void ppc_emit_native_conversion(ppc_mir_emit_t *emit, anvil_mir_opcode_t op, const char *dst, const char *source,
                                       const anvil_mir_vreg_info_t *dst_info, const anvil_mir_vreg_info_t *src_info)
{
    bool int64 = (emit->features & ANVIL_FEATURE_PPC_64BIT) != 0;
    bool fpcvt = (emit->features & ANVIL_FEATURE_PPC_FPCVT) != 0;
    int slot = emit->fp_const_scratch_offset;
    if (!emit->has_frame || slot < 0) {
        emit->failed = true;
        return;
    }

    if (op == ANVIL_MIR_OP_FPTOSI || op == ANVIL_MIR_OP_FPTOUI) {
        bool is_unsigned = op == ANVIL_MIR_OP_FPTOUI;
        bool wide = dst_info->size_bits > 32;
        const char *convert = is_unsigned ? (fpcvt ? (wide ? "fctiduz" : "fctiwuz") : "fctidz") : (wide ? "fctidz" : "fctiwz");
        anvil_strbuf_appendf(&emit->code, "\t%s %%f0, %s\n", convert, source);
        ppc_emit_local_access(emit, "stfd", "%f0", slot);
        if (wide) {
            ppc_emit_local_access(emit, ppc_load_word_op(emit), dst, slot);
            return;
        }
        ppc_emit_local_access(emit, "lwz", dst, ppc_scratch_word(emit, true));
        /* Keep the register canonical for its width, as loads do. */
        if (dst_info->size_bits <= 8)
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm %s, %s, 0, 24, 31\n" : "\textsb %s, %s\n", dst, dst);
        else if (dst_info->size_bits <= 16)
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm %s, %s, 0, 16, 31\n" : "\textsh %s, %s\n", dst, dst);
        else if (!is_unsigned && ppc_is_64(emit))
            anvil_strbuf_appendf(&emit->code, "\textsw %s, %s\n", dst, dst);
        return;
    }

    bool is_unsigned = op == ANVIL_MIR_OP_UITOFP;
    bool single = dst_info->size_bits <= 32;
    if (int64) {
        if (src_info->size_bits > 32) {
            ppc_emit_local_access(emit, ppc_store_word_op(emit), source, slot);
        } else {
            ppc_emit_extend_to_r0(emit, source, src_info->size_bits, is_unsigned, true);
            ppc_emit_local_access(emit, ppc_store_word_op(emit), "%r0", slot);
        }
        ppc_emit_local_access(emit, "lfd", "%f0", slot);
        /* A narrow integer always fits fcfid's signed doubleword. */
        bool unsigned_form = is_unsigned && src_info->size_bits > 32;
        const char *convert = single && fpcvt ? (unsigned_form ? "fcfidus" : "fcfids") : (unsigned_form ? "fcfidu" : "fcfid");
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %%f0\n", convert, dst);
        if (single && !fpcvt)
            anvil_strbuf_appendf(&emit->code, "\tfrsp %s, %s\n", dst, dst);
        return;
    }

    /* 32-bit implementations: the double 2^52 + x, built from words, minus
     * 2^52 (biased by 2^31 for signed values) is exactly x. */
    const char *temp = strcmp(dst, "%f0") == 0 ? "%f13" : "%f0";
    ppc_emit_extend_to_r0(emit, source, src_info->size_bits, is_unsigned, false);
    if (!is_unsigned)
        anvil_strbuf_append(&emit->code, "\txoris %r0, %r0, 0x8000\n");
    ppc_emit_local_access(emit, "stw", "%r0", ppc_scratch_word(emit, true));
    anvil_strbuf_append(&emit->code, "\tlis %r0, 0x4330\n");
    ppc_emit_local_access(emit, "stw", "%r0", ppc_scratch_word(emit, false));
    ppc_emit_local_access(emit, "lfd", temp, slot);
    anvil_strbuf_append(&emit->code, is_unsigned ? "\tli %r0, 0\n" : "\tlis %r0, 0x8000\n");
    ppc_emit_local_access(emit, "stw", "%r0", ppc_scratch_word(emit, true));
    ppc_emit_local_access(emit, "lfd", dst, slot);
    anvil_strbuf_appendf(&emit->code, "\tfsub %s, %s, %s\n", dst, temp, dst);
    if (single)
        anvil_strbuf_appendf(&emit->code, "\tfrsp %s, %s\n", dst, dst);
}

static void ppc_emit_cast(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = ppc_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = ppc_vreg_info_checked(emit, src);
    if (!dst_info || !src_info)
        return;

    const char *dst = ppc_reg_name(emit, info->def);
    const char *source = ppc_reg_name(emit, src);
    if (emit->failed)
        return;

    if (ppc_is_conversion(info->op) && ppc_conversion_native_at(emit, instr_index, info)) {
        ppc_emit_native_conversion(emit, info->op, dst, source, dst_info, src_info);
        return;
    }

    switch (info->op) {
    case ANVIL_MIR_OP_ZEXT:
        ppc_emit_gpr_extend(emit, instr_index, info, false);
        break;
    case ANVIL_MIR_OP_SEXT:
        ppc_emit_gpr_extend(emit, instr_index, info, true);
        break;
    case ANVIL_MIR_OP_TRUNC:
    case ANVIL_MIR_OP_BITCAST:
        ppc_emit_copy(emit, info->def, src);
        break;
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP: {
        bool is_unsigned = info->op == ANVIL_MIR_OP_UITOFP;
        if (src_info->size_bits <= 8) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm %%r3, %s, 0, 24, 31\n" : "\textsb %%r3, %s\n", source);
        } else if (src_info->size_bits <= 16) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm %%r3, %s, 0, 16, 31\n" : "\textsh %%r3, %s\n", source);
        } else if (src_info->size_bits <= 32 && ppc_is_64(emit)) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trldicl %%r3, %s, 0, 32\n" : "\textsw %%r3, %s\n", source);
        } else if (strcmp(source, "%r3") != 0) {
            anvil_strbuf_appendf(&emit->code, "\tmr %%r3, %s\n", source);
        }

        const char *helper;
        if (is_unsigned) {
            helper = src_info->size_bits <= 32 ? (dst_info->size_bits == 32 ? "__floatunsisf" : "__floatunsidf") : (dst_info->size_bits == 32 ? "__floatundisf" : "__floatundidf");
        } else {
            helper = src_info->size_bits <= 32 ? (dst_info->size_bits == 32 ? "__floatsisf" : "__floatsidf") : (dst_info->size_bits == 32 ? "__floatdisf" : "__floatdidf");
        }
        emit->desc->abi_ops->emit_direct_call(emit, helper, emit->reloc != ANVIL_RELOC_STATIC);
        if (strcmp(dst, "%f1") != 0) {
            anvil_strbuf_appendf(&emit->code, "\tfmr %s, %%f1\n", dst);
        }
        break;
    }
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI: {
        bool is_unsigned = info->op == ANVIL_MIR_OP_FPTOUI;
        if (strcmp(source, "%f1") != 0) {
            anvil_strbuf_appendf(&emit->code, "\tfmr %%f1, %s\n", source);
        }
        const char *helper;
        if (is_unsigned) {
            helper = dst_info->size_bits <= 32 ? (src_info->size_bits == 32 ? "__fixunssfsi" : "__fixunsdfsi") : (src_info->size_bits == 32 ? "__fixunssfdi" : "__fixunsdfdi");
        } else {
            helper = dst_info->size_bits <= 32 ? (src_info->size_bits == 32 ? "__fixsfsi" : "__fixdfsi") : (src_info->size_bits == 32 ? "__fixsfdi" : "__fixdfdi");
        }
        emit->desc->abi_ops->emit_direct_call(emit, helper, emit->reloc != ANVIL_RELOC_STATIC);

        if (dst_info->size_bits <= 8) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm %s, %%r3, 0, 24, 31\n" : "\textsb %s, %%r3\n", dst);
        } else if (dst_info->size_bits <= 16) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm %s, %%r3, 0, 16, 31\n" : "\textsh %s, %%r3\n", dst);
        } else if (dst_info->size_bits <= 32 && ppc_is_64(emit)) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trldicl %s, %%r3, 0, 32\n" : "\textsw %s, %%r3\n", dst);
        } else if (strcmp(dst, "%r3") != 0) {
            anvil_strbuf_appendf(&emit->code, "\tmr %s, %%r3\n", dst);
        }
        break;
    }
    case ANVIL_MIR_OP_FPEXT:
        /* PowerPC FPRs hold a loaded f32 as its numeric value in the
         * register's wider representation, so extending it is a move. */
        ppc_emit_copy(emit, info->def, src);
        break;
    case ANVIL_MIR_OP_FPTRUNC:
        anvil_strbuf_appendf(&emit->code, "\tfrsp %s, %s\n", dst, source);
        break;
    default:
        emit->failed = true;
        break;
    }
}

static void ppc_emit_select(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t cond = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t then_v = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    anvil_mir_vreg_t else_v = anvil_mir_get_instr_use(emit->mir, instr_index, 2);
    if (cond == ANVIL_MIR_NO_VREG || then_v == ANVIL_MIR_NO_VREG || else_v == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const char *cond_reg = ppc_reg_name(emit, cond);
    if (!def_info || emit->failed)
        return;

    /* isel rD, rA, rB, bit picks rA when the CR0 bit is set (rA = r0 reads
     * as zero, so that order falls back to the branches). */
    int bit = emit->fused_select_bit;
    bool negated = emit->fused_select_negated, fused = bit >= 0;
    emit->fused_select_bit = -1;
    if (!fused) {
        bit = 2;          /* EQ after comparing the boolean with zero */
        negated = true;
    }
    if ((emit->features & ANVIL_FEATURE_PPC_ISEL) && def_info->reg_class == ANVIL_MIR_REG_GPR) {
        anvil_mir_vreg_t first = negated ? else_v : then_v, second = negated ? then_v : else_v;
        if (ppc_assignment_checked(emit, first) && ppc_assignment_checked(emit, first)->phys_reg != 0) {
            if (!fused)
                anvil_strbuf_appendf(&emit->code, "\tcmpwi 0, %s, 0\n", cond_reg);
            anvil_strbuf_appendf(&emit->code, "\tisel %s, %s, %s, %d\n", ppc_reg_name(emit, info->def), ppc_reg_name(emit, first),
                                 ppc_reg_name(emit, second), bit);
            return;
        }
    }
    if (fused) {
        /* The fused compare left no boolean: branch on its bit. */
        size_t id = emit->label_counter++;
        /* To the else value when the condition fails: the bit clear
         * (bc 4), or set for a negated condition (bc 12). */
        anvil_strbuf_appendf(&emit->code, "\tbc %d, %d, .L%s_select_else_%zu\n", negated ? 12 : 4, bit, anvil_mir_func_name(emit->mir), id);
        ppc_emit_copy(emit, info->def, then_v);
        anvil_strbuf_appendf(&emit->code, "\tb .L%s_select_done_%zu\n", anvil_mir_func_name(emit->mir), id);
        anvil_strbuf_appendf(&emit->code, ".L%s_select_else_%zu:\n", anvil_mir_func_name(emit->mir), id);
        ppc_emit_copy(emit, info->def, else_v);
        anvil_strbuf_appendf(&emit->code, ".L%s_select_done_%zu:\n", anvil_mir_func_name(emit->mir), id);
        return;
    }

    size_t id = emit->label_counter++;
    anvil_strbuf_appendf(&emit->code, "\tcmpwi 0, %s, 0\n", cond_reg);
    anvil_strbuf_appendf(&emit->code, "\tbeq .L%s_select_else_%zu\n", anvil_mir_func_name(emit->mir), id);
    ppc_emit_copy(emit, info->def, then_v);
    anvil_strbuf_appendf(&emit->code, "\tb .L%s_select_done_%zu\n", anvil_mir_func_name(emit->mir), id);
    anvil_strbuf_appendf(&emit->code, ".L%s_select_else_%zu:\n", anvil_mir_func_name(emit->mir), id);
    ppc_emit_copy(emit, info->def, else_v);
    anvil_strbuf_appendf(&emit->code, ".L%s_select_done_%zu:\n", anvil_mir_func_name(emit->mir), id);
}

static void ppc_emit_call_stack_arg(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
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

    const anvil_mir_vreg_info_t *value_info = ppc_vreg_info_checked(emit, value);
    const char *src = ppc_reg_name(emit, value);
    if (!value_info || emit->failed)
        return;

    int size = ppc_mir_size_bytes(value_info->size_bits);
    int offset = (int)emit->desc->outgoing_arg_offset + (int)info->imm;
    ppc_emit_sp_access(emit, ppc_store_op(emit->desc, value_info->reg_class, size), src, offset);
}

static void ppc_emit_spill_load(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
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

    const char *dst = ppc_reg_name(emit, info->def);
    if (emit->failed)
        return;
    int size = ppc_mir_size_bytes(slot.size_bits);
    ppc_emit_local_access(emit, ppc_load_op(emit->desc, slot.reg_class, size, false), dst, emit->spill_offsets[info->spill_slot]);
}

static void ppc_emit_spill_store(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
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

    const char *src = ppc_reg_name(emit, src_vreg);
    if (emit->failed)
        return;
    int size = ppc_mir_size_bytes(slot.size_bits);
    ppc_emit_local_access(emit, ppc_store_op(emit->desc, slot.reg_class, size), src, emit->spill_offsets[info->spill_slot]);
}

static void ppc_emit_ret(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (info->num_uses > 0) {
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        const anvil_mir_vreg_info_t *ret_info = ppc_vreg_info_checked(emit, ret);
        const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, ret);
        if (!ret_info || !assignment)
            return;

        int ret_reg = ret_info->reg_class == ANVIL_MIR_REG_FPR ? emit->desc->fpr_return_reg : emit->desc->gpr_return_reg;
        if (assignment->phys_reg != ret_reg) {
            if (ret_info->reg_class == ANVIL_MIR_REG_FPR) {
                anvil_strbuf_appendf(&emit->code, "\tfmr %s, %s\n", ppc_fpr_names[ret_reg], ppc_fpr_names[assignment->phys_reg]);
            } else {
                anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", ppc_gpr_names[ret_reg], ppc_gpr_names[assignment->phys_reg]);
            }
        }
    }
    if (!emit->failed)
        ppc_emit_epilogue(emit);
}

static void ppc_emit_indirect_call(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (info->num_uses == 0) {
        emit->failed = true;
        return;
    }

    anvil_mir_vreg_t target = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (target == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const char *target_reg = ppc_reg_name(emit, target);
    if (emit->failed)
        return;

    emit->desc->abi_ops->emit_indirect_call(emit, target_reg);
}

static void ppc_parallel_move_reg(void *user, int dst, int src)
{
    ppc_mir_emit_t *emit = user;
    anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", ppc_gpr_names[dst], ppc_gpr_names[src]);
}

/* Atomics as reservation loops (lwarx/stwcx., ldarx/stdcx. on 64-bit).
 * 8/16-bit read-modify-writes work on the containing aligned word through
 * a shifted mask, which needs no lbarx/lharx. Temporaries: r0, r5-r10,
 * with the operands staged in r3, r4, r11 (all declared clobbered). */
static void ppc_emit_atomic(ppc_mir_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info)
{
    anvil_memory_order_t order = info->atomic.order;
    bool acquire = order == ANVIL_ORDER_ACQUIRE || order == ANVIL_ORDER_ACQ_REL || order == ANVIL_ORDER_SEQ_CST;
    bool release = order == ANVIL_ORDER_RELEASE || order == ANVIL_ORDER_ACQ_REL || order == ANVIL_ORDER_SEQ_CST;
    const char *leading = order == ANVIL_ORDER_SEQ_CST ? "\tsync\n" : (release ? "\tlwsync\n" : "");

    if (info->ir_op == ANVIL_OP_ATOMIC_FENCE) {
        if (order != ANVIL_ORDER_RELAXED)
            anvil_strbuf_append(&emit->code, order == ANVIL_ORDER_SEQ_CST ? "\tsync\n" : "\tlwsync\n");
        return;
    }

    anvil_mir_vreg_t value = info->def != ANVIL_MIR_NO_VREG ? info->def : anvil_mir_get_instr_use(emit->mir, index, 1);
    const anvil_mir_vreg_info_t *type = ppc_vreg_info_checked(emit, value);
    if (!type || emit->failed)
        return;
    /* Operands are staged in r3, r4 and r11, clear of the loop's
     * temporaries, as one parallel move (they may sit anywhere). */
    static const int staging[3] = {3, 4, 11};
    static const anvil_parallel_move_ops_t moves = {ppc_parallel_move_reg, NULL, 0};
    const char *operand_names[3] = {NULL, NULL, NULL};
    int sources[3];
    size_t count = info->num_uses < 3 ? info->num_uses : 3;
    for (size_t operand = 0; operand < count; operand++) {
        const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, anvil_mir_get_instr_use(emit->mir, index, operand));
        if (!assignment)
            return;
        sources[operand] = assignment->phys_reg;
        operand_names[operand] = ppc_gpr_names[staging[operand]];
    }
    if (!anvil_parallel_move(&moves, emit, staging, sources, count)) {
        emit->failed = true;
        return;
    }

    const char *address = operand_names[0];
    unsigned bits = type->size_bits;
    const char *prefix = ".L";
    const char *function = anvil_mir_func_name(emit->mir);
    if (info->ir_op == ANVIL_OP_ATOMIC_LOAD || info->ir_op == ANVIL_OP_ATOMIC_STORE) {
        bool load = info->ir_op == ANVIL_OP_ATOMIC_LOAD;
        const char *op = bits == 8 ? (load ? "lbz" : "stb") : bits == 16 ? (load ? "lhz" : "sth") : bits == 32 ? (load ? "lwz" : "stw") : (load ? "ld" : "std");
        anvil_strbuf_append(&emit->code, load ? (order == ANVIL_ORDER_SEQ_CST ? "\tsync\n" : "") : leading);
        if (load) {
            anvil_strbuf_appendf(&emit->code, "\t%s %s, 0(%s)\n", op, ppc_reg_name(emit, info->def), address);
            if (acquire)
                anvil_strbuf_append(&emit->code, "\tlwsync\n");
        } else {
            anvil_strbuf_appendf(&emit->code, "\t%s %s, 0(%s)\n", op, operand_names[1], address);
        }
        return;
    }

    bool compare = info->ir_op == ANVIL_OP_ATOMIC_CMPXCHG;
    anvil_atomic_rmw_t rmw = info->atomic.rmw;
    anvil_strbuf_append(&emit->code, leading);
    if (bits >= 32) {
        bool wide = bits == 64;
        const char *larx = wide ? "ldarx" : "lwarx";
        const char *stcx = wide ? "stdcx." : "stwcx.";
        const char *update = operand_names[1];
        anvil_strbuf_appendf(&emit->code, "%s%s_atomic_%zu_retry:\n\t%s %%r7, 0, %s\n", prefix, function, index, larx, address);
        if (compare) {
            anvil_strbuf_appendf(&emit->code, "\t%s %%r7, %s\n\tbne- %s%s_atomic_%zu_done\n", wide ? "cmpd" : "cmpw", operand_names[1], prefix, function, index);
            update = operand_names[2];
        } else if (rmw != ANVIL_ATOMIC_EXCHANGE) {
            if (rmw == ANVIL_ATOMIC_SUB)
                anvil_strbuf_appendf(&emit->code, "\tsubf %%r8, %s, %%r7\n", operand_names[1]);
            else
                anvil_strbuf_appendf(&emit->code, "\t%s %%r8, %%r7, %s\n",
                                     rmw == ANVIL_ATOMIC_ADD ? "add" : rmw == ANVIL_ATOMIC_AND ? "and" : rmw == ANVIL_ATOMIC_OR ? "or" : "xor", operand_names[1]);
            update = "%r8";
        }
        anvil_strbuf_appendf(&emit->code, "\t%s %s, 0, %s\n\tbne- %s%s_atomic_%zu_retry\n%s%s_atomic_%zu_done:\n", stcx, update, address, prefix,
                             function, index, prefix, function, index);
        if (acquire)
            anvil_strbuf_append(&emit->code, "\tisync\n");
        anvil_strbuf_appendf(&emit->code, "\tmr %s, %%r7\n", ppc_reg_name(emit, info->def));
        return;
    }

    /* r10: field shift, r9: aligned word address, r0: field mask. */
    anvil_strbuf_appendf(&emit->code, "\trlwinm %%r10, %s, 3, 27, %d\n", address, bits == 8 ? 28 : 27);
    if (!emit->desc->little_endian)
        anvil_strbuf_appendf(&emit->code, "\txori %%r10, %%r10, %d\n", bits == 8 ? 24 : 16);
    if (ppc_is_64(emit))
        anvil_strbuf_appendf(&emit->code, "\trldicr %%r9, %s, 0, 61\n", address);
    else
        anvil_strbuf_appendf(&emit->code, "\trlwinm %%r9, %s, 0, 0, 29\n", address);
    anvil_strbuf_appendf(&emit->code, "\tli %%r0, -1\n\trlwinm %%r0, %%r0, 0, %d, 31\n\tslw %%r0, %%r0, %%r10\n", bits == 8 ? 24 : 16);
    /* r8: the operand (CMPXCHG: expected) in field position, r6: desired. */
    anvil_strbuf_appendf(&emit->code, "\tslw %%r8, %s, %%r10\n\tand %%r8, %%r8, %%r0\n", operand_names[1]);
    if (compare)
        anvil_strbuf_appendf(&emit->code, "\tslw %%r6, %s, %%r10\n\tand %%r6, %%r6, %%r0\n", operand_names[2]);

    anvil_strbuf_appendf(&emit->code, "%s%s_atomic_%zu_retry:\n\tlwarx %%r7, 0, %%r9\n", prefix, function, index);
    if (compare) {
        anvil_strbuf_appendf(&emit->code, "\tand %%r5, %%r7, %%r0\n\tcmpw %%r5, %%r8\n\tbne- %s%s_atomic_%zu_done\n", prefix, function, index);
        anvil_strbuf_append(&emit->code, "\tandc %r5, %r7, %r0\n\tor %r5, %r5, %r6\n");
    } else {
        switch (rmw) {
        case ANVIL_ATOMIC_EXCHANGE:
            anvil_strbuf_append(&emit->code, "\tandc %r5, %r7, %r0\n\tor %r5, %r5, %r8\n");
            break;
        case ANVIL_ATOMIC_ADD:
        case ANVIL_ATOMIC_SUB:
            anvil_strbuf_append(&emit->code, rmw == ANVIL_ATOMIC_ADD ? "\tadd %r5, %r7, %r8\n" : "\tsubf %r5, %r8, %r7\n");
            anvil_strbuf_append(&emit->code, "\tand %r5, %r5, %r0\n\tandc %r6, %r7, %r0\n\tor %r5, %r5, %r6\n");
            break;
        case ANVIL_ATOMIC_AND:
            anvil_strbuf_append(&emit->code, "\torc %r5, %r8, %r0\n\tand %r5, %r7, %r5\n");
            break;
        default:
            anvil_strbuf_appendf(&emit->code, "\t%s %%r5, %%r7, %%r8\n", rmw == ANVIL_ATOMIC_OR ? "or" : "xor");
            break;
        }
    }
    anvil_strbuf_appendf(&emit->code, "\tstwcx. %%r5, 0, %%r9\n\tbne- %s%s_atomic_%zu_retry\n%s%s_atomic_%zu_done:\n", prefix, function, index, prefix,
                         function, index);
    if (acquire)
        anvil_strbuf_append(&emit->code, "\tisync\n");
    anvil_strbuf_appendf(&emit->code, "\tand %%r7, %%r7, %%r0\n\tsrw %s, %%r7, %%r10\n", ppc_reg_name(emit, info->def));
}

static void ppc_emit_instr(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    switch (info->op) {
    case ANVIL_MIR_OP_ATOMIC:
        ppc_emit_atomic(emit, instr_index, info);
        return;
    case ANVIL_MIR_OP_MOV:
        ppc_emit_mov(emit, info);
        break;
    case ANVIL_MIR_OP_COPY: {
        anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        if (src == ANVIL_MIR_NO_VREG) {
            emit->failed = true;
            break;
        }
        ppc_emit_copy(emit, info->def, src);
        break;
    }
    case ANVIL_MIR_OP_VECTOR:
        ppc_emit_vector(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SCALAR:
        ppc_emit_scalar(emit, instr_index, info);
        break;
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
        ppc_emit_binary(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_NEG:
    case ANVIL_MIR_OP_NOT:
    case ANVIL_MIR_OP_FABS:
        ppc_emit_unary(emit, instr_index, info);
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
        ppc_emit_cast(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SELECT:
        ppc_emit_select(emit, instr_index, info);
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
        ppc_emit_cmp(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SYMBOL_ADDR:
        ppc_emit_symbol_addr(emit, info);
        break;
    case ANVIL_MIR_OP_LOAD:
        ppc_emit_load(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_STORE:
        ppc_emit_store(emit, instr_index);
        break;
    case ANVIL_MIR_OP_FRAME_ADDR:
        ppc_emit_frame_addr(emit, info);
        break;
    case ANVIL_MIR_OP_VA_START: {
        const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, info->def);
        if (!assignment || !emit->desc->abi_ops->emit_va_start)
            emit->failed = true;
        else
            emit->desc->abi_ops->emit_va_start(emit, assignment->phys_reg, info);
        break;
    }
    case ANVIL_MIR_OP_DYN_ALLOCA:
        ppc_emit_dyn_alloca(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        ppc_emit_incoming_stack_arg(emit, info);
        break;
    case ANVIL_MIR_OP_CALL:
        if (info->has_imm && emit->desc->abi_ops->emit_variadic_call_setup)
            emit->desc->abi_ops->emit_variadic_call_setup(emit, info->imm != 0);
        if (info->symbol && info->symbol[0]) {
            emit->desc->abi_ops->emit_direct_call(emit, info->symbol, info->symbol_got);
        } else {
            ppc_emit_indirect_call(emit, instr_index, info);
        }
        break;
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        ppc_emit_call_stack_arg(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_BR:
        if (info->true_block == emit->block + 1)
            break;
        anvil_strbuf_append(&emit->code, "\tb ");
        if (!ppc_emit_branch_target(emit, info->true_block)) {
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
        const char *taken = emit->fused_branch;
        emit->fused_branch = NULL;
        if (!taken) {
            const char *cond_reg = ppc_reg_name(emit, cond);
            if (emit->failed)
                break;
            anvil_strbuf_appendf(&emit->code, "\tcmpwi 0, %s, 0\n", cond_reg);
            taken = "bne";
        }
        ppc_emit_cond_branch(emit, taken, info->true_block, info->false_block);
        break;
    }
    case ANVIL_MIR_OP_RET:
        ppc_emit_ret(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SPILL_LOAD:
        ppc_emit_spill_load(emit, info);
        break;
    case ANVIL_MIR_OP_SPILL_STORE:
        ppc_emit_spill_store(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_CALL_RESULT: {
        /* Defined by the preceding call; the register was checked legal. */
        const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, info->def);
        const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
        if (!assignment || !def_info || !def_info->has_fixed_reg || assignment->phys_reg != def_info->fixed_phys_reg) {
            emit->failed = true;
        }
    } break;
    case ANVIL_MIR_OP_KEEPALIVE:
        break;
    case ANVIL_MIR_OP_RET_VALUE_PART:
        emit->failed = true;
        break;
    default:
        emit->failed = true;
        break;
    }
}

static void ppc_emit_escaped_string(anvil_strbuf_t *code, const char *value)
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

static void ppc_emit_string_literals(ppc_mir_emit_t *emit)
{
    size_t count = anvil_mir_num_string_literals(emit->mir);
    if (count == 0)
        return;

    anvil_strbuf_append(&emit->code, "\t.section .rodata\n");
    for (size_t i = 0; i < count; i++) {
        anvil_mir_string_literal_info_t info;
        if (!anvil_mir_get_string_literal_info(emit->mir, i, &info) || !info.label) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "%s:\n", info.label);
        ppc_emit_escaped_string(&emit->code, info.value);
    }
}

bool anvil_ppc_emit_mir(const anvil_mir_func_t *mir, anvil_ppc_variant_t variant, char **output, size_t *len)
{
    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc)
        return false;
    return anvil_ppc_emit_mir_features(mir, variant, anvil_cpu_model_features(anvil_arch_default_cpu(desc->arch)), output, len);
}

bool anvil_ppc_emit_mir_features(const anvil_mir_func_t *mir, anvil_ppc_variant_t variant, anvil_cpu_features_t features,
                                 char **output, size_t *len)
{
    return ppc_emit_mir_reloc(mir, variant, features, ANVIL_RELOC_STATIC, output, len);
}

bool ppc_emit_mir_reloc(const anvil_mir_func_t *mir, anvil_ppc_variant_t variant, anvil_cpu_features_t features,
                        anvil_reloc_model_t reloc, char **output, size_t *len)
{
    if (!output)
        return false;
    *output = NULL;
    if (len)
        *len = 0;

    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc || !mir)
        return false;
    if (!anvil_ppc_verify_mir_legal(mir, variant, NULL, 0))
        return false;

    ppc_mir_emit_t emit;
    memset(&emit, 0, sizeof(emit));
    emit.desc = desc;
    emit.features = features;
    emit.reloc = reloc == ANVIL_RELOC_DEFAULT ? ANVIL_RELOC_STATIC : reloc;
    emit.fused_select_bit = -1;
    emit.mir = mir;
    anvil_strbuf_init(&emit.code);

    if (!ppc_prepare_frame(&emit) || !anvil_mir_isel_facts_build(&emit.isel, mir, ppc_takes_immediate, &emit)) {
        anvil_strbuf_destroy(&emit.code);
        free(emit.spill_offsets);
        free(emit.frame_slot_offsets);
        anvil_mir_isel_facts_destroy(&emit.isel);
        return false;
    }

    ppc_emit_prologue(&emit);

    /* Loop headers start aligned (padding outside the loop is cheap). */
    bool *loop_headers = calloc(anvil_mir_num_blocks(mir) ? anvil_mir_num_blocks(mir) : 1, sizeof(*loop_headers));
    if (!loop_headers || !anvil_mir_loop_header_blocks(mir, loop_headers))
        emit.failed = true;
    for (size_t b = 0; b < anvil_mir_num_blocks(mir) && !emit.code.failed; b++) {
        if (loop_headers && loop_headers[b])
            anvil_strbuf_append(&emit.code, "\t.p2align 4\n");
        if (!ppc_emit_label(&emit, (anvil_mir_block_t)b)) {
            emit.failed = true;
            break;
        }
        emit.block = (anvil_mir_block_t)b;

        for (size_t instr_index = 0; instr_index < anvil_mir_num_instrs(mir); instr_index++) {
            anvil_mir_instr_info_t instr;
            if (!anvil_mir_get_instr_info(mir, instr_index, &instr)) {
                emit.failed = true;
                break;
            }
            if (instr.block != (anvil_mir_block_t)b)
                continue;
            ppc_emit_instr(&emit, instr_index, &instr);
            if (emit.failed || emit.code.failed)
                break;
        }
        if (emit.failed || emit.code.failed)
            break;
    }
    free(loop_headers);

    anvil_strbuf_append(&emit.code, "\t.cfi_endproc\n");
    ppc_emit_string_literals(&emit);

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
