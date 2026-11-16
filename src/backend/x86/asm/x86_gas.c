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
