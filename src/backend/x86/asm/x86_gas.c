#include "x86_gas.h"
#include "../x86_address.h"

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

/* The register shifts read a variable count from (CL). */
enum { X86_GAS_COUNT_REG = 1 };

static bool emit_shift(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, int dst_phys, int a_phys, int b_phys, int size, int width)
{
    const char *sh = info->op == ANVIL_MIR_OP_SHL ? "shl" : info->op == ANVIL_MIR_OP_SHR ? "shr" : "sar";
    const char *move = x86_gas_size_suffix(width), *suffix = x86_gas_size_suffix(size);
    const char *a = x86_gas_gpr(a_phys, width), *dst = x86_gas_gpr(dst_phys, width);
    int64_t count = 0;
    if (size == 1 && gas->has_byte && !gas->has_byte(dst_phys)) {
        gas->failed = true;
        return true;
    }
    if (x86_gas_known_imm(gas, anvil_mir_get_instr_use(gas->mir, index, 1), &count)) {
        if (dst_phys != a_phys)
            anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n", move, a, dst);
        anvil_strbuf_appendf(&gas->code, "\t%s%s $%d, %%%s\n", sh, suffix, (int)(count & (size == 8 ? 63 : 31)), x86_gas_gpr(dst_phys, size));
        return true;
    }
    /* CL is the printer's temporary; dst may share b's register: b reaches
     * CL first. */
    anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n", move, x86_gas_gpr(b_phys, width), x86_gas_gpr(X86_GAS_COUNT_REG, width));
    if (dst_phys != a_phys)
        anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n", move, a, dst);
    anvil_strbuf_appendf(&gas->code, "\t%s%s %%cl, %%%s\n", sh, suffix, x86_gas_gpr(dst_phys, size));
    return true;
}

