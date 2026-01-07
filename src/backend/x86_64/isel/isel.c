#include "isel.h"
#include "../regs.h"
#include <string.h>

static bool match_mul_by_2(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return inst->operands[1].imm.value == 2;
}

static void emit_mul_by_2(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    match->inst->kind = ANVIL_MIR_ADD;
    match->inst->operands[1] = match->inst->operands[0];
}

static bool match_mul_by_3_5_9(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    int64_t val = inst->operands[1].imm.value;
    return val == 3 || val == 5 || val == 9;
}

static void emit_mul_by_3_5_9(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    int64_t val = match->inst->operands[1].imm.value;
    int scale = (val == 3) ? 2 : (val == 5) ? 4 : 8;
    
    match->inst->kind = ANVIL_MIR_LEA;
    AnvilMOperand op0 = match->inst->operands[0];
    match->inst->operands[1].kind = ANVIL_MOP_MEM;
    match->inst->operands[1].mem.base_reg = op0.preg.id;
    match->inst->operands[1].mem.index_reg = op0.preg.id;
    match->inst->operands[1].mem.scale = scale;
    match->inst->operands[1].mem.disp = 0;
    match->inst->operands[1].size = op0.size;
}

static bool match_mul_power_of_2(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    int64_t val = inst->operands[1].imm.value;
    if (val == 2 || val == 3 || val == 5 || val == 9) return false;
    return anvil_isel_match_power_of_2(val, NULL);
}

static void emit_mul_power_of_2(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    int shift;
    anvil_isel_match_power_of_2(match->inst->operands[1].imm.value, &shift);
    
    match->inst->kind = ANVIL_MIR_SHL;
    match->inst->operands[1] = anvil_mop_imm(shift, match->inst->operands[1].size);
}

static bool match_div_power_of_2(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return anvil_isel_match_power_of_2(inst->operands[1].imm.value, NULL);
}

static void emit_div_power_of_2(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    int shift;
    anvil_isel_match_power_of_2(match->inst->operands[1].imm.value, &shift);
    
    match->inst->kind = ANVIL_MIR_SHR;
    match->inst->operands[1] = anvil_mop_imm(shift, match->inst->operands[1].size);
}

static bool match_add_to_lea(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[0].kind != ANVIL_MOP_PREG) return false;
    if (inst->operands[1].kind != ANVIL_MOP_PREG) return false;
    return inst->operands[0].preg.id != inst->operands[1].preg.id;
}

static void emit_add_to_lea(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    AnvilMOperand op0 = match->inst->operands[0];
    AnvilMOperand op1 = match->inst->operands[1];
    
    match->inst->kind = ANVIL_MIR_LEA;
    match->inst->operands[1].kind = ANVIL_MOP_MEM;
    match->inst->operands[1].mem.base_reg = op0.preg.id;
    match->inst->operands[1].mem.index_reg = op1.preg.id;
    match->inst->operands[1].mem.scale = 1;
    match->inst->operands[1].mem.disp = 0;
    match->inst->operands[1].size = op0.size;
}

static bool match_mov_zero(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return inst->operands[1].imm.value == 0;
}

static void emit_mov_zero(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    match->inst->kind = ANVIL_MIR_XOR;
    match->inst->operands[1] = match->inst->operands[0];
}

static bool match_sub_to_neg(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return inst->operands[1].imm.value < 0 && inst->operands[1].imm.value > -128;
}

static void emit_sub_to_neg(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    match->inst->kind = ANVIL_MIR_ADD;
    match->inst->operands[1] = anvil_mop_imm(-match->inst->operands[1].imm.value, match->inst->operands[1].size);
}

static bool match_mod_power_of_2(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return anvil_isel_match_power_of_2(inst->operands[1].imm.value, NULL);
}

static void emit_mod_power_of_2(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    int64_t val = match->inst->operands[1].imm.value;
    
    match->inst->kind = ANVIL_MIR_AND;
    match->inst->operands[1] = anvil_mop_imm(val - 1, match->inst->operands[1].size);
}

static bool match_mov_fp(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    (void)match;
    if (inst->num_operands < 2) return false;
    return inst->operands[0].is_fp && inst->operands[1].is_fp;
}

