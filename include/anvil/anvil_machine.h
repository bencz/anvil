/*
 * ANVIL - Machine IR and register allocation infrastructure.
 *
 * This layer is target-independent backend infrastructure. Backends can lower
 * ANVIL IR into MachineIR, run allocation, then emit target assembly from the
 * allocated machine operations.
 */

#ifndef ANVIL_MACHINE_H
#define ANVIL_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anvil.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t anvil_mir_vreg_t;
typedef uint32_t anvil_mir_block_t;

#define ANVIL_MIR_NO_VREG UINT32_MAX
#define ANVIL_MIR_NO_BLOCK UINT32_MAX

typedef enum {
    ANVIL_MIR_REG_GPR = 0,
    ANVIL_MIR_REG_FPR,
    ANVIL_MIR_REG_FLAGS,
    ANVIL_MIR_REG_SPECIAL,
    ANVIL_MIR_REG_CLASS_COUNT
} anvil_mir_reg_class_t;

typedef enum {
    ANVIL_MIR_OP_INVALID = 0,
    ANVIL_MIR_OP_MOV,
    ANVIL_MIR_OP_COPY,
    ANVIL_MIR_OP_ADD,
    ANVIL_MIR_OP_SUB,
    ANVIL_MIR_OP_MUL,
    ANVIL_MIR_OP_DIV,
    ANVIL_MIR_OP_SDIV,
    ANVIL_MIR_OP_UDIV,
    ANVIL_MIR_OP_FDIV,
    ANVIL_MIR_OP_SMOD,
    ANVIL_MIR_OP_UMOD,
    ANVIL_MIR_OP_AND,
    ANVIL_MIR_OP_OR,
    ANVIL_MIR_OP_XOR,
    ANVIL_MIR_OP_SHL,
    ANVIL_MIR_OP_SHR,
    ANVIL_MIR_OP_SAR,
    ANVIL_MIR_OP_NEG,
    ANVIL_MIR_OP_NOT,
    ANVIL_MIR_OP_FABS,
    ANVIL_MIR_OP_ZEXT,
    ANVIL_MIR_OP_SEXT,
    ANVIL_MIR_OP_TRUNC,
    ANVIL_MIR_OP_BITCAST,
    ANVIL_MIR_OP_SITOFP,
    ANVIL_MIR_OP_UITOFP,
    ANVIL_MIR_OP_FPTOSI,
    ANVIL_MIR_OP_FPTOUI,
    ANVIL_MIR_OP_FPEXT,
    ANVIL_MIR_OP_FPTRUNC,
    ANVIL_MIR_OP_SELECT,
    ANVIL_MIR_OP_CMP,
    ANVIL_MIR_OP_CMP_EQ,
    ANVIL_MIR_OP_CMP_NE,
    ANVIL_MIR_OP_CMP_LT,
    ANVIL_MIR_OP_CMP_LE,
    ANVIL_MIR_OP_CMP_GT,
    ANVIL_MIR_OP_CMP_GE,
    ANVIL_MIR_OP_CMP_ULT,
    ANVIL_MIR_OP_CMP_ULE,
    ANVIL_MIR_OP_CMP_UGT,
    ANVIL_MIR_OP_CMP_UGE,
    /* Floating comparison. Required immediate is anvil_fcmp_pred_t. */
    ANVIL_MIR_OP_FCMP,
    ANVIL_MIR_OP_SYMBOL_ADDR,
    ANVIL_MIR_OP_LOAD,
    ANVIL_MIR_OP_STORE,
    ANVIL_MIR_OP_FRAME_ADDR,
    ANVIL_MIR_OP_DYN_ALLOCA,
    ANVIL_MIR_OP_INCOMING_STACK_ARG,
    ANVIL_MIR_OP_CALL,
    ANVIL_MIR_OP_CALL_STACK_ARG,
    ANVIL_MIR_OP_RET,
    ANVIL_MIR_OP_BR,
    ANVIL_MIR_OP_BR_COND,
    ANVIL_MIR_OP_SPILL_LOAD,
    ANVIL_MIR_OP_SPILL_STORE,
    /* Semantic no-op that keeps all of its operands live. */
    ANVIL_MIR_OP_KEEPALIVE,
    /* Defines an additional fixed-register result of the preceding call. */
    ANVIL_MIR_OP_CALL_RESULT,
    /* Supplies a value to an additional ABI return register before RET.  The
       source need not be fixed: the target emitter performs the ABI move. */
    ANVIL_MIR_OP_RET_VALUE_PART
} anvil_mir_opcode_t;

