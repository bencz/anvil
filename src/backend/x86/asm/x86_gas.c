#include "x86_gas.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

const char *x86_gas_setcc(anvil_mir_opcode_t op)
{
    switch (op) {
    case ANVIL_MIR_OP_CMP_EQ:
        return "sete";
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

const char *x86_gas_jcc(anvil_mir_opcode_t op)
{
    return x86_gas_setcc(op) + 3;
}

const char *x86_gas_inverse_jcc(const char *cc)
{
    static const char *const pairs[][2] = {{"e", "ne"}, {"l", "ge"}, {"le", "g"}, {"b", "ae"}, {"be", "a"}, {"p", "np"}};
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        if (strcmp(cc, pairs[i][0]) == 0)
            return pairs[i][1];
        if (strcmp(cc, pairs[i][1]) == 0)
            return pairs[i][0];
    }
    return NULL;
}

bool x86_gas_fcmp_branch(anvil_fcmp_pred_t pred, const char **cc, bool *swap)
{
    static const struct {
        const char *cc;
        bool swap;
    } branches[] = {
        [ANVIL_FCMP_OGT] = {"a", false},  [ANVIL_FCMP_OGE] = {"ae", false}, [ANVIL_FCMP_OLT] = {"a", true},
        [ANVIL_FCMP_OLE] = {"ae", true},  [ANVIL_FCMP_ONE] = {"ne", false}, [ANVIL_FCMP_ORD] = {"np", false},
        [ANVIL_FCMP_UEQ] = {"e", false},  [ANVIL_FCMP_UGT] = {"b", true},   [ANVIL_FCMP_UGE] = {"be", true},
        [ANVIL_FCMP_ULT] = {"b", false},  [ANVIL_FCMP_ULE] = {"be", false}, [ANVIL_FCMP_UNO] = {"p", false},
    };
    if ((unsigned)pred > (unsigned)ANVIL_FCMP_TRUE || !branches[pred].cc)
        return false;
    *cc = branches[pred].cc;
    *swap = branches[pred].swap;
    return true;
}

void x86_gas_emit_fcmp_bool(anvil_strbuf_t *code, const char *label_prefix, const char *function, size_t index,
                            anvil_fcmp_pred_t pred, const char *dst8)
{
    /* Per predicate: 1 less (CF), 2 greater, 4 equal (ZF), 8 unordered (PF). */
    static const unsigned masks[] = {0, 4, 2, 6, 1, 5, 3, 7, 12, 10, 14, 9, 13, 11, 8, 15};
    unsigned mask = (unsigned)pred < sizeof(masks) / sizeof(masks[0]) ? masks[pred] : 0;
    anvil_strbuf_appendf(code, "\tmovb $0, %%%s\n", dst8);
    if (mask == 15) {
        anvil_strbuf_appendf(code, "\tmovb $1, %%%s\n", dst8);
        return;
    }
    if (mask == 0)
        return;
    anvil_strbuf_appendf(code, "\tjp %s%s_fcmp_%s_%zu\n", label_prefix, function, mask & 8 ? "true" : "done", index);
    if (mask & 1)
        anvil_strbuf_appendf(code, "\tjb %s%s_fcmp_true_%zu\n", label_prefix, function, index);
    if (mask & 2)
        anvil_strbuf_appendf(code, "\tja %s%s_fcmp_true_%zu\n", label_prefix, function, index);
    if (mask & 4)
        anvil_strbuf_appendf(code, "\tje %s%s_fcmp_true_%zu\n", label_prefix, function, index);
    anvil_strbuf_appendf(code, "\tjmp %s%s_fcmp_done_%zu\n%s%s_fcmp_true_%zu:\n\tmovb $1, %%%s\n%s%s_fcmp_done_%zu:\n", label_prefix, function,
                         index, label_prefix, function, index, dst8, label_prefix, function, index);
}

bool x86_gas_emit_cmov(anvil_strbuf_t *code, const char *cc, const char *suffix, const char *dst, const char *then_reg, const char *else_reg)
{
    const char *inverse = x86_gas_inverse_jcc(cc);
    if (!inverse)
        return false;
    if (strcmp(dst, then_reg) == 0) {
        anvil_strbuf_appendf(code, "\tcmov%s%s %%%s, %%%s\n", inverse, suffix, else_reg, dst);
        return true;
    }
    if (strcmp(dst, else_reg) != 0)
        anvil_strbuf_appendf(code, "\tmov%s %%%s, %%%s\n", suffix, else_reg, dst);
    anvil_strbuf_appendf(code, "\tcmov%s%s %%%s, %%%s\n", cc, suffix, then_reg, dst);
    return true;
}

static bool x86_gas_label_name(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(mir, block, &info))
        return false;
    anvil_strbuf_appendf(code, "%s%s_%s", label_prefix, anvil_mir_func_name(mir), info.name && info.name[0] ? info.name : "block");
    return true;
}

