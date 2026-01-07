#ifndef ANVIL_ISEL_H
#define ANVIL_ISEL_H

#include "mir.h"
#include "../core/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnvilISelContext AnvilISelContext;
typedef struct AnvilISelPattern AnvilISelPattern;
typedef struct AnvilISelRule AnvilISelRule;

typedef enum AnvilISelPatternKind {
    ANVIL_ISEL_PAT_INST,
    ANVIL_ISEL_PAT_IMM,
    ANVIL_ISEL_PAT_REG,
    ANVIL_ISEL_PAT_MEM,
    ANVIL_ISEL_PAT_ANY,
    ANVIL_ISEL_PAT_POWER_OF_2,
    ANVIL_ISEL_PAT_SMALL_CONST,
    ANVIL_ISEL_PAT_ZERO,
} AnvilISelPatternKind;

typedef struct AnvilISelMatch {
    AnvilMInst* inst;
    AnvilMInst* operands[8];
    int64_t imm_values[8];
    int matched_count;
} AnvilISelMatch;

typedef bool (*AnvilISelMatchFn)(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match);
typedef void (*AnvilISelEmitFn)(AnvilISelContext* ctx, AnvilISelMatch* match, AnvilVec* output);
typedef int (*AnvilISelCostFn)(AnvilISelContext* ctx, AnvilISelMatch* match);

struct AnvilISelRule {
    const char* name;
    AnvilMInstKind src_kind;
    AnvilISelMatchFn match;
    AnvilISelEmitFn emit;
    AnvilISelCostFn cost;
    int priority;
};

typedef struct AnvilISelRuleSet {
    const AnvilISelRule* rules;
    int num_rules;
} AnvilISelRuleSet;

struct AnvilISelContext {
    AnvilMFunc* func;
    AnvilArena* arena;
    const AnvilISelRuleSet* ruleset;
    
    AnvilMInst* (*create_inst)(AnvilISelContext* ctx, AnvilMInstKind kind);
    void (*emit_inst)(AnvilISelContext* ctx, AnvilMInst* inst, AnvilVec* output);
    
    void* target_data;
};

void anvil_isel_init(AnvilISelContext* ctx, AnvilMFunc* func, const AnvilISelRuleSet* ruleset);
void anvil_isel_run(AnvilISelContext* ctx);
AnvilMInst* anvil_isel_create_inst(AnvilISelContext* ctx, AnvilMInstKind kind);

bool anvil_isel_match_power_of_2(int64_t value, int* shift_out);
bool anvil_isel_match_small_const(int64_t value, int bits);

#ifdef __cplusplus
}
#endif

#endif