bool x86_gas_emit_gpr_arith(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, int dst_phys, int a_phys, int b_phys, int size)
{
    int width = size < gas->min_operation_bytes ? gas->min_operation_bytes : size, pointer = gas->pointer_bytes;
    if (info->op == ANVIL_MIR_OP_SHL || info->op == ANVIL_MIR_OP_SHR || info->op == ANVIL_MIR_OP_SAR)
        return gas->failed || emit_shift(gas, index, info, dst_phys, a_phys, b_phys, size, width);
    const char *mnemonic = info->op == ANVIL_MIR_OP_ADD ? "add" : info->op == ANVIL_MIR_OP_SUB ? "sub" : info->op == ANVIL_MIR_OP_MUL ? "imul"
                         : info->op == ANVIL_MIR_OP_AND ? "and" : info->op == ANVIL_MIR_OP_OR ? "or" : info->op == ANVIL_MIR_OP_XOR ? "xor" : NULL;
    if (!mnemonic)
        return false;
    if (gas->failed)
        return true;
    const char *suffix = x86_gas_size_suffix(width);
    const char *a = x86_gas_gpr(a_phys, width), *b = x86_gas_gpr(b_phys, width), *dst = x86_gas_gpr(dst_phys, width);
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(gas->mir, index, 0), rhs = anvil_mir_get_instr_use(gas->mir, index, 1);
    bool commutative = info->op != ANVIL_MIR_OP_SUB;
    int64_t imm = 0;
    bool b_imm = x86_gas_known_imm(gas, rhs, &imm);
    if (!b_imm && commutative && x86_gas_known_imm(gas, lhs, &imm)) {
        /* x OP c == c OP x */
        b_imm = true;
        a_phys = b_phys;
        a = b;
    }
    bool wide = width >= 4;
    if (b_imm) {
        unsigned shift = 0;
        anvil_mir_mul_form_t form = info->op == ANVIL_MIR_OP_MUL ? anvil_mir_isel_mul_form(imm, (unsigned)width * 8, &shift) : ANVIL_MIR_MUL_MULTIPLY;
        if (form == ANVIL_MIR_MUL_SHIFT) {
            if (dst_phys != a_phys)
                anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n", suffix, a, dst);
            if (shift != 0)
                anvil_strbuf_appendf(&gas->code, "\tshl%s $%u, %%%s\n", suffix, shift, dst);
        } else if (form == ANVIL_MIR_MUL_SHIFT_ADD && wide && x86_address_mode(NULL, 1u << shift, 0, 0, ANVIL_MIR_INDEX_PLAIN)) {
            /* x * (2^k + 1): x + x * 2^k, one lea for scales 2, 4, 8. */
            anvil_strbuf_appendf(&gas->code, "\tlea%s (%%%s,%%%s,%u), %%%s\n", suffix, x86_gas_gpr(a_phys, pointer), x86_gas_gpr(a_phys, pointer), 1u << shift, dst);
        } else if (info->op == ANVIL_MIR_OP_MUL) {
            /* Bytes multiply in 16 bits: the low byte is the same. */
            int bytes = width >= 2 ? width : 2;
            anvil_strbuf_appendf(&gas->code, "\timul%s $%lld, %%%s, %%%s\n", x86_gas_size_suffix(bytes), (long long)imm, x86_gas_gpr(a_phys, bytes),
                                 x86_gas_gpr(dst_phys, bytes));
        } else if (dst_phys != a_phys && wide && (info->op == ANVIL_MIR_OP_ADD || (info->op == ANVIL_MIR_OP_SUB && imm != INT32_MIN))) {
            long long offset = info->op == ANVIL_MIR_OP_ADD ? (long long)imm : -(long long)imm;
            anvil_strbuf_appendf(&gas->code, "\tlea%s %lld(%%%s), %%%s\n", suffix, offset, x86_gas_gpr(a_phys, pointer), dst);
        } else {
            if (dst_phys != a_phys)
                anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n", suffix, a, dst);
            anvil_strbuf_appendf(&gas->code, "\t%s%s $%lld, %%%s\n", mnemonic, suffix, (long long)imm, dst);
            anvil_print_flags_set(&gas->flags, dst_phys, width);
        }
        return true;
    }
    bool lea = dst_phys != a_phys && dst_phys != b_phys && info->op == ANVIL_MIR_OP_ADD && wide;
    if (dst_phys == a_phys)
        anvil_strbuf_appendf(&gas->code, "\t%s%s %%%s, %%%s\n", mnemonic, suffix, b, dst);
    else if (lea)
        anvil_strbuf_appendf(&gas->code, "\tlea%s (%%%s,%%%s), %%%s\n", suffix, x86_gas_gpr(a_phys, pointer), x86_gas_gpr(b_phys, pointer), dst);
    else if (dst_phys != b_phys)
        anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n\t%s%s %%%s, %%%s\n", suffix, a, dst, mnemonic, suffix, b, dst);
    else if (commutative)
        anvil_strbuf_appendf(&gas->code, "\t%s%s %%%s, %%%s\n", mnemonic, suffix, a, dst);
    else
        /* dst holds the subtrahend: a - b == -b + a. */
        anvil_strbuf_appendf(&gas->code, "\tneg%s %%%s\n\tadd%s %%%s, %%%s\n", suffix, dst, suffix, a, dst);
    /* Every form but lea (and imul's) ends with the operation into dst,
     * which sets ZF from the result. */
    if (!lea && info->op != ANVIL_MIR_OP_MUL)
        anvil_print_flags_set(&gas->flags, dst_phys, width);
    return true;
}

bool x86_gas_fuses(const x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info)
{
    return anvil_mir_isel_feeds_next_branch(&gas->isel, index, info) || anvil_mir_isel_feeds_next_select(&gas->isel, index, info);
}