typedef struct anvil_mir_func anvil_mir_func_t;

typedef struct {
    anvil_mir_reg_class_t reg_class;
    uint16_t size_bits;
    bool is_signed;
    bool is_live_in;
    bool has_fixed_reg;
    int fixed_phys_reg;
} anvil_mir_vreg_info_t;

typedef struct {
    anvil_mir_reg_class_t reg_class;
    bool spilled;
    int phys_reg;
    int spill_slot;
} anvil_regalloc_assignment_t;

typedef struct {
    anvil_mir_reg_class_t reg_class;
    int num_phys_regs;
    const int *phys_regs;
} anvil_regalloc_class_config_t;

typedef struct {
    anvil_mir_reg_class_t reg_class;
    uint16_t size_bits;
} anvil_mir_spill_slot_info_t;

typedef struct {
    uint16_t size_bits;
    uint16_t align_bytes;
} anvil_mir_frame_slot_info_t;

typedef struct {
    const char *label;
    const char *value;
} anvil_mir_string_literal_info_t;

typedef struct {
    const char *name;
    /* Index of the first instruction owned by this block and total ownership
       count. Instructions are not required to be contiguous; consumers must
       filter anvil_mir_instr_info_t::block rather than treating this as a
       [first_instr, first_instr + num_instrs) interval. */
    size_t first_instr;
    size_t num_instrs;
} anvil_mir_block_info_t;

typedef struct {
    anvil_mir_opcode_t op;
    anvil_mir_vreg_t def;
    size_t num_uses;
    anvil_mir_block_t block;
    anvil_mir_block_t true_block;
    anvil_mir_block_t false_block;
    bool has_imm;
    int64_t imm;
    /* Effective, target-canonical calling convention for CALL.  The
       immediate remains available for ABI-specific payloads such as the
       SysV variadic vector-register count. */
    anvil_cc_t call_cc;
    const char *symbol;
    int spill_slot;
    int frame_slot;
} anvil_mir_instr_info_t;

anvil_mir_func_t *anvil_mir_func_create(const char *name);
void anvil_mir_func_destroy(anvil_mir_func_t *func);

const char *anvil_mir_func_name(const anvil_mir_func_t *func);

anvil_mir_block_t anvil_mir_add_block(anvil_mir_func_t *func,
                                      const char *name);
bool anvil_mir_set_current_block(anvil_mir_func_t *func,
                                 anvil_mir_block_t block);
anvil_mir_block_t anvil_mir_current_block(const anvil_mir_func_t *func);
size_t anvil_mir_num_blocks(const anvil_mir_func_t *func);
bool anvil_mir_get_block_info(const anvil_mir_func_t *func,
                              anvil_mir_block_t block,
                              anvil_mir_block_info_t *out_info);

anvil_mir_vreg_t anvil_mir_add_vreg(anvil_mir_func_t *func);
anvil_mir_vreg_t anvil_mir_add_vreg_ex(anvil_mir_func_t *func,
                                       anvil_mir_reg_class_t reg_class,
                                       uint16_t size_bits);
anvil_mir_vreg_t anvil_mir_add_vreg_typed(anvil_mir_func_t *func,
                                          anvil_mir_reg_class_t reg_class,
                                          uint16_t size_bits,
                                          bool is_signed);
const anvil_mir_vreg_info_t *
anvil_mir_get_vreg_info(const anvil_mir_func_t *func, anvil_mir_vreg_t vreg);
bool anvil_mir_set_vreg_signed(anvil_mir_func_t *func,
                               anvil_mir_vreg_t vreg,
                               bool is_signed);
bool anvil_mir_set_live_in(anvil_mir_func_t *func, anvil_mir_vreg_t vreg,
                           bool is_live_in);
bool anvil_mir_set_fixed_reg(anvil_mir_func_t *func, anvil_mir_vreg_t vreg,
                             int phys_reg);
bool anvil_mir_clear_fixed_reg(anvil_mir_func_t *func, anvil_mir_vreg_t vreg);
bool anvil_mir_add_instr(anvil_mir_func_t *func, anvil_mir_opcode_t op,
                         anvil_mir_vreg_t def,
                         const anvil_mir_vreg_t *uses,
                         size_t num_uses);