bool x86_gas_emit_label(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t block)
{
    if (!x86_gas_label_name(code, label_prefix, mir, block))
        return false;
    anvil_strbuf_append(code, ":\n");
    return true;
}

bool x86_gas_emit_branch_target(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t block)
{
    return x86_gas_label_name(code, label_prefix, mir, block);
}

/* `<mnemonic><cc> <label of target>`. */
static bool x86_gas_emit_jump_insn(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, const char *mnemonic,
                                   const char *cc, anvil_mir_block_t target)
{
    anvil_strbuf_appendf(code, "\t%s%s ", mnemonic, cc);
    if (!x86_gas_label_name(code, label_prefix, mir, target))
        return false;
    anvil_strbuf_append(code, "\n");
    return true;
}

bool x86_gas_emit_cond_branch(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, const anvil_mir_layout_t *layout,
                              anvil_mir_block_t current, const char *cc, anvil_mir_block_t true_block, anvil_mir_block_t false_block)
{
    const char *inverse = x86_gas_inverse_jcc(cc);
    if (anvil_mir_layout_falls_into(layout, current, true_block) && inverse)
        return x86_gas_emit_jump_insn(code, label_prefix, mir, "j", inverse, anvil_mir_layout_target(layout, false_block));
    return x86_gas_emit_jump_insn(code, label_prefix, mir, "j", cc, anvil_mir_layout_target(layout, true_block)) &&
           x86_gas_emit_jump(code, label_prefix, mir, layout, current, false_block);
}

bool x86_gas_emit_jump(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, const anvil_mir_layout_t *layout,
                       anvil_mir_block_t current, anvil_mir_block_t target)
{
    return anvil_mir_layout_falls_into(layout, current, target) ||
           x86_gas_emit_jump_insn(code, label_prefix, mir, "jmp", "", anvil_mir_layout_target(layout, target));
}

bool x86_gas_compare_folds(const anvil_mir_func_t *mir, size_t reader, anvil_mir_vreg_t load, unsigned load_bits, bool *load_is_lhs,
                           anvil_mir_vreg_t *other)
{
    anvil_mir_instr_info_t info;
    if (!anvil_mir_get_instr_info(mir, reader, &info) || info.op < ANVIL_MIR_OP_CMP_EQ || info.op > ANVIL_MIR_OP_CMP_UGE || info.num_uses != 2)
        return false;
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(mir, reader, 0), rhs = anvil_mir_get_instr_use(mir, reader, 1);
    const anvil_mir_vreg_info_t *lhs_info = anvil_mir_get_vreg_info(mir, lhs);
    if (lhs == rhs || (lhs != load && rhs != load) || !lhs_info || lhs_info->reg_class != ANVIL_MIR_REG_GPR || lhs_info->size_bits != load_bits)
        return false;
    *load_is_lhs = lhs == load;
    *other = lhs == load ? rhs : lhs;
    return true;
}

void x86_gas_emit_memory_compare(anvil_strbuf_t *code, const char *suffix, const char *address, bool load_is_lhs, const char *other_reg)
{
    if (load_is_lhs)
        anvil_strbuf_appendf(code, "\tcmp%s %%%s, %s\n", suffix, other_reg, address);
    else
        anvil_strbuf_appendf(code, "\tcmp%s %s, %%%s\n", suffix, address, other_reg);
}

