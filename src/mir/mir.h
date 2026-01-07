#ifndef ANVIL_MIR_H
#define ANVIL_MIR_H

#include "../core/arena.h"
#include "../core/vec.h"
#include "../ir/types_internal.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnvilMInstKind {
    ANVIL_MIR_NOP,
    
    ANVIL_MIR_MOV,
    ANVIL_MIR_MOV_IMM,
    ANVIL_MIR_LOAD,
    ANVIL_MIR_STORE,
    ANVIL_MIR_LEA,
    
    ANVIL_MIR_ADD,
    ANVIL_MIR_SUB,
    ANVIL_MIR_MUL,
    ANVIL_MIR_IMUL,
    ANVIL_MIR_DIV,
    ANVIL_MIR_IDIV,
    ANVIL_MIR_MOD,
    ANVIL_MIR_NEG,
    
    ANVIL_MIR_AND,
    ANVIL_MIR_OR,
    ANVIL_MIR_XOR,
    ANVIL_MIR_NOT,
    ANVIL_MIR_SHL,
    ANVIL_MIR_SHR,
    ANVIL_MIR_SAR,
    ANVIL_MIR_ROL,
    ANVIL_MIR_ROR,
    
    ANVIL_MIR_CMP,
    ANVIL_MIR_TEST,
    ANVIL_MIR_SETCC,
    ANVIL_MIR_CMOV,
    ANVIL_MIR_SELECT,
    
    ANVIL_MIR_JMP,
    ANVIL_MIR_JCC,
    ANVIL_MIR_RET,
    
    ANVIL_MIR_CALL,
    ANVIL_MIR_CALL_INDIRECT,
    ANVIL_MIR_CALL_PLT,
    ANVIL_MIR_CALL_GOT,
    ANVIL_MIR_TAIL_CALL,
    
    ANVIL_MIR_PHI,
    
    ANVIL_MIR_PUSH,
    ANVIL_MIR_POP,
    
    ANVIL_MIR_CVTSI2SS,
    ANVIL_MIR_CVTSI2SD,
    ANVIL_MIR_CVTSS2SI,
    ANVIL_MIR_CVTSD2SI,
    ANVIL_MIR_CVTSS2SD,
    ANVIL_MIR_CVTSD2SS,
    
    ANVIL_MIR_FADD,
    ANVIL_MIR_FSUB,
    ANVIL_MIR_FMUL,
    ANVIL_MIR_FDIV,
    ANVIL_MIR_FNEG,
    ANVIL_MIR_FABS,
    ANVIL_MIR_FSQRT,
    ANVIL_MIR_FCMP,
    ANVIL_MIR_FMADD,
    ANVIL_MIR_FMSUB,
    ANVIL_MIR_FNMADD,
    ANVIL_MIR_FNMSUB,
    ANVIL_MIR_FMIN,
    ANVIL_MIR_FMAX,
    
    ANVIL_MIR_MOVSS,
    ANVIL_MIR_MOVSD,
    ANVIL_MIR_MOVAPS,
    ANVIL_MIR_MOVUPS,
    ANVIL_MIR_MOVAPD,
    ANVIL_MIR_MOVUPD,
    
    ANVIL_MIR_ADDPS,
    ANVIL_MIR_ADDPD,
    ANVIL_MIR_SUBPS,
    ANVIL_MIR_SUBPD,
    ANVIL_MIR_MULPS,
    ANVIL_MIR_MULPD,
    ANVIL_MIR_DIVPS,
    ANVIL_MIR_DIVPD,
    
    ANVIL_MIR_ZEXT,
    ANVIL_MIR_SEXT,
    ANVIL_MIR_TRUNC,
    ANVIL_MIR_BITCAST,
    
    ANVIL_MIR_COPY,
    
    ANVIL_MIR_LDP,
    ANVIL_MIR_STP,
    ANVIL_MIR_MADD,
    ANVIL_MIR_MSUB,
    ANVIL_MIR_CSEL,
    ANVIL_MIR_CSINC,
    ANVIL_MIR_CSNEG,
    
    ANVIL_MIR_LOAD_UPDATE,
    ANVIL_MIR_STORE_UPDATE,
    
    ANVIL_MIR_XCHG,
    ANVIL_MIR_CMPXCHG,
    ANVIL_MIR_LOCK_ADD,
    ANVIL_MIR_LOCK_SUB,
    ANVIL_MIR_LOCK_AND,
    ANVIL_MIR_LOCK_OR,
    ANVIL_MIR_LOCK_XOR,
    
    ANVIL_MIR_PREFETCH,
    ANVIL_MIR_FENCE,
    
    ANVIL_MIR_LABEL,
    ANVIL_MIR_COMMENT,
    
    ANVIL_MIR_COUNT
} AnvilMInstKind;

