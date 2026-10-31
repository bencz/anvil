/*
 * ANVIL - Internal MachineIR structures.
 */

#ifndef ANVIL_MACHINE_INTERNAL_H
#define ANVIL_MACHINE_INTERNAL_H

#include "anvil/anvil_machine.h"

typedef struct anvil_mir_instr {
    anvil_mir_opcode_t op;
    anvil_mir_vreg_t def;
    anvil_mir_vreg_t *uses;
    size_t num_uses;
    anvil_mir_block_t block;
    anvil_mir_block_t true_block;
    anvil_mir_block_t false_block;
    bool has_imm;
    int64_t imm;
    anvil_cc_t call_cc;
    anvil_memory_access_t memory_access;
    anvil_op_t ir_op;
    anvil_atomic_info_t atomic;
    unsigned named_gpr;
    unsigned named_fpr;
    size_t named_stack_bytes;
    unsigned call_effects;
    uint64_t clobbers[ANVIL_MIR_REG_CLASS_COUNT];
    uint64_t scratch[ANVIL_MIR_REG_CLASS_COUNT];
    char *symbol;
    int spill_slot;
    int frame_slot;
    uint8_t index_scale;
    bool symbol_got;
    anvil_fcmp_pred_t fcmp_pred;
    uint8_t widening;
} anvil_mir_instr_t;

typedef struct anvil_mir_block {
    char *name;
    size_t first_instr;
    size_t num_instrs;
    bool cold;          /* rarely runs (from the IR block) */
} anvil_mir_block_data_t;

typedef struct anvil_mir_string_literal {
    char *label;
    char *value;
} anvil_mir_string_literal_t;

struct anvil_mir_func {
    char *name;
    anvil_linkage_t linkage;
    anvil_cpu_features_t cpu_features;  /* of the CPU the function targets */

    anvil_mir_block_data_t *blocks;
    size_t num_blocks;
    size_t cap_blocks;
    anvil_mir_block_t current_block;

    anvil_mir_instr_t *instrs;
    size_t num_instrs;
    size_t cap_instrs;

    anvil_mir_vreg_info_t *vregs;
    size_t num_vregs;
    size_t cap_vregs;

    anvil_regalloc_assignment_t *assignments;
    anvil_mir_spill_slot_info_t *spill_slots;
    size_t num_spills;
    size_t cap_spills;
    size_t num_pinned_spills; /* Explicit interval-split storage survives reallocation. */

    anvil_mir_frame_slot_info_t *frame_slots;
    size_t num_frame_slots;
    size_t cap_frame_slots;

    anvil_mir_string_literal_t *string_literals;
    size_t num_string_literals;
    size_t cap_string_literals;
};

bool anvil_mir_valid_vreg(const anvil_mir_func_t *func, anvil_mir_vreg_t vreg);
bool anvil_mir_prepare_assignments(anvil_mir_func_t *func);
int anvil_mir_allocate_spill_slot(anvil_mir_func_t *func,
                                  anvil_mir_reg_class_t reg_class,
                                  uint16_t size_bits);

typedef struct
{
    size_t words_per_block;
    uint64_t *live_in;
    uint64_t *live_out;
    size_t *first_instr;
    size_t *last_instr;
} anvil_mir_liveness_t;

bool anvil_mir_compute_liveness(const anvil_mir_func_t *func, anvil_mir_liveness_t *result);
void anvil_mir_liveness_destroy(anvil_mir_liveness_t *result);
bool anvil_mir_verify_clobbers(const anvil_mir_func_t *func, char *error, size_t error_len);
bool anvil_mir_split_spilled_intervals(anvil_mir_func_t *func, bool *changed);
/* Spills values that occupy scratch registers where a bundle's reloads do
 * not fit (anvil_mir_materialize_spills would fail there); *changed tells
 * whether any was. */
bool anvil_mir_spill_for_scratch(anvil_mir_func_t *func, const anvil_regalloc_class_config_t *scratch_configs,
                                 size_t num_scratch_configs, bool *changed);
/* split_class[c]: whether values of class c are split (no allocatable
 * register of the class survives the cold calls). */
bool anvil_mir_split_around_cold_calls(anvil_mir_func_t *func, const bool *split_class);
bool anvil_mir_sink_compares(anvil_mir_func_t *func);

#endif /* ANVIL_MACHINE_INTERNAL_H */