/* The boolean of the compare the flags hold: setcc, widened to 32 bits. */
static void emit_setcc(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, const char *dst8, const char *dst32)
{
    if (info->op == ANVIL_MIR_OP_FCMP)
        x86_gas_emit_fcmp_bool(&gas->code, gas->local_label_prefix, anvil_mir_func_name(gas->mir), index, (anvil_fcmp_pred_t)info->imm, dst8);
    else
        anvil_strbuf_appendf(&gas->code, "\t%s %%%s\n", x86_gas_setcc(info->op), dst8);
    anvil_strbuf_appendf(&gas->code, "\tmovzbl %%%s, %%%s\n", dst8, dst32);
}

static bool byte_register(const x86_gas_t *gas, int phys)
{
    return !gas->has_byte || gas->has_byte(phys);
}

/* An integer compare's flags (false when the compare needs no code of its
 * own: the flags of the value just computed tell already). */
static bool integer_compare(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, const anvil_mir_vreg_info_t *lhs_info, bool fuses)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(gas->mir, index, 0), rhs = anvil_mir_get_instr_use(gas->mir, index, 1);
    int size = (lhs_info->size_bits + 7) / 8;
    const char *suffix = x86_gas_size_suffix(size);
    bool load_is_lhs;
    anvil_mir_vreg_t compared;
    anvil_mir_instr_info_t load;
    if (index == gas->fold_target && anvil_mir_get_instr_info(gas->mir, gas->fold_load, &load) &&
        x86_gas_compare_folds(gas->mir, index, load.def, lhs_info->size_bits, &load_is_lhs, &compared)) {
        /* The folded load is the memory operand. */
        gas->fold_target = SIZE_MAX;
        const char *other = x86_gas_gpr(x86_gas_phys_of(gas, compared), size);
        if (!gas->failed)
            x86_gas_emit_memory_compare(&gas->code, suffix, gas->fold_address, load_is_lhs, other);
        return true;
    }
    int a_phys = x86_gas_phys_of(gas, lhs), b_phys = x86_gas_phys_of(gas, rhs);
    const char *a = x86_gas_gpr(a_phys, size);
    int64_t imm = 0;
    if (gas->failed || (size == 1 && !byte_register(gas, a_phys))) {
        gas->failed = true;
        return true;
    }
    if (x86_gas_known_imm(gas, rhs, &imm)) {
        imm = size == 1 ? (int8_t)imm : size == 2 ? (int16_t)imm : imm;
        /* == 0 or != 0 of the value just computed: its flags tell. */
        if (imm == 0 && fuses && (info->op == ANVIL_MIR_OP_CMP_EQ || info->op == ANVIL_MIR_OP_CMP_NE) &&
            anvil_print_flags_take(&gas->flags, &gas->code, a_phys, size))
            return false;
        /* test sets the flags of a compare with zero, in fewer bytes. */
        if (imm == 0)
            anvil_strbuf_appendf(&gas->code, "\ttest%s %%%s, %%%s\n", suffix, a, a);
        else
            anvil_strbuf_appendf(&gas->code, "\tcmp%s $%lld, %%%s\n", suffix, (long long)imm, a);
        return true;
    }
    if (size == 1 && !byte_register(gas, b_phys)) {
        gas->failed = true;
        return true;
    }
    anvil_strbuf_appendf(&gas->code, "\tcmp%s %%%s, %%%s\n", suffix, x86_gas_gpr(b_phys, size), a);
    return true;
}

