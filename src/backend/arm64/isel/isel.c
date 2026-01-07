#include "isel.h"
#include "../regs.h"
#include <string.h>

static bool match_mul_power_of_2(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return anvil_isel_match_power_of_2(inst->operands[1].imm.value, NULL);
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

static bool match_mov_zero(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return inst->operands[1].imm.value == 0;
}

static void emit_mov_zero(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    match->inst->operands[1] = anvil_mop_preg(ARM64_XZR, match->inst->operands[0].size);
}

static bool match_add_imm_12bit(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    int64_t val = inst->operands[1].imm.value;
    return val >= 0 && val <= 4095;
}

static bool match_sub_neg_to_add(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    int64_t val = inst->operands[1].imm.value;
    return val < 0 && (-val) <= 4095;
}

static void emit_sub_neg_to_add(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    match->inst->kind = ANVIL_MIR_ADD;
    match->inst->operands[1] = anvil_mop_imm(-match->inst->operands[1].imm.value, match->inst->operands[1].size);
}

static bool match_mul_by_neg_1(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return inst->operands[1].imm.value == -1;
}

static void emit_mul_by_neg_1(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    (void)ctx;
    (void)output;
    match->inst->kind = ANVIL_MIR_NEG;
    match->inst->num_operands = 1;
}

static const AnvilISelRule arm64_rules[] = {
    { "mul_power_of_2", ANVIL_MIR_MUL, match_mul_power_of_2, emit_mul_power_of_2, NULL, 1 },
    { "mul_by_neg_1", ANVIL_MIR_MUL, match_mul_by_neg_1, emit_mul_by_neg_1, NULL, 1 },
    { "div_power_of_2", ANVIL_MIR_DIV, match_div_power_of_2, emit_div_power_of_2, NULL, 1 },
    { "mod_power_of_2", ANVIL_MIR_MOD, match_mod_power_of_2, emit_mod_power_of_2, NULL, 1 },
    { "mov_zero", ANVIL_MIR_MOV, match_mov_zero, emit_mov_zero, NULL, 1 },
    { "sub_neg_to_add", ANVIL_MIR_SUB, match_sub_neg_to_add, emit_sub_neg_to_add, NULL, 2 },
};

const AnvilISelRuleSet arm64_isel_ruleset = {
    .rules = arm64_rules,
    .num_rules = sizeof(arm64_rules) / sizeof(arm64_rules[0]),
};

void arm64_isel_run(AnvilBackend* backend, AnvilMFunc* func) {
    (void)backend;
    AnvilISelContext ctx;
    anvil_isel_init(&ctx, func, &arm64_isel_ruleset);
    anvil_isel_run(&ctx);
}
