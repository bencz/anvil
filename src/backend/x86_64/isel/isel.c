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
    AnvilMInst* add = anvil_isel_create_inst(ctx, ANVIL_MIR_ADD);
    add->operands[0] = match->inst->operands[0];
    add->operands[1] = match->inst->operands[0];
    add->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = add;
}

static bool match_mul_by_3_5_9(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    int64_t val = inst->operands[1].imm.value;
    return val == 3 || val == 5 || val == 9;
}

static void emit_mul_by_3_5_9(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    int64_t val = match->inst->operands[1].imm.value;
    int scale = (val == 3) ? 2 : (val == 5) ? 4 : 8;
    
    AnvilMInst* lea = anvil_isel_create_inst(ctx, ANVIL_MIR_LEA);
    lea->operands[0] = match->inst->operands[0];
    lea->operands[1].kind = ANVIL_MOP_MEM;
    lea->operands[1].mem.base_reg = match->inst->operands[0].preg.id;
    lea->operands[1].mem.index_reg = match->inst->operands[0].preg.id;
    lea->operands[1].mem.scale = scale;
    lea->operands[1].mem.disp = 0;
    lea->operands[1].size = match->inst->operands[0].size;
    lea->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = lea;
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
    int shift;
    anvil_isel_match_power_of_2(match->inst->operands[1].imm.value, &shift);
    
    AnvilMInst* shl = anvil_isel_create_inst(ctx, ANVIL_MIR_SHL);
    shl->operands[0] = match->inst->operands[0];
    shl->operands[1] = anvil_mop_imm(shift, match->inst->operands[1].size);
    shl->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = shl;
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
    
    AnvilMInst* shr = anvil_isel_create_inst(ctx, ANVIL_MIR_SHR);
    shr->operands[0] = match->inst->operands[0];
    shr->operands[1] = anvil_mop_imm(shift, match->inst->operands[1].size);
    shr->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = shr;
}

static bool match_add_to_lea(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[0].kind != ANVIL_MOP_PREG) return false;
    if (inst->operands[1].kind != ANVIL_MOP_PREG) return false;
    return inst->operands[0].preg.id != inst->operands[1].preg.id;
}

static void emit_add_to_lea(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    AnvilMInst* lea = anvil_isel_create_inst(ctx, ANVIL_MIR_LEA);
    lea->operands[0] = match->inst->operands[0];
    lea->operands[1].kind = ANVIL_MOP_MEM;
    lea->operands[1].mem.base_reg = match->inst->operands[0].preg.id;
    lea->operands[1].mem.index_reg = match->inst->operands[1].preg.id;
    lea->operands[1].mem.scale = 1;
    lea->operands[1].mem.disp = 0;
    lea->operands[1].size = match->inst->operands[0].size;
    lea->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = lea;
}

static bool match_mov_zero(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return inst->operands[1].imm.value == 0;
}

static void emit_mov_zero(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    AnvilMInst* xor = anvil_isel_create_inst(ctx, ANVIL_MIR_XOR);
    xor->operands[0] = match->inst->operands[0];
    xor->operands[1] = match->inst->operands[0];
    xor->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = xor;
}

static bool match_sub_to_neg(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return inst->operands[1].imm.value < 0 && inst->operands[1].imm.value > -128;
}

static void emit_sub_to_neg(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    AnvilMInst* add = anvil_isel_create_inst(ctx, ANVIL_MIR_ADD);
    add->operands[0] = match->inst->operands[0];
    add->operands[1] = anvil_mop_imm(-match->inst->operands[1].imm.value, match->inst->operands[1].size);
    add->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = add;
}

static bool match_mod_power_of_2(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    (void)ctx;
    if (inst->num_operands < 2) return false;
    if (inst->operands[1].kind != ANVIL_MOP_IMM) return false;
    return anvil_isel_match_power_of_2(inst->operands[1].imm.value, NULL);
}

static void emit_mod_power_of_2(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output) {
    int64_t val = match->inst->operands[1].imm.value;
    
    AnvilMInst* and = anvil_isel_create_inst(ctx, ANVIL_MIR_AND);
    and->operands[0] = match->inst->operands[0];
    and->operands[1] = anvil_mop_imm(val - 1, match->inst->operands[1].size);
    and->num_operands = 2;
    
    AnvilMInst** slot = (AnvilMInst**)anvil_vec_push(output);
    *slot = and;
}

static const AnvilISelRule x86_64_rules[] = {
    { "mul_by_2", ANVIL_MIR_MUL, match_mul_by_2, emit_mul_by_2, NULL, 1 },
    { "mul_by_3_5_9", ANVIL_MIR_MUL, match_mul_by_3_5_9, emit_mul_by_3_5_9, NULL, 2 },
    { "mul_power_of_2", ANVIL_MIR_MUL, match_mul_power_of_2, emit_mul_power_of_2, NULL, 3 },
    { "div_power_of_2", ANVIL_MIR_DIV, match_div_power_of_2, emit_div_power_of_2, NULL, 1 },
    { "mod_power_of_2", ANVIL_MIR_MOD, match_mod_power_of_2, emit_mod_power_of_2, NULL, 1 },
    { "add_to_lea", ANVIL_MIR_ADD, match_add_to_lea, emit_add_to_lea, NULL, 5 },
    { "mov_zero", ANVIL_MIR_MOV, match_mov_zero, emit_mov_zero, NULL, 1 },
    { "sub_neg_to_add", ANVIL_MIR_SUB, match_sub_to_neg, emit_sub_to_neg, NULL, 2 },
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