void x86_gas_emit_cmp(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info)
{
    if (index == gas->test_at) {
        /* The test before it set the flags. */
        gas->test_at = SIZE_MAX;
        gas->fused_jcc = x86_gas_jcc(info->op);
        return;
    }
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(gas->mir, index, 0), rhs = anvil_mir_get_instr_use(gas->mir, index, 1);
    const anvil_mir_vreg_info_t *lhs_info = lhs == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_vreg_info(gas->mir, lhs);
    if (!lhs_info || rhs == ANVIL_MIR_NO_VREG) {
        gas->failed = true;
        return;
    }
    int dst_phys = x86_gas_phys_of(gas, info->def);
    if (gas->failed)
        return;
    /* Booleans live in the byte registers (the allocator's narrow ones). */
    const char *dst8 = byte_register(gas, dst_phys) ? x86_gas_gpr(dst_phys, 1) : NULL;
    const char *dst32 = x86_gas_gpr(dst_phys, 4);
    bool fuses = x86_gas_fuses(gas, index, info);
    if (!dst8 && !fuses) {
        gas->failed = true;
        return;
    }
    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        /* A branch right after tests the predicates one condition decides
         * (x86_gas_fcmp_branch); OEQ and UNE stay materialized. */
        const char *branch_cc;
        bool swap;
        if (info->op == ANVIL_MIR_OP_FCMP && fuses && x86_gas_fcmp_branch((anvil_fcmp_pred_t)info->imm, &branch_cc, &swap)) {
            gas->fp_compare(gas, swap ? rhs : lhs, swap ? lhs : rhs);
            gas->fused_jcc = branch_cc;
            return;
        }
        if (!dst8) {
            gas->failed = true;
            return;
        }
        gas->fp_compare(gas, lhs, rhs);
        if (!gas->failed)
            emit_setcc(gas, index, info, dst8, dst32);
        return;
    }
    if (!integer_compare(gas, index, info, lhs_info, fuses) || fuses) {
        /* Consumed only by the branch or select right after it: the flags. */
        if (!gas->failed)
            gas->fused_jcc = x86_gas_jcc(info->op);
        return;
    }
    if (gas->failed)
        return;
    /* x + (a u< b): the carry of the compare is the condition (an add at
     * least the width operations compute at). */
    size_t extension = SIZE_MAX, add = SIZE_MAX;
    anvil_mir_vreg_t other = ANVIL_MIR_NO_VREG;
    int64_t constant;
    const anvil_mir_vreg_info_t *other_info = NULL;
    if (info->op == ANVIL_MIR_OP_CMP_ULT && anvil_mir_isel_condition_add(gas->mir, &gas->isel, index, &extension, &add, &other) &&
        !anvil_mir_isel_constant(&gas->isel, other, &constant) && (other_info = anvil_mir_get_vreg_info(gas->mir, other)) &&
        other_info->size_bits >= gas->min_operation_bytes * 8) {
        gas->adc_skip = extension;
        gas->adc_at = add;
        gas->adc_other = other;
        return;
    }
    emit_setcc(gas, index, info, dst8, dst32);
}

/* The instruction a load folded into: its other operand first, then the
 * operation from memory. */
static void emit_folded_binary(x86_gas_t *gas, const anvil_mir_instr_info_t *info, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs,
                               const anvil_mir_vreg_info_t *def_info)
{
    gas->fold_target = SIZE_MAX;
    anvil_mir_instr_info_t load;
    if (!anvil_mir_get_instr_info(gas->mir, gas->fold_load, &load)) {
        gas->failed = true;
        return;
    }
    anvil_mir_vreg_t other = rhs == load.def ? lhs : rhs;
    bool fpr = def_info->reg_class == ANVIL_MIR_REG_FPR;
    int size = (def_info->size_bits + 7) / 8;
    int width = fpr || size >= gas->min_operation_bytes ? size : gas->min_operation_bytes;
    const char *op = info->op == ANVIL_MIR_OP_ADD ? "add" : info->op == ANVIL_MIR_OP_SUB ? "sub" : info->op == ANVIL_MIR_OP_MUL ? (fpr ? "mul" : "imul")
                   : info->op == ANVIL_MIR_OP_AND ? "and" : info->op == ANVIL_MIR_OP_OR ? "or" : info->op == ANVIL_MIR_OP_XOR ? "xor" : "div";
    const char *suffix = fpr ? (size <= 4 ? "ss" : "sd") : x86_gas_size_suffix(width);
    int dst_phys = x86_gas_phys_of(gas, info->def), keep_phys = x86_gas_phys_of(gas, other);
    const char *dst = fpr ? x86_gas_xmm(dst_phys) : x86_gas_gpr(dst_phys, width);
    const char *keep = fpr ? x86_gas_xmm(keep_phys) : x86_gas_gpr(keep_phys, width);
    if (gas->failed)
        return;
    if (dst_phys != keep_phys)
        anvil_strbuf_appendf(&gas->code, "\t%s%s %%%s, %%%s\n", fpr ? "movaps" : "mov", fpr ? "" : suffix, keep, dst);
    anvil_strbuf_appendf(&gas->code, "\t%s%s %s, %%%s\n", op, suffix, gas->fold_address, dst);
    if (!fpr && info->op != ANVIL_MIR_OP_MUL)
        anvil_print_flags_set(&gas->flags, dst_phys, width);
}

