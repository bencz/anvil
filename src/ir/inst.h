#ifndef ANVIL_INST_H
#define ANVIL_INST_H

#include "value.h"
#include "../core/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnvilInstKind {
    ANVIL_INST_NOP,
    
    ANVIL_INST_ADD,
    ANVIL_INST_SUB,
    ANVIL_INST_MUL,
    ANVIL_INST_DIV,
    ANVIL_INST_MOD,
    ANVIL_INST_NEG,
    
    ANVIL_INST_AND,
    ANVIL_INST_OR,
    ANVIL_INST_XOR,
    ANVIL_INST_NOT,
    ANVIL_INST_SHL,
    ANVIL_INST_SHR,
    ANVIL_INST_SAR,
    
    ANVIL_INST_EQ,
    ANVIL_INST_NE,
    ANVIL_INST_LT,
    ANVIL_INST_LE,
    ANVIL_INST_GT,
    ANVIL_INST_GE,
    
    ANVIL_INST_LOAD,
    ANVIL_INST_STORE,
    ANVIL_INST_ALLOCA,
    ANVIL_INST_ADDR_OF,
    ANVIL_INST_DEREF,
    ANVIL_INST_INDEX,
    ANVIL_INST_FIELD,
    
    ANVIL_INST_CAST,
    ANVIL_INST_BITCAST,
    ANVIL_INST_TRUNC,
    ANVIL_INST_ZEXT,
    ANVIL_INST_SEXT,
    ANVIL_INST_FPEXT,
    ANVIL_INST_FPTRUNC,
    ANVIL_INST_FPTOSI,
    ANVIL_INST_FPTOUI,
    ANVIL_INST_SITOFP,
    ANVIL_INST_UITOFP,
    ANVIL_INST_PTRTOINT,
    ANVIL_INST_INTTOPTR,
    
    ANVIL_INST_BR,
    ANVIL_INST_BR_COND,
    ANVIL_INST_RET,
    ANVIL_INST_RET_VOID,
    
    ANVIL_INST_CALL,
    ANVIL_INST_CALL_INDIRECT,
    
    ANVIL_INST_PHI,
    ANVIL_INST_SELECT,
} AnvilInstKind;

typedef struct AnvilBlock AnvilBlock;

typedef struct AnvilInst {
    AnvilInstKind kind;
    AnvilValue* result;
    AnvilValue* operands[3];
    int num_operands;
    
    union {
        struct {
            AnvilBlock* target;
        } br;
        
        struct {
            AnvilBlock* then_block;
            AnvilBlock* else_block;
        } br_cond;
        
        struct {
            const char* func_name;
            AnvilVec args;
            int num_fixed_args;
            bool is_variadic;
        } call;
        
        struct {
            AnvilType* target_type;
        } cast;
        
        struct {
            const char* field_name;
            int field_index;
        } field;
        
        struct {
            AnvilVec incoming_values;
            AnvilVec incoming_blocks;
        } phi;
    };
    
    struct AnvilInst* next;
    struct AnvilInst* prev;
} AnvilInst;

AnvilInst* anvil_inst_create(AnvilArena* arena, AnvilInstKind kind);
AnvilInst* anvil_inst_binary(AnvilArena* arena, AnvilInstKind kind, AnvilValue* lhs, AnvilValue* rhs, AnvilValue* result);
AnvilInst* anvil_inst_unary(AnvilArena* arena, AnvilInstKind kind, AnvilValue* operand, AnvilValue* result);
AnvilInst* anvil_inst_ret(AnvilArena* arena, AnvilValue* val);
AnvilInst* anvil_inst_ret_void(AnvilArena* arena);
AnvilInst* anvil_inst_br(AnvilArena* arena, AnvilBlock* target);
AnvilInst* anvil_inst_br_cond(AnvilArena* arena, AnvilValue* cond, AnvilBlock* then_bb, AnvilBlock* else_bb);
AnvilInst* anvil_inst_call(AnvilArena* arena, const char* func_name, AnvilValue* result);
AnvilInst* anvil_inst_load(AnvilArena* arena, AnvilValue* ptr, AnvilValue* result);
AnvilInst* anvil_inst_store(AnvilArena* arena, AnvilValue* ptr, AnvilValue* val);
AnvilInst* anvil_inst_alloca(AnvilArena* arena, AnvilType* type, AnvilValue* result);
AnvilInst* anvil_inst_cast(AnvilArena* arena, AnvilInstKind kind, AnvilValue* val, AnvilType* target_type, AnvilValue* result);

void anvil_inst_add_arg(AnvilInst* call_inst, AnvilValue* arg);

#ifdef __cplusplus
}
#endif

#endif