typedef enum AnvilCondCode {
    ANVIL_CC_EQ,
    ANVIL_CC_NE,
    ANVIL_CC_LT,
    ANVIL_CC_LE,
    ANVIL_CC_GT,
    ANVIL_CC_GE,
    ANVIL_CC_ULT,
    ANVIL_CC_ULE,
    ANVIL_CC_UGT,
    ANVIL_CC_UGE,
    ANVIL_CC_S,
    ANVIL_CC_NS,
    ANVIL_CC_O,
    ANVIL_CC_NO,
} AnvilCondCode;

typedef enum AnvilMOperandKind {
    ANVIL_MOP_NONE,
    ANVIL_MOP_VREG,
    ANVIL_MOP_PREG,
    ANVIL_MOP_IMM,
    ANVIL_MOP_FIMM,
    ANVIL_MOP_MEM,
    ANVIL_MOP_LABEL,
    ANVIL_MOP_FUNC,
    ANVIL_MOP_GLOBAL,
} AnvilMOperandKind;

typedef struct AnvilMOperand {
    AnvilMOperandKind kind;
    int size;
    bool is_fp;
    
    union {
        struct {
            int id;
            AnvilType* type;
        } vreg;
        
        struct {
            int id;
        } preg;
        
        struct {
            int64_t value;
        } imm;
        
        struct {
            double value;
        } fimm;
        
        struct {
            int base_reg;
            int index_reg;
            int scale;
            int64_t disp;
            bool base_is_vreg;
            bool index_is_vreg;
        } mem;
        
        struct {
            const char* name;
            int id;
        } label;
        
        struct {
            const char* name;
        } func;
        
        struct {
            const char* name;
        } global;
    };
} AnvilMOperand;

struct AnvilMFunc;

typedef struct AnvilMInst {
    AnvilMInstKind kind;
    AnvilCondCode cc;
    
    AnvilMOperand* operands;
    int num_operands;
    int operands_capacity;
    
    AnvilMOperand defs[2];
    int num_defs;
    
    const char* comment;
    
    bool is_variadic;
    int num_fixed_args;
    bool is_tail_call;
    
    struct AnvilMFunc* func;
    
    struct AnvilMInst* next;
    struct AnvilMInst* prev;
} AnvilMInst;

typedef struct AnvilMBlock {
    const char* name;
    int id;
    
    AnvilMInst* first;
    AnvilMInst* last;
    int inst_count;
    
    AnvilVec preds;
    AnvilVec succs;
    
    AnvilVec live_in;
    AnvilVec live_out;
    
    struct AnvilMFunc* func;
    struct AnvilMBlock* next;
    struct AnvilMBlock* prev;
} AnvilMBlock;

struct AnvilABI;

typedef struct AnvilMFunc {
    const char* name;
    AnvilType* ret_type;
    
    AnvilVec params;
    int num_params;
    
    AnvilMBlock* entry;
    AnvilVec blocks;
    int block_count;
    
    int next_vreg_id;
    int next_block_id;
    
    int stack_size;
    int spill_slots;
    
    bool is_leaf;
    bool needs_frame;
    
    const struct AnvilABI* abi;
    
    AnvilArena* arena;
    struct AnvilMFunc* next;
} AnvilMFunc;