/* x & c only tested against zero by the next branch: test, which keeps x
 * and writes no register (a mask within a narrow value's own bits when
 * the test is wider). */
static bool emit_and_as_test(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, int a_phys, int size, int width)
{
    int64_t mask, zero = 1;
    if (info->op != ANVIL_MIR_OP_AND || gas->test_at != SIZE_MAX || !x86_gas_known_imm(gas, anvil_mir_get_instr_use(gas->mir, index, 1), &mask) ||
        (size < width && ((uint64_t)mask >> (8 * size)) != 0))
        return false;
    size_t reader = anvil_mir_isel_fold_reader(gas->mir, &gas->isel, index, 4, false);
    anvil_mir_instr_info_t next;
    if (reader == SIZE_MAX || !anvil_mir_get_instr_info(gas->mir, reader, &next) || (next.op != ANVIL_MIR_OP_CMP_EQ && next.op != ANVIL_MIR_OP_CMP_NE) ||
        next.num_uses != 2 || !x86_gas_known_imm(gas, anvil_mir_get_instr_use(gas->mir, reader, 1), &zero) || zero != 0 || !x86_gas_fuses(gas, reader, &next))
        return false;
    anvil_strbuf_appendf(&gas->code, "\ttest%s $%lld, %%%s\n", x86_gas_size_suffix(width), (long long)mask, x86_gas_gpr(a_phys, width));
    gas->test_at = reader;
    return true;
}

void x86_gas_emit_binary(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(gas->mir, index, 0), rhs = anvil_mir_get_instr_use(gas->mir, index, 1);
    const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(gas->mir, info->def);
    if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG || !def_info) {
        gas->failed = true;
        return;
    }
    if (index == gas->fold_target) {
        emit_folded_binary(gas, info, lhs, rhs, def_info);
        return;
    }
    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        gas->fp_binary(gas, info->op, info->def, lhs, rhs);
        return;
    }
    int size = (def_info->size_bits + 7) / 8;
    int width = size < gas->min_operation_bytes ? gas->min_operation_bytes : size;
    int dst_phys = x86_gas_phys_of(gas, info->def), a_phys = x86_gas_phys_of(gas, lhs), b_phys = x86_gas_phys_of(gas, rhs);
    if (gas->failed || emit_and_as_test(gas, index, info, a_phys, size, width) || x86_gas_emit_gpr_arith(gas, index, info, dst_phys, a_phys, b_phys, size))
        return;
    gas->gpr_divide(gas, index, info, dst_phys, a_phys, b_phys, size);
}

void x86_gas_emit_copy(x86_gas_t *gas, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = anvil_mir_get_vreg_info(gas->mir, dst), *src_info = anvil_mir_get_vreg_info(gas->mir, src);
    if (!dst_info || !src_info) {
        gas->failed = true;
        return;
    }
    if (dst_info->reg_class == ANVIL_MIR_REG_FPR || src_info->reg_class == ANVIL_MIR_REG_FPR) {
        gas->fp_copy(gas, dst, src);
        return;
    }
    int dst_phys = x86_gas_phys_of(gas, dst), src_phys = x86_gas_phys_of(gas, src);
    if (gas->failed || dst_phys == src_phys)
        return;
    int bytes = (dst_info->size_bits + 7) / 8;
    if (bytes < gas->min_operation_bytes)
        bytes = gas->min_operation_bytes;
    anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n", x86_gas_size_suffix(bytes), x86_gas_gpr(src_phys, bytes), x86_gas_gpr(dst_phys, bytes));
}