static void emit_mov_fp(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    if (match->inst->operands[0].size == 4) {
        match->inst->kind = ANVIL_MIR_MOVSS;
    } else {
        match->inst->kind = ANVIL_MIR_MOVSD;
    }
}

static bool match_call_indirect(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    (void)match;
    if (inst->num_operands < 1) return false;
    return inst->operands[0].kind == ANVIL_MOP_PREG || inst->operands[0].kind == ANVIL_MOP_VREG;
}

static void emit_call_indirect(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    match->inst->kind = ANVIL_MIR_CALL_INDIRECT;
}

static bool match_tail_call(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    (void)match;
    if (inst->num_operands < 1) return false;
    return inst->is_tail_call;
}

static void emit_tail_call(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    match->inst->kind = ANVIL_MIR_TAIL_CALL;
}

static bool match_fmadd(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (!inst->operands[0].is_fp) return false;
    
    AnvilMInst* prev = inst->prev;
    if (!prev || prev->kind != ANVIL_MIR_FMUL) return false;
    if (prev->num_operands < 2) return false;
    
    if (prev->operands[0].kind == ANVIL_MOP_PREG &&
        inst->operands[1].kind == ANVIL_MOP_PREG &&
        prev->operands[0].preg.id == inst->operands[1].preg.id) {
        match->matched_insts[0] = prev;
        match->num_matched = 1;
        return true;
    }
    return false;
}

static void emit_fmadd(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    AnvilMInst* fmul = match->matched_insts[0];
    AnvilMInst* fadd = match->inst;
    
    fadd->kind = ANVIL_MIR_FMADD;
    fadd->operands[1] = fmul->operands[0];
    fadd->operands[2] = fmul->operands[1];
    if (fadd->num_operands < 4) {
        fadd->operands[3] = fadd->operands[0];
        fadd->num_operands = 4;
    }
    
    fmul->kind = ANVIL_MIR_NOP;
    fmul->num_operands = 0;
}

static bool match_cmov_pattern(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    (void)match;
    if (inst->num_operands < 2) return false;
    
    AnvilMInst* prev = inst->prev;
    if (!prev || prev->kind != ANVIL_MIR_JCC) return false;
    
    return false;
}

static void emit_cmov_pattern(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)match;
    (void)output;
}

static const AnvilISelRule x86_64_rules[] = {
    { "mul_by_2", ANVIL_MIR_MUL, match_mul_by_2, emit_mul_by_2, NULL, 1 },
    { "mul_by_3_5_9", ANVIL_MIR_MUL, match_mul_by_3_5_9, emit_mul_by_3_5_9, NULL, 2 },
    { "mul_power_of_2", ANVIL_MIR_MUL, match_mul_power_of_2, emit_mul_power_of_2, NULL, 3 },
    { "div_power_of_2", ANVIL_MIR_DIV, match_div_power_of_2, emit_div_power_of_2, NULL, 1 },
    { "mod_power_of_2", ANVIL_MIR_MOD, match_mod_power_of_2, emit_mod_power_of_2, NULL, 1 },
    { "add_to_lea", ANVIL_MIR_ADD, match_add_to_lea, emit_add_to_lea, NULL, 5 },
    { "mov_zero", ANVIL_MIR_MOV, match_mov_zero, emit_mov_zero, NULL, 1 },
    { "mov_fp", ANVIL_MIR_MOV, match_mov_fp, emit_mov_fp, NULL, 10 },
    { "sub_neg_to_add", ANVIL_MIR_SUB, match_sub_to_neg, emit_sub_to_neg, NULL, 2 },
    { "call_indirect", ANVIL_MIR_CALL, match_call_indirect, emit_call_indirect, NULL, 5 },
    { "tail_call", ANVIL_MIR_CALL, match_tail_call, emit_tail_call, NULL, 10 },
    { "fmadd", ANVIL_MIR_FADD, match_fmadd, emit_fmadd, NULL, 15 },
};

const AnvilISelRuleSet x86_64_isel_ruleset = {
    .rules = x86_64_rules,
    .num_rules = sizeof(x86_64_rules) / sizeof(x86_64_rules[0]),
};

void x86_64_isel_run(AnvilBackend* backend, AnvilMFunc* func) {
    (void)backend;
    AnvilISelContext ctx;
    anvil_isel_init(&ctx, func, &x86_64_isel_ruleset);
    anvil_isel_run(&ctx);
}