static const char *const gpr_names[4][16] = {
    {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"},
    {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"},
    {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"},
    {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"},
};

static const char *const xmm_names[16] = {"xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
                                          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};

const char *x86_gas_gpr(int phys, int bytes)
{
    if (phys < 0 || phys >= 16)
        return "?";
    return gpr_names[bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : 3][phys];
}

const char *x86_gas_xmm(int phys)
{
    return phys >= 0 && phys < 16 ? xmm_names[phys] : "?";
}

const char *x86_gas_size_suffix(int bytes)
{
    return bytes == 1 ? "b" : bytes == 2 ? "w" : bytes == 4 ? "l" : "q";
}

int x86_gas_phys_of(x86_gas_t *gas, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(gas->mir, vreg);
    if (!assignment || assignment->spilled || assignment->phys_reg < 0) {
        gas->failed = true;
        return -1;
    }
    return assignment->phys_reg;
}

const char *x86_gas_frame(x86_gas_t *gas, int disp)
{
    char *text = gas->frame_text[gas->frame_next++ % 8];
    snprintf(text, sizeof(gas->frame_text[0]), "%d(%%%s)", disp + gas->frame_bias, gas->frame_reg);
    return text;
}

bool x86_gas_address_reads(x86_gas_t *gas, size_t address_index, const anvil_mir_instr_info_t *address, int phys)
{
    size_t uses = address->index_scale ? 2 : 1;
    for (size_t u = 0; u < uses && u < address->num_uses; u++)
        if (x86_gas_phys_of(gas, anvil_mir_get_instr_use(gas->mir, address_index, u)) == phys)
            return true;
    return false;
}

void x86_gas_parallel_move(void *user, int dst, int src)
{
    x86_gas_t *gas = user;
    anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n", x86_gas_size_suffix(gas->pointer_bytes), x86_gas_gpr(src, gas->pointer_bytes),
                         x86_gas_gpr(dst, gas->pointer_bytes));
}

void x86_gas_parallel_swap(void *user, int a, int b)
{
    x86_gas_t *gas = user;
    anvil_strbuf_appendf(&gas->code, "\txchg%s %%%s, %%%s\n", x86_gas_size_suffix(gas->pointer_bytes), x86_gas_gpr(a, gas->pointer_bytes),
                         x86_gas_gpr(b, gas->pointer_bytes));
}

void x86_gas_emit_frame_addr(x86_gas_t *gas, const anvil_mir_instr_info_t *info)
{
    if (info->frame_slot < 0 || (size_t)info->frame_slot >= gas->num_frame_slot_offsets) {
        gas->failed = true;
        return;
    }
    int dst = x86_gas_phys_of(gas, info->def);
    if (gas->failed)
        return;
    anvil_strbuf_appendf(&gas->code, "\tlea%s %s, %%%s\n", x86_gas_size_suffix(gas->pointer_bytes), x86_gas_frame(gas, -gas->frame_slot_offsets[info->frame_slot]),
                         x86_gas_gpr(dst, gas->pointer_bytes));
}

/* A spill slot's frame operand and the bytes a general register moves. */
static bool spill_slot(x86_gas_t *gas, const anvil_mir_instr_info_t *info, anvil_mir_spill_slot_info_t *slot, const char **mem, int *bytes)
{
    if (info->spill_slot < 0 || (size_t)info->spill_slot >= gas->num_spill_offsets || !anvil_mir_get_spill_slot_info(gas->mir, info->spill_slot, slot)) {
        gas->failed = true;
        return false;
    }
    *mem = x86_gas_frame(gas, -gas->spill_offsets[info->spill_slot]);
    *bytes = (slot->size_bits + 7) / 8;
    if (*bytes < gas->min_gpr_move)
        *bytes = gas->min_gpr_move;
    return true;
}

void x86_gas_emit_spill_load(x86_gas_t *gas, const anvil_mir_instr_info_t *info)
{
    anvil_mir_spill_slot_info_t slot;
    const char *mem;
    int bytes;
    if (!spill_slot(gas, info, &slot, &mem, &bytes) || !anvil_mir_get_vreg_info(gas->mir, info->def)) {
        gas->failed = true;
        return;
    }
    if (slot.reg_class == ANVIL_MIR_REG_FPR) {
        gas->fp_load(gas, info->def, mem);
        return;
    }
    int def = x86_gas_phys_of(gas, info->def);
    if (!gas->failed)
        anvil_strbuf_appendf(&gas->code, "\tmov%s %s, %%%s\n", x86_gas_size_suffix(bytes), mem, x86_gas_gpr(def, bytes));
}

void x86_gas_emit_spill_store(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_spill_slot_info_t slot;
    const char *mem;
    int bytes;
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(gas->mir, index, 0);
    if (value == ANVIL_MIR_NO_VREG || !spill_slot(gas, info, &slot, &mem, &bytes)) {
        gas->failed = true;
        return;
    }
    if (slot.reg_class == ANVIL_MIR_REG_FPR) {
        gas->fp_store(gas, value, mem);
        return;
    }
    int src = x86_gas_phys_of(gas, value);
    if (!gas->failed)
        anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %s\n", x86_gas_size_suffix(bytes), x86_gas_gpr(src, bytes), mem);
}

void x86_gas_emit_select(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t cond = anvil_mir_get_instr_use(gas->mir, index, 0);
    anvil_mir_vreg_t then_value = anvil_mir_get_instr_use(gas->mir, index, 1);
    anvil_mir_vreg_t else_value = anvil_mir_get_instr_use(gas->mir, index, 2);
    const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(gas->mir, info->def);
    if (cond == ANVIL_MIR_NO_VREG || then_value == ANVIL_MIR_NO_VREG || else_value == ANVIL_MIR_NO_VREG || !def_info) {
        gas->failed = true;
        return;
    }
    int bytes = def_info->size_bits > 32 ? 8 : 4;
    int dst = x86_gas_phys_of(gas, info->def), then_reg = x86_gas_phys_of(gas, then_value), else_reg = x86_gas_phys_of(gas, else_value);
    const char *cc = gas->fused_jcc;
    gas->fused_jcc = NULL;
    if (!cc) {
        int flag = x86_gas_phys_of(gas, cond);
        bool byte = !gas->has_byte || gas->has_byte(flag);
        const char *name = x86_gas_gpr(flag, byte ? 1 : 4);
        if (!gas->failed)
            anvil_strbuf_appendf(&gas->code, "\ttest%s %%%s, %%%s\n", byte ? "b" : "l", name, name);
        cc = "ne";
    }
    if (!gas->failed && !x86_gas_emit_cmov(&gas->code, cc, x86_gas_size_suffix(bytes), x86_gas_gpr(dst, bytes), x86_gas_gpr(then_reg, bytes),
                                           x86_gas_gpr(else_reg, bytes)))
        gas->failed = true;
}

void x86_gas_emit_call_stack_arg(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(gas->mir, index, 0);
    const anvil_mir_vreg_info_t *value_info = anvil_mir_get_vreg_info(gas->mir, value);
    if (!info->has_imm || info->imm < 0 || info->num_uses != 1 || info->imm > INT32_MAX - gas->outgoing_base || !value_info) {
        gas->failed = true;
        return;
    }
    char mem[32];
    snprintf(mem, sizeof(mem), "%d(%%%s)", gas->outgoing_base + (int)info->imm, gas->stack_reg);
    if (value_info->reg_class == ANVIL_MIR_REG_FPR) {
        gas->fp_store(gas, value, mem);
        return;
    }
    int bytes = (value_info->size_bits + 7) / 8;
    if (bytes < gas->min_gpr_move)
        bytes = gas->min_gpr_move;
    int src = x86_gas_phys_of(gas, value);
    if (!gas->failed)
        anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %s\n", x86_gas_size_suffix(bytes), x86_gas_gpr(src, bytes), mem);
}

bool x86_gas_facts_imm(const anvil_mir_isel_facts_t *facts, bool wraps_imm32, anvil_mir_vreg_t vreg, int64_t *out)
{
    int64_t value;
    if (!anvil_mir_isel_constant(facts, vreg, &value))
        return false;
    if (wraps_imm32)
        value = (int32_t)value;
    else if (value < INT32_MIN || value > INT32_MAX)
        return false;
    *out = value;
    return true;
}

bool x86_gas_known_imm(const x86_gas_t *gas, anvil_mir_vreg_t vreg, int64_t *out)
{
    return x86_gas_facts_imm(&gas->isel, gas->wraps_imm32, vreg, out);
}

/* Mirrors the binary-operation and compare printers. */
bool x86_gas_use_takes_immediate(void *ctx, size_t index, const anvil_mir_instr_info_t *info, size_t use, int64_t value)
{
    const x86_gas_t *gas = ctx;
    const anvil_mir_vreg_info_t *def = info->def != ANVIL_MIR_NO_VREG ? anvil_mir_get_vreg_info(gas->mir, info->def) : NULL;
    bool fits = gas->wraps_imm32 || (value >= INT32_MIN && value <= INT32_MAX);
    int64_t other;
    /* A stored integer: mov $imm, mem (64-bit: a sign-extended imm32). */
    if (info->op == ANVIL_MIR_OP_STORE && use == 0) {
        const anvil_mir_vreg_info_t *stored = anvil_mir_get_vreg_info(gas->mir, anvil_mir_get_instr_use(gas->mir, index, 0));
        return stored && stored->reg_class == ANVIL_MIR_REG_GPR && stored->size_bits <= gas->pointer_bytes * 8 && (stored->size_bits <= 32 || fits);
    }
    if (!def || def->reg_class != ANVIL_MIR_REG_GPR || info->num_uses != 2 || !fits)
        return false;
    switch (info->op) {
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_MUL:
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR:
        /* The right operand when constant, else a constant left one. */
        return use == 1 || !x86_gas_known_imm(gas, anvil_mir_get_instr_use(gas->mir, index, 1), &other);
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
        return use == 1;
    case ANVIL_MIR_OP_SCALAR:
        return info->ir_op == ANVIL_OP_ROTL && use == 1;
    default:
        if (anvil_mir_isel_is_compare(info->op) && info->op != ANVIL_MIR_OP_FCMP) {
            const anvil_mir_vreg_info_t *lhs = anvil_mir_get_vreg_info(gas->mir, anvil_mir_get_instr_use(gas->mir, index, 0));
            return use == 1 && lhs && lhs->reg_class == ANVIL_MIR_REG_GPR;
        }
        return false;
    }
}

bool x86_gas_memory_operand(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, size_t base_use, char *out, size_t out_len)
{
    anvil_mir_vreg_t base = anvil_mir_get_instr_use(gas->mir, index, base_use);
    anvil_mir_vreg_t index_vreg = info->index_scale ? anvil_mir_get_instr_use(gas->mir, index, base_use + 1) : ANVIL_MIR_NO_VREG;
    if (base == ANVIL_MIR_NO_VREG || (info->index_scale && index_vreg == ANVIL_MIR_NO_VREG)) {
        gas->failed = true;
        return false;
    }
    int64_t imm = info->has_imm ? info->imm : 0, offset;
    const char *symbol;
    char disp[300] = "";
    bool absolute = !anvil_mir_isel_materialize(&gas->isel, base) && anvil_mir_isel_symbol(&gas->isel, base, &symbol, &offset);
    const char *prefix = absolute ? x86_gas_symbol_ref_prefix(gas, symbol) : "";
    if (absolute && imm + offset)
        snprintf(disp, sizeof(disp), "%s%s%+lld", prefix, symbol, (long long)(imm + offset));
    else if (absolute)
        snprintf(disp, sizeof(disp), "%s%s", prefix, symbol);
    else if (imm)
        snprintf(disp, sizeof(disp), "%lld", (long long)imm);
    const char *base_name = absolute ? NULL : x86_gas_gpr(x86_gas_phys_of(gas, base), gas->pointer_bytes);
    if (info->index_scale)
        snprintf(out, out_len, "%s(%s%s,%%%s,%u)", disp, base_name ? "%" : "", base_name ? base_name : "",
                 x86_gas_gpr(x86_gas_phys_of(gas, index_vreg), gas->pointer_bytes), info->index_scale);
    else if (base_name)
        snprintf(out, out_len, "%s(%%%s)", disp, base_name);
    else
        snprintf(out, out_len, "%s", disp);
    return !gas->failed;
}

static bool fold_into(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, size_t reader)
{
    if (!x86_gas_memory_operand(gas, index, info, 0, gas->fold_address, sizeof(gas->fold_address)))
        return false;
    gas->fold_target = reader;
    gas->fold_load = index;
    return true;
}

bool x86_gas_fold_load(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, const anvil_mir_vreg_info_t *def_info)
{
    bool fpr = def_info->reg_class == ANVIL_MIR_REG_FPR;
    /* One fold at a time: a pending one keeps its operand. */
    if (gas->fold_target != SIZE_MAX || (fpr && !gas->fp_memory_operands) || (!fpr && def_info->reg_class != ANVIL_MIR_REG_GPR) ||
        def_info->size_bits > gas->pointer_bytes * 8 || info->memory_access.is_volatile || gas->isel.use_count[info->def] != 1)
        return false;
    /* The reader, a few instructions on in the block: nothing between may
     * write memory or the address's registers. */
    anvil_mir_instr_info_t next;
    size_t reader = anvil_mir_isel_fold_reader(gas->mir, &gas->isel, index, 8, true);
    if (reader == SIZE_MAX || !anvil_mir_get_instr_info(gas->mir, reader, &next))
        return false;
    /* An integer compare of the load's width with a register, either side
     * (narrow loads too: the compare reads just their bytes); a byte
     * compare's register is a byte register. */
    bool load_is_lhs;
    anvil_mir_vreg_t compared;
    int64_t imm;
    if (!fpr && x86_gas_compare_folds(gas->mir, reader, info->def, def_info->size_bits, &load_is_lhs, &compared) &&
        !x86_gas_known_imm(gas, compared, &imm)) {
        int phys = x86_gas_phys_of(gas, compared);
        if (gas->failed || (def_info->size_bits == 8 && gas->has_byte && !gas->has_byte(phys)))
            return false;
        return fold_into(gas, index, info, reader);
    }
    if (def_info->size_bits < 32 || next.num_uses != 2 || next.def == ANVIL_MIR_NO_VREG)
        return false;
    bool gpr_op = next.op == ANVIL_MIR_OP_ADD || next.op == ANVIL_MIR_OP_SUB || next.op == ANVIL_MIR_OP_AND || next.op == ANVIL_MIR_OP_OR ||
                  next.op == ANVIL_MIR_OP_XOR || next.op == ANVIL_MIR_OP_MUL;
    bool fpr_op = next.op == ANVIL_MIR_OP_ADD || next.op == ANVIL_MIR_OP_SUB || next.op == ANVIL_MIR_OP_MUL || next.op == ANVIL_MIR_OP_DIV ||
                  next.op == ANVIL_MIR_OP_FDIV;
    const anvil_mir_vreg_info_t *result = anvil_mir_get_vreg_info(gas->mir, next.def);
    if (!result || result->reg_class != def_info->reg_class || result->size_bits != def_info->size_bits || !(fpr ? fpr_op : gpr_op))
        return false;
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(gas->mir, reader, 0), rhs = anvil_mir_get_instr_use(gas->mir, reader, 1);
    bool commutes = next.op == ANVIL_MIR_OP_ADD || next.op == ANVIL_MIR_OP_MUL || next.op == ANVIL_MIR_OP_AND || next.op == ANVIL_MIR_OP_OR ||
                    next.op == ANVIL_MIR_OP_XOR;
    if (lhs == rhs || (rhs != info->def && !(commutes && lhs == info->def)))
        return false;
    anvil_mir_vreg_t other = rhs == info->def ? lhs : rhs;
    int dst = x86_gas_phys_of(gas, next.def), keep = x86_gas_phys_of(gas, other);
    if (gas->failed || (!fpr && x86_gas_known_imm(gas, other, &imm)) || (!fpr && dst != keep && x86_gas_address_reads(gas, index, info, dst)))
        return false;
    return fold_into(gas, index, info, reader);
}

void x86_gas_emit_load(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(gas->mir, info->def);
    char address[sizeof(gas->fold_address)];
    if (!def_info) {
        gas->failed = true;
        return;
    }
    int def = x86_gas_phys_of(gas, info->def);
    if (gas->failed || x86_gas_fold_load(gas, index, info, def_info) || !x86_gas_memory_operand(gas, index, info, 0, address, sizeof(address)))
        return;
    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        gas->fp_load(gas, info->def, address);
        return;
    }
    /* Narrow integers load extended to 32 bits. */
    int bytes = (def_info->size_bits + 7) / 8;
    const char *op = bytes == 1 ? (def_info->is_signed ? "movsbl" : "movzbl") : bytes == 2 ? (def_info->is_signed ? "movswl" : "movzwl") : NULL;
    if (op)
        anvil_strbuf_appendf(&gas->code, "\t%s %s, %%%s\n", op, address, x86_gas_gpr(def, 4));
    else
        anvil_strbuf_appendf(&gas->code, "\tmov%s %s, %%%s\n", x86_gas_size_suffix(bytes), address, x86_gas_gpr(def, bytes));
}

void x86_gas_emit_store(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(gas->mir, index, 0);
    const anvil_mir_vreg_info_t *value_info = value == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_vreg_info(gas->mir, value);
    char address[sizeof(gas->fold_address)];
    if (!value_info || gas->failed || !x86_gas_memory_operand(gas, index, info, 1, address, sizeof(address))) {
        gas->failed = true;
        return;
    }
    if (value_info->reg_class == ANVIL_MIR_REG_FPR) {
        gas->fp_store(gas, value, address);
        return;
    }
    int bytes = (value_info->size_bits + 7) / 8;
    int64_t imm;
    if (anvil_mir_isel_constant(&gas->isel, value, &imm) && x86_gas_use_takes_immediate(gas, index, info, 0, imm)) {
        long long shown = bytes == 1 ? (long long)(uint8_t)imm : bytes == 2 ? (long long)(uint16_t)imm : bytes == 4 ? (long long)(uint32_t)imm : (long long)imm;
        anvil_strbuf_appendf(&gas->code, "\tmov%s $%lld, %s\n", x86_gas_size_suffix(bytes), shown, address);
        return;
    }
    int src = x86_gas_phys_of(gas, value);
    if (gas->failed || (bytes == 1 && gas->has_byte && !gas->has_byte(src))) {
        gas->failed = true;
        return;
    }
    anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %s\n", x86_gas_size_suffix(bytes), x86_gas_gpr(src, bytes), address);
}

bool x86_gas_mark_registerless(anvil_mir_func_t *mir, void (*mode)(x86_gas_t *gas, const void *user), const void *user)
{
    x86_gas_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.mir = mir;
    mode(&probe, user);
    if (!anvil_mir_sink_compares(mir) || !anvil_mir_isel_facts_build(&probe.isel, mir, x86_gas_use_takes_immediate, NULL, &probe))
        return false;
    bool ok = anvil_mir_isel_mark_registerless(mir, &probe.isel);
    anvil_mir_isel_facts_destroy(&probe.isel);
    return ok;
}

const char *x86_gas_symbol_ref_prefix(const x86_gas_t *gas, const char *symbol)
{
    return symbol && symbol[0] == '.' ? "" : gas->symbol_prefix;
}