bool anvil_mir_add_instr_imm(anvil_mir_func_t *func, anvil_mir_opcode_t op,
                             anvil_mir_vreg_t def, int64_t imm);
bool anvil_mir_add_instr_imm_uses(anvil_mir_func_t *func,
                                  anvil_mir_opcode_t op,
                                  anvil_mir_vreg_t def,
                                  const anvil_mir_vreg_t *uses,
                                  size_t num_uses,
                                  int64_t imm);
bool anvil_mir_add_instr_symbol(anvil_mir_func_t *func,
                                anvil_mir_opcode_t op,
                                anvil_mir_vreg_t def,
                                const anvil_mir_vreg_t *uses,
                                size_t num_uses,
                                const char *symbol);
bool anvil_mir_add_instr_symbol_imm(anvil_mir_func_t *func,
                                    anvil_mir_opcode_t op,
                                    anvil_mir_vreg_t def,
                                    const anvil_mir_vreg_t *uses,
                                    size_t num_uses,
                                    const char *symbol,
                                    int64_t imm);
bool anvil_mir_add_call(anvil_mir_func_t *func,
                        anvil_mir_vreg_t def,
                        const anvil_mir_vreg_t *uses,
                        size_t num_uses,
                        const char *symbol,
                        anvil_cc_t call_cc,
                        bool has_abi_imm,
                        int64_t abi_imm);
int anvil_mir_add_frame_slot(anvil_mir_func_t *func,
                             uint16_t size_bits,
                             uint16_t align_bytes);
bool anvil_mir_add_frame_addr(anvil_mir_func_t *func,
                              anvil_mir_vreg_t def,
                              int frame_slot);
int anvil_mir_add_string_literal(anvil_mir_func_t *func,
                                 const char *value,
                                 const char **out_label);
bool anvil_mir_add_branch(anvil_mir_func_t *func,
                          anvil_mir_block_t target);
bool anvil_mir_add_cond_branch(anvil_mir_func_t *func,
                               anvil_mir_vreg_t cond,
                               anvil_mir_block_t true_block,
                               anvil_mir_block_t false_block);

size_t anvil_mir_num_vregs(const anvil_mir_func_t *func);
size_t anvil_mir_num_instrs(const anvil_mir_func_t *func);
bool anvil_mir_get_instr_info(const anvil_mir_func_t *func, size_t index,
                              anvil_mir_instr_info_t *out_info);
anvil_mir_vreg_t anvil_mir_get_instr_use(const anvil_mir_func_t *func,
                                         size_t instr_index,
                                         size_t use_index);

void anvil_mir_clear_allocations(anvil_mir_func_t *func);
bool anvil_mir_coalesce_copies(anvil_mir_func_t *func);
bool anvil_regalloc_linear_scan(anvil_mir_func_t *func, int num_phys_regs);
bool anvil_regalloc_linear_scan_classes(
    anvil_mir_func_t *func,
    const anvil_regalloc_class_config_t *configs,
    size_t num_configs);
bool anvil_mir_materialize_spills(
    anvil_mir_func_t *func,
    const anvil_regalloc_class_config_t *scratch_configs,
    size_t num_scratch_configs);

const anvil_regalloc_assignment_t *
anvil_mir_get_assignment(const anvil_mir_func_t *func, anvil_mir_vreg_t vreg);
size_t anvil_mir_num_spills(const anvil_mir_func_t *func);
bool anvil_mir_get_spill_slot_info(const anvil_mir_func_t *func,
                                   int spill_slot,
                                   anvil_mir_spill_slot_info_t *out_info);
size_t anvil_mir_num_frame_slots(const anvil_mir_func_t *func);
bool anvil_mir_get_frame_slot_info(const anvil_mir_func_t *func,
                                   int frame_slot,
                                   anvil_mir_frame_slot_info_t *out_info);
size_t anvil_mir_num_string_literals(const anvil_mir_func_t *func);
bool anvil_mir_get_string_literal_info(
    const anvil_mir_func_t *func,
    size_t index,
    anvil_mir_string_literal_info_t *out_info);

bool anvil_mir_verify(const anvil_mir_func_t *func,
                      char *error,
                      size_t error_len);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_MACHINE_H */
