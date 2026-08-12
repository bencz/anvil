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
    char *symbol;
    int spill_slot;
    int frame_slot;
} anvil_mir_instr_t;

typedef struct anvil_mir_block {
    char *name;
    size_t first_instr;
    size_t num_instrs;
} anvil_mir_block_data_t;

typedef struct anvil_mir_string_literal {
    char *label;
    char *value;
} anvil_mir_string_literal_t;

struct anvil_mir_func {
    char *name;

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

#endif /* ANVIL_MACHINE_INTERNAL_H */