void x86_gas_emit_mov(x86_gas_t *gas, const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(gas->mir, info->def);
    if (!def_info) {
        gas->failed = true;
        return;
    }
    if (!anvil_mir_isel_materialize(&gas->isel, info->def))
        return;
    int dst_phys = x86_gas_phys_of(gas, info->def);
    if (gas->failed)
        return;
    int64_t imm = info->has_imm ? info->imm : 0;
    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        gas->fp_constant(gas, info->def, (uint64_t)imm);
        return;
    }
    /* The shortest form: xor for zero (the flags are dead here: a compare
     * fused with its consumer is adjacent to it), movl for a value of 32
     * bits or one that fits 32 unsigned bits (it zero-extends), movq for a
     * signed 32-bit immediate, movabsq otherwise. */
    const char *dst32 = x86_gas_gpr(dst_phys, 4);
    if (def_info->size_bits <= 32)
        imm = (int32_t)imm;
    if (imm == 0)
        anvil_strbuf_appendf(&gas->code, "\txorl %%%s, %%%s\n", dst32, dst32);
    else if (def_info->size_bits <= 32 || (imm > 0 && imm <= (int64_t)UINT32_MAX))
        anvil_strbuf_appendf(&gas->code, "\tmovl $%lld, %%%s\n", (long long)imm, dst32);
    else if (imm >= INT32_MIN && imm < 0)
        anvil_strbuf_appendf(&gas->code, "\tmovq $%lld, %%%s\n", (long long)imm, x86_gas_gpr(dst_phys, 8));
    else
        anvil_strbuf_appendf(&gas->code, "\tmovabsq $%lld, %%%s\n", (long long)imm, x86_gas_gpr(dst_phys, 8));
}

void x86_gas_emit_unary(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(gas->mir, index, 0);
    const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(gas->mir, info->def);
    if (src == ANVIL_MIR_NO_VREG || !def_info) {
        gas->failed = true;
        return;
    }
    if (def_info->reg_class == ANVIL_MIR_REG_FPR && (info->op == ANVIL_MIR_OP_NEG || info->op == ANVIL_MIR_OP_FABS)) {
        gas->fp_unary(gas, info->op, info->def, src);
        return;
    }
    if (def_info->reg_class != ANVIL_MIR_REG_GPR || (info->op != ANVIL_MIR_OP_NEG && info->op != ANVIL_MIR_OP_NOT)) {
        gas->failed = true;
        return;
    }
    int bytes = (def_info->size_bits + 7) / 8;
    if (bytes < gas->min_operation_bytes)
        bytes = gas->min_operation_bytes;
    const char *suffix = x86_gas_size_suffix(bytes);
    const char *dst = x86_gas_gpr(x86_gas_phys_of(gas, info->def), bytes), *value = x86_gas_gpr(x86_gas_phys_of(gas, src), bytes);
    if (!gas->failed)
        anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n\t%s%s %%%s\n", suffix, value, dst, info->op == ANVIL_MIR_OP_NEG ? "neg" : "not", suffix, dst);
}

/* Operations whose 32-bit form zeroes the upper half of the register on
 * x86-64. */
static bool writes_zero_extended(anvil_mir_opcode_t op)
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

static void move32_if_needed(x86_gas_t *gas, int src_phys, int dst_phys)
{
    if (src_phys != dst_phys)
        anvil_strbuf_appendf(&gas->code, "\tmovl %%%s, %%%s\n", x86_gas_gpr(src_phys, 4), x86_gas_gpr(dst_phys, 4));
}

