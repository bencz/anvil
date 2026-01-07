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
    int shift;
    anvil_isel_match_power_of_2(match->inst->operands[1].imm.value, &shift);
    
    AnvilMInst* sldi = anvil_isel_create_inst(ctx, ANVIL_MIR_SHL);
    sldi->operands[0] = match->inst->operands[0];
    sldi->operands[1] = anvil_mop_imm(shift, match->inst->operands[1].size);
    sldi->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = sldi;
}

static bool match_div_power_of_2(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return anvil_isel_match_power_of_2(inst->operands[1].imm.value, NULL);
}

static void emit_div_power_of_2(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    int shift;
    anvil_isel_match_power_of_2(match->inst->operands[1].imm.value, &shift);
    
    AnvilMInst* srdi = anvil_isel_create_inst(ctx, ANVIL_MIR_SHR);
    srdi->operands[0] = match->inst->operands[0];
    srdi->operands[1] = anvil_mop_imm(shift, match->inst->operands[1].size);
    srdi->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = srdi;
}

static bool match_mod_power_of_2(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return anvil_isel_match_power_of_2(inst->operands[1].imm.value, NULL);
}

static void emit_mod_power_of_2(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    int64_t val = match->inst->operands[1].imm.value;
    
    AnvilMInst* andi = anvil_isel_create_inst(ctx, ANVIL_MIR_AND);
    andi->operands[0] = match->inst->operands[0];
    andi->operands[1] = anvil_mop_imm(val - 1, match->inst->operands[1].size);
    andi->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = andi;
}

static bool match_mov_zero(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return inst->operands[1].imm.value == 0;
}

static void emit_mov_zero(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    AnvilMInst* li = anvil_isel_create_inst(ctx, ANVIL_MIR_MOV);
    li->operands[0] = match->inst->operands[0];
    li->operands[1] = anvil_mop_imm(0, match->inst->operands[0].size);
    li->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = li;
}

static bool match_add_small_imm(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    int64_t val = inst->operands[1].imm.value;
    return val >= -32768 && val <= 32767;
}

static bool match_sub_neg_to_add(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    int64_t val = inst->operands[1].imm.value;
    return val < 0 && (-val) <= 32767;
}

static void emit_sub_neg_to_add(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    AnvilMInst* addi = anvil_isel_create_inst(ctx, ANVIL_MIR_ADD);
    addi->operands[0] = match->inst->operands[0];
    addi->operands[1] = anvil_mop_imm(-match->inst->operands[1].imm.value, match->inst->operands[1].size);
    addi->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = addi;
}

static bool match_mul_by_neg_1(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return inst->operands[1].imm.value == -1;
}

static void emit_mul_by_neg_1(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    AnvilMInst* neg = anvil_isel_create_inst(ctx, ANVIL_MIR_NEG);
    neg->operands[0] = match->inst->operands[0];
    neg->num_operands = 1;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = neg;
}

static const AnvilISelRule ppc64_rules[] = {
    { "mul_power_of_2", ANVIL_MIR_MUL, match_mul_power_of_2, emit_mul_power_of_2, NULL, 1 },
    { "mul_by_neg_1", ANVIL_MIR_MUL, match_mul_by_neg_1, emit_mul_by_neg_1, NULL, 1 },
    { "div_power_of_2", ANVIL_MIR_DIV, match_div_power_of_2, emit_div_power_of_2, NULL, 1 },
    { "mod_power_of_2", ANVIL_MIR_MOD, match_mod_power_of_2, emit_mod_power_of_2, NULL, 1 },
    { "mov_zero", ANVIL_MIR_MOV, match_mov_zero, emit_mov_zero, NULL, 1 },
    { "sub_neg_to_add", ANVIL_MIR_SUB, match_sub_neg_to_add, emit_sub_neg_to_add, NULL, 2 },
};

const AnvilISelRuleSet ppc64_isel_ruleset = {
    .rules = ppc64_rules,
    .num_rules = sizeof(ppc64_rules) / sizeof(ppc64_rules[0]),
};

void ppc64_isel_run(AnvilBackend* backend, AnvilMFunc* func) {
    (void)backend;
    AnvilISelContext ctx;
    anvil_isel_init(&ctx, func, &ppc64_isel_ruleset);
    anvil_isel_run(&ctx);
}