typedef struct AnvilMString {
    int id;
    const char* value;
} AnvilMString;

typedef struct AnvilMIR {
    AnvilArena* arena;
    AnvilMFunc* first_func;
    AnvilMFunc* last_func;
    int func_count;
    
    AnvilMString* strings;
    int string_count;
    int string_capacity;
} AnvilMIR;

AnvilMIR* anvil_mir_create(AnvilArena* arena);
AnvilMFunc* anvil_mir_add_func(AnvilMIR* mir, const char* name, AnvilType* ret_type);
void anvil_mir_add_string(AnvilMIR* mir, int id, const char* value);

AnvilMFunc* anvil_mfunc_create(AnvilArena* arena, const char* name, AnvilType* ret_type);
AnvilMBlock* anvil_mfunc_add_block(AnvilMFunc* func, const char* name);
int anvil_mfunc_new_vreg(AnvilMFunc* func);

AnvilMBlock* anvil_mblock_create(AnvilArena* arena, AnvilMFunc* func, const char* name);
void anvil_mblock_append(AnvilMBlock* block, AnvilMInst* inst);
void anvil_mblock_prepend(AnvilMBlock* block, AnvilMInst* inst);
void anvil_mblock_insert_before(AnvilMBlock* block, AnvilMInst* before, AnvilMInst* inst);
void anvil_mblock_insert_after(AnvilMBlock* block, AnvilMInst* after, AnvilMInst* inst);
void anvil_mblock_remove(AnvilMBlock* block, AnvilMInst* inst);

AnvilMInst* anvil_minst_create(AnvilArena* arena, AnvilMInstKind kind);
AnvilMInst* anvil_minst_create_with_capacity(AnvilArena* arena, AnvilMInstKind kind, int capacity);
AnvilMInst* anvil_minst_mov(AnvilArena* arena, AnvilMOperand dst, AnvilMOperand src);
AnvilMInst* anvil_minst_mov_imm(AnvilArena* arena, AnvilMOperand dst, int64_t imm, int size);
AnvilMInst* anvil_minst_binary(AnvilArena* arena, AnvilMInstKind kind, AnvilMOperand dst, AnvilMOperand src);
AnvilMInst* anvil_minst_unary(AnvilArena* arena, AnvilMInstKind kind, AnvilMOperand op);
AnvilMInst* anvil_minst_cmp(AnvilArena* arena, AnvilMOperand lhs, AnvilMOperand rhs);
AnvilMInst* anvil_minst_jmp(AnvilArena* arena, const char* label);
AnvilMInst* anvil_minst_jcc(AnvilArena* arena, AnvilCondCode cc, const char* label);
AnvilMInst* anvil_minst_ret(AnvilArena* arena);
AnvilMInst* anvil_minst_call(AnvilArena* arena, const char* func_name);
AnvilMInst* anvil_minst_label(AnvilArena* arena, const char* name);

AnvilMOperand anvil_mop_none(void);
AnvilMOperand anvil_mop_vreg(int id, int size);
AnvilMOperand anvil_mop_vreg_fp(int id, int size);
AnvilMOperand anvil_mop_preg(int id, int size);
AnvilMOperand anvil_mop_preg_fp(int id, int size);
AnvilMOperand anvil_mop_imm(int64_t value, int size);
AnvilMOperand anvil_mop_fimm(double value, int size);
AnvilMOperand anvil_mop_mem(int base, int64_t disp, int size);
AnvilMOperand anvil_mop_mem_indexed(int base, int index, int scale, int64_t disp, int size);
AnvilMOperand anvil_mop_label(const char* name, int id);
AnvilMOperand anvil_mop_func(const char* name);
AnvilMOperand anvil_mop_global(const char* name);

#ifdef __cplusplus
}
#endif

#endif