void x86_gas_emit_gpr_extend(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info, bool sign_extend)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(gas->mir, index, 0);
    const anvil_mir_vreg_info_t *dst_info = anvil_mir_get_vreg_info(gas->mir, info->def);
    const anvil_mir_vreg_info_t *src_info = src == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_vreg_info(gas->mir, src);
    int dst_phys = x86_gas_phys_of(gas, info->def), src_phys = x86_gas_phys_of(gas, src);
    if (!dst_info || !src_info || gas->failed || dst_info->reg_class != ANVIL_MIR_REG_GPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        gas->failed = true;
        return;
    }
    int src_size = (src_info->size_bits + 7) / 8, dst_size = (dst_info->size_bits + 7) / 8;
    if (dst_size < src_size)
        dst_size = src_size;
    if (dst_size < gas->min_operation_bytes)
        dst_size = gas->min_operation_bytes;
    /* The whole register: a copy. */
    if (src_size == dst_size && dst_size >= gas->pointer_bytes) {
        if (src_phys != dst_phys)
            anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n", x86_gas_size_suffix(dst_size), x86_gas_gpr(src_phys, dst_size), x86_gas_gpr(dst_phys, dst_size));
        return;
    }
    /* A narrow load of the same signedness already extended the value
     * (x86_gas_emit_load: movs*l to 32 bits, movz*l to 64), and a value
     * known zero above its width needs no zero extension. */
    if ((src_size < 4 && src_info->is_signed == sign_extend && (!sign_extend || dst_size <= 4) &&
         anvil_mir_isel_def_op(&gas->isel, src) == ANVIL_MIR_OP_LOAD) ||
        (!sign_extend && src_size <= 4 && anvil_mir_isel_zero_extended(&gas->isel, src))) {
        move32_if_needed(gas, src_phys, dst_phys);
        return;
    }
    if (sign_extend) {
        if (src_size == dst_size) {
            anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n", x86_gas_size_suffix(dst_size), x86_gas_gpr(src_phys, dst_size), x86_gas_gpr(dst_phys, dst_size));
            return;
        }
        const char *suffix = src_size == 1 ? (dst_size <= 4 ? "bl" : "bq") : src_size == 2 ? (dst_size <= 4 ? "wl" : "wq") : "lq";
        if (src_size == 1 && gas->has_byte && !gas->has_byte(src_phys)) {
            gas->failed = true;
            return;
        }
        anvil_strbuf_appendf(&gas->code, "\tmovs%s %%%s, %%%s\n", suffix, x86_gas_gpr(src_phys, src_size), x86_gas_gpr(dst_phys, dst_size));
        return;
    }
    if (src_size >= 4) {
        /* A 32-bit operation already zeroed the upper half (an omitted
         * TRUNC or COPY in the same register did not). */
        if (src_phys != dst_phys || !writes_zero_extended(anvil_mir_isel_def_op(&gas->isel, src)))
            anvil_strbuf_appendf(&gas->code, "\tmovl %%%s, %%%s\n", x86_gas_gpr(src_phys, 4), x86_gas_gpr(dst_phys, 4));
        return;
    }
    if (src_size == 1 && gas->has_byte && !gas->has_byte(src_phys)) {
        gas->failed = true;
        return;
    }
    anvil_strbuf_appendf(&gas->code, "\tmovz%s %%%s, %%%s\n", src_size == 1 ? "bl" : "wl", x86_gas_gpr(src_phys, src_size), x86_gas_gpr(dst_phys, 4));
}

/* The add of a compare's carry (x + (a u< b)): mov keeps the carry. */
static void emit_add_carry(x86_gas_t *gas, const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(gas->mir, info->def);
    int to = x86_gas_phys_of(gas, info->def), from = x86_gas_phys_of(gas, gas->adc_other);
    if (!def || gas->failed) {
        gas->failed = true;
        return;
    }
    int bytes = (def->size_bits + 7) / 8 < 4 ? 4 : (def->size_bits + 7) / 8;
    if (to != from)
        anvil_strbuf_appendf(&gas->code, "\tmov%s %%%s, %%%s\n", x86_gas_size_suffix(bytes), x86_gas_gpr(from, bytes), x86_gas_gpr(to, bytes));
    anvil_strbuf_appendf(&gas->code, "\tadc%s $0, %%%s\n", x86_gas_size_suffix(bytes), x86_gas_gpr(to, bytes));
}

static void emit_cond_branch(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info)
{
    const char *cc = gas->fused_jcc;
    gas->fused_jcc = NULL;
    if (!cc) {
        anvil_mir_vreg_t cond = anvil_mir_get_instr_use(gas->mir, index, 0);
        const anvil_mir_vreg_info_t *cond_info = cond == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_vreg_info(gas->mir, cond);
        int cond_phys = cond_info ? x86_gas_phys_of(gas, cond) : -1;
        if (!cond_info || gas->failed) {
            gas->failed = true;
            return;
        }
        /* A boolean tests in its byte register. */
        int bytes = byte_register(gas, cond_phys) ? (cond_info->size_bits + 7) / 8 : 4;
        const char *reg = x86_gas_gpr(cond_phys, bytes);
        anvil_strbuf_appendf(&gas->code, "\ttest%s %%%s, %%%s\n", x86_gas_size_suffix(bytes), reg, reg);
        cc = "ne";
    }
    gas->failed |= !x86_gas_emit_cond_branch(&gas->code, gas->local_label_prefix, gas->mir, &gas->layout, gas->block, cc, info->true_block, info->false_block);
}

void x86_gas_emit_instr(x86_gas_t *gas, size_t index, const anvil_mir_instr_info_t *info)
{
    if (index == gas->adc_skip) {
        gas->adc_skip = SIZE_MAX;
        return;
    }
    if (index == gas->adc_at) {
        gas->adc_at = SIZE_MAX;
        emit_add_carry(gas, info);
        return;
    }
    if (!anvil_mir_isel_registerless_ok(gas->mir, &gas->isel, index, info)) {
        gas->failed = true;
        return;
    }
    switch (info->op) {
    case ANVIL_MIR_OP_MOV:
        x86_gas_emit_mov(gas, info);
        break;
    case ANVIL_MIR_OP_COPY: {
        anvil_mir_vreg_t src = anvil_mir_get_instr_use(gas->mir, index, 0);
        if (src == ANVIL_MIR_NO_VREG)
            gas->failed = true;
        else
            x86_gas_emit_copy(gas, info->def, src);
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
        x86_gas_emit_binary(gas, index, info);
        break;
    case ANVIL_MIR_OP_NEG:
    case ANVIL_MIR_OP_NOT:
    case ANVIL_MIR_OP_FABS:
        x86_gas_emit_unary(gas, index, info);
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
        x86_gas_emit_cmp(gas, index, info);
        break;
    case ANVIL_MIR_OP_LOAD:
        x86_gas_emit_load(gas, index, info);
        break;
    case ANVIL_MIR_OP_STORE:
        x86_gas_emit_store(gas, index, info);
        break;
    case ANVIL_MIR_OP_FRAME_ADDR:
        x86_gas_emit_frame_addr(gas, info);
        break;
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        x86_gas_emit_call_stack_arg(gas, index, info);
        break;
    case ANVIL_MIR_OP_BR:
        gas->failed |= !x86_gas_emit_jump(&gas->code, gas->local_label_prefix, gas->mir, &gas->layout, gas->block, info->true_block);
        break;
    case ANVIL_MIR_OP_SELECT:
        x86_gas_emit_select(gas, index, info);
        break;
    case ANVIL_MIR_OP_BR_COND:
        emit_cond_branch(gas, index, info);
        break;
    case ANVIL_MIR_OP_SPILL_LOAD:
        x86_gas_emit_spill_load(gas, info);
        break;
    case ANVIL_MIR_OP_SPILL_STORE:
        x86_gas_emit_spill_store(gas, index, info);
        break;
    case ANVIL_MIR_OP_KEEPALIVE:
        break;
    default:
        if (!gas->target_instr(gas, index, info))
            gas->failed = true;
        break;
    }
}
