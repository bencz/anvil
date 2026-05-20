/*
 * ANVIL - Linear-scan register allocation for MachineIR.
 */

#include "machine_internal.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct {
    anvil_mir_vreg_t vreg;
    size_t start;
    size_t end;
    bool live;
} live_interval_t;

static int phys_reg_at(const anvil_regalloc_class_config_t *config,
                       int index)
{
    return config->phys_regs ? config->phys_regs[index] : index;
}

static void note_live_range(live_interval_t *intervals,
                            anvil_mir_vreg_t vreg,
                            size_t index)
{
    live_interval_t *interval = &intervals[vreg];
    if (!interval->live) {
        interval->start = index;
        interval->end = index;
        interval->live = true;
        return;
    }

    if (index < interval->start) interval->start = index;
    if (index > interval->end) interval->end = index;
}

static size_t live_bit_index(const anvil_mir_func_t *func,
                             anvil_mir_block_t block,
                             anvil_mir_vreg_t vreg)
{
    return ((size_t)block * func->num_vregs) + (size_t)vreg;
}

static bool valid_block_index(const anvil_mir_func_t *func,
                              anvil_mir_block_t block)
{
    return func && block != ANVIL_MIR_NO_BLOCK && (size_t)block < func->num_blocks;
}

static bool bit_is_set(const unsigned char *bits,
                       const anvil_mir_func_t *func,
                       anvil_mir_block_t block,
                       anvil_mir_vreg_t vreg)
{
    return bits[live_bit_index(func, block, vreg)] != 0;
}

static bool set_bit_if_changed(unsigned char *bits,
                               const anvil_mir_func_t *func,
                               anvil_mir_block_t block,
                               anvil_mir_vreg_t vreg,
                               bool value)
{
    unsigned char *slot = &bits[live_bit_index(func, block, vreg)];
    unsigned char next = value ? 1 : 0;
    if (*slot == next) return false;
    *slot = next;
    return true;
}

static bool block_successor_live_in(const anvil_mir_func_t *func,
                                    const unsigned char *live_in,
                                    anvil_mir_block_t block,
                                    anvil_mir_vreg_t vreg)
{
    for (size_t i = 0; i < func->num_instrs; i++) {
        const anvil_mir_instr_t *instr = &func->instrs[i];
        if (instr->block != block) continue;

        if (valid_block_index(func, instr->true_block) &&
            bit_is_set(live_in, func, instr->true_block, vreg)) {
            return true;
        }
        if (valid_block_index(func, instr->false_block) &&
            bit_is_set(live_in, func, instr->false_block, vreg)) {
            return true;
        }
    }

    return false;
}

static bool note_cfg_live_ranges(anvil_mir_func_t *func,
                                 live_interval_t *intervals)
{
    if (func->num_blocks == 0 || func->num_vregs == 0) return true;
    if (func->num_blocks > SIZE_MAX / func->num_vregs) return false;

    size_t total_bits = func->num_blocks * func->num_vregs;
    unsigned char *use = calloc(total_bits, sizeof(*use));
    unsigned char *def = calloc(total_bits, sizeof(*def));
    unsigned char *live_in = calloc(total_bits, sizeof(*live_in));
    unsigned char *live_out = calloc(total_bits, sizeof(*live_out));
    size_t *first_instr = malloc(func->num_blocks * sizeof(*first_instr));
    size_t *last_instr = calloc(func->num_blocks, sizeof(*last_instr));

    if (!use || !def || !live_in || !live_out || !first_instr || !last_instr) {
        free(use);
        free(def);
        free(live_in);
        free(live_out);
        free(first_instr);
        free(last_instr);
        return false;
    }

    for (size_t b = 0; b < func->num_blocks; b++) {
        first_instr[b] = SIZE_MAX;
    }

    for (size_t i = 0; i < func->num_instrs; i++) {
        const anvil_mir_instr_t *instr = &func->instrs[i];
        if (!valid_block_index(func, instr->block)) continue;

        size_t block = instr->block;
        if (first_instr[block] == SIZE_MAX) first_instr[block] = i;
        last_instr[block] = i;

        for (size_t u = 0; u < instr->num_uses; u++) {
            anvil_mir_vreg_t vreg = instr->uses[u];
            if (!bit_is_set(def, func, instr->block, vreg)) {
                set_bit_if_changed(use, func, instr->block, vreg, true);
            }
        }

        if (instr->def != ANVIL_MIR_NO_VREG) {
            set_bit_if_changed(def, func, instr->block, instr->def, true);
        }
    }

    bool changed;
    do {
        changed = false;
        for (size_t b = func->num_blocks; b > 0; b--) {
            anvil_mir_block_t block = (anvil_mir_block_t)(b - 1);
            for (size_t v = 0; v < func->num_vregs; v++) {
                anvil_mir_vreg_t vreg = (anvil_mir_vreg_t)v;
                bool out = block_successor_live_in(func, live_in, block, vreg);
                if (set_bit_if_changed(live_out, func, block, vreg, out)) {
                    changed = true;
                }

                bool in = bit_is_set(use, func, block, vreg) ||
                          (out && !bit_is_set(def, func, block, vreg));
                if (set_bit_if_changed(live_in, func, block, vreg, in)) {
                    changed = true;
                }
            }
        }
    } while (changed);

    for (size_t b = 0; b < func->num_blocks; b++) {
        if (first_instr[b] == SIZE_MAX) continue;

        size_t start_pos = first_instr[b] * 2;
        size_t end_pos = (last_instr[b] * 2) + 2;
        anvil_mir_block_t block = (anvil_mir_block_t)b;

        for (size_t v = 0; v < func->num_vregs; v++) {
            anvil_mir_vreg_t vreg = (anvil_mir_vreg_t)v;
            if (bit_is_set(live_in, func, block, vreg)) {
                note_live_range(intervals, vreg, start_pos);
            }
            if (bit_is_set(live_out, func, block, vreg)) {
                note_live_range(intervals, vreg, end_pos);
            }
        }
    }

    free(use);
    free(def);
    free(live_in);
    free(live_out);
    free(first_instr);
    free(last_instr);
    return true;
}

static int compare_intervals(const live_interval_t *intervals,
                             anvil_mir_vreg_t lhs,
                             anvil_mir_vreg_t rhs)
{
    const live_interval_t *a = &intervals[lhs];
    const live_interval_t *b = &intervals[rhs];

    if (!a->live && !b->live) return 0;
    if (!a->live) return 1;
    if (!b->live) return -1;
    if (a->start < b->start) return -1;
    if (a->start > b->start) return 1;
    if (a->end < b->end) return -1;
    if (a->end > b->end) return 1;
    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

static void sort_vregs_by_start(anvil_mir_vreg_t *order,
                                size_t count,
                                const live_interval_t *intervals)
{
    for (size_t i = 1; i < count; i++) {
        anvil_mir_vreg_t current = order[i];
        size_t j = i;
        while (j > 0 && compare_intervals(intervals, current, order[j - 1]) < 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = current;
    }
}

static void expire_old_intervals(anvil_mir_vreg_t *active,
                                 size_t *num_active,
                                 const live_interval_t *intervals,
                                 size_t current_start)
{
    size_t write = 0;
    for (size_t read = 0; read < *num_active; read++) {
        anvil_mir_vreg_t vreg = active[read];
        if (intervals[vreg].end >= current_start) {
            active[write++] = vreg;
        }
    }
    *num_active = write;
}

static int find_free_phys_reg(const anvil_mir_func_t *func,
                              const anvil_mir_vreg_t *active,
                              size_t num_active,
                              const anvil_regalloc_class_config_t *config)
{
    if (!config || config->num_phys_regs <= 0) return -1;

    for (int i = 0; i < config->num_phys_regs; i++) {
        int candidate = phys_reg_at(config, i);
        bool used = false;

        for (size_t a = 0; a < num_active; a++) {
            const anvil_regalloc_assignment_t *assignment =
                &func->assignments[active[a]];
            if (!assignment->spilled &&
                assignment->reg_class == config->reg_class &&
                assignment->phys_reg == candidate) {
                used = true;
                break;
            }
        }

        if (!used) {
            return candidate;
        }
    }

    return -1;
}

static bool mark_spilled(anvil_mir_func_t *func, anvil_mir_vreg_t vreg)
{
    int slot = anvil_mir_allocate_spill_slot(func,
                                             func->vregs[vreg].reg_class,
                                             func->vregs[vreg].size_bits);
    if (slot < 0) return false;

    anvil_regalloc_assignment_t *assignment = &func->assignments[vreg];
    assignment->reg_class = func->vregs[vreg].reg_class;
    assignment->spilled = true;
    assignment->phys_reg = -1;
    assignment->spill_slot = slot;
    return true;
}

static void assign_phys_reg(anvil_mir_func_t *func,
                            anvil_mir_vreg_t vreg,
                            int phys_reg)
{
    anvil_regalloc_assignment_t *assignment = &func->assignments[vreg];
    assignment->reg_class = func->vregs[vreg].reg_class;
    assignment->spilled = false;
    assignment->phys_reg = phys_reg;
    assignment->spill_slot = -1;
}

static bool active_with_latest_end_in_class(const anvil_mir_func_t *func,
                                            const anvil_mir_vreg_t *active,
                                            size_t num_active,
                                            const live_interval_t *intervals,
                                            anvil_mir_reg_class_t reg_class,
                                            size_t *out_index)
{
    bool found = false;
    size_t latest = 0;

    for (size_t i = 0; i < num_active; i++) {
        anvil_mir_vreg_t vreg = active[i];
        if (func->assignments[vreg].reg_class != reg_class) continue;
        if (func->vregs[vreg].has_fixed_reg) continue;

        if (!found || intervals[vreg].end > intervals[active[latest]].end) {
            latest = i;
            found = true;
        }
    }

    if (found) *out_index = latest;
    return found;
}

static bool active_with_phys_reg_in_class(const anvil_mir_func_t *func,
                                          const anvil_mir_vreg_t *active,
                                          size_t num_active,
                                          anvil_mir_reg_class_t reg_class,
                                          int phys_reg,
                                          size_t *out_index)
{
    for (size_t i = 0; i < num_active; i++) {
        const anvil_regalloc_assignment_t *assignment =
            &func->assignments[active[i]];
        if (!assignment->spilled &&
            assignment->reg_class == reg_class &&
            assignment->phys_reg == phys_reg) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

static const anvil_regalloc_class_config_t *
config_for_class(const anvil_regalloc_class_config_t *configs,
                 size_t num_configs,
                 anvil_mir_reg_class_t reg_class)
{
    for (size_t i = 0; i < num_configs; i++) {
        if (configs[i].reg_class == reg_class) {
            return &configs[i];
        }
    }
    return NULL;
}

bool anvil_regalloc_linear_scan_classes(
    anvil_mir_func_t *func,
    const anvil_regalloc_class_config_t *configs,
    size_t num_configs)
{
    if (!func || !configs || num_configs == 0) return false;
    if (!anvil_mir_prepare_assignments(func)) return false;
    if (func->num_vregs == 0) return true;

    live_interval_t *intervals = calloc(func->num_vregs, sizeof(*intervals));
    anvil_mir_vreg_t *order = malloc(func->num_vregs * sizeof(*order));
    anvil_mir_vreg_t *active = malloc(func->num_vregs * sizeof(*active));
    if (!intervals || !order || !active) {
        free(intervals);
        free(order);
        free(active);
        anvil_mir_clear_allocations(func);
        return false;
    }

    for (size_t i = 0; i < func->num_vregs; i++) {
        intervals[i].vreg = (anvil_mir_vreg_t)i;
        order[i] = (anvil_mir_vreg_t)i;
    }

    for (size_t i = 0; i < func->num_instrs; i++) {
        const anvil_mir_instr_t *instr = &func->instrs[i];
        size_t use_pos = i * 2;
        size_t def_pos = use_pos + 1;
        if (instr->def != ANVIL_MIR_NO_VREG) {
            note_live_range(intervals, instr->def, def_pos);
        }
        for (size_t u = 0; u < instr->num_uses; u++) {
            note_live_range(intervals, instr->uses[u], use_pos);
        }
    }

    if (!note_cfg_live_ranges(func, intervals)) {
        free(intervals);
        free(order);
        free(active);
        anvil_mir_clear_allocations(func);
        return false;
    }

    sort_vregs_by_start(order, func->num_vregs, intervals);

    size_t num_active = 0;
    for (size_t i = 0; i < func->num_vregs; i++) {
        anvil_mir_vreg_t current = order[i];
        live_interval_t *current_interval = &intervals[current];
        if (!current_interval->live) continue;

        expire_old_intervals(active, &num_active, intervals,
                             current_interval->start);

        anvil_mir_reg_class_t reg_class = func->vregs[current].reg_class;
        const anvil_regalloc_class_config_t *config =
            config_for_class(configs, num_configs, reg_class);

        if (func->vregs[current].has_fixed_reg) {
            int fixed_reg = func->vregs[current].fixed_phys_reg;
            if (fixed_reg < 0) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                free(order);
                free(active);
                return false;
            }

            size_t conflict_index = 0;
            if (active_with_phys_reg_in_class(func, active, num_active,
                                              reg_class, fixed_reg,
                                              &conflict_index)) {
                anvil_mir_vreg_t conflict = active[conflict_index];
                if (func->vregs[conflict].has_fixed_reg) {
                    anvil_mir_clear_allocations(func);
                    free(intervals);
                    free(order);
                    free(active);
                    return false;
                }

                if (!mark_spilled(func, conflict)) {
                    anvil_mir_clear_allocations(func);
                    free(intervals);
                    free(order);
                    free(active);
                    return false;
                }
                assign_phys_reg(func, current, fixed_reg);
                active[conflict_index] = current;
            } else {
                assign_phys_reg(func, current, fixed_reg);
                active[num_active++] = current;
            }
            continue;
        }

        int free_reg = find_free_phys_reg(func, active, num_active, config);
        if (free_reg >= 0) {
            assign_phys_reg(func, current, free_reg);
            active[num_active++] = current;
            continue;
        }

        size_t spill_index = 0;
        if (!active_with_latest_end_in_class(func, active, num_active,
                                             intervals, reg_class,
                                             &spill_index)) {
            if (!mark_spilled(func, current)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                free(order);
                free(active);
                return false;
            }
            continue;
        }

        anvil_mir_vreg_t spill_candidate = active[spill_index];
        int reclaimed_reg = func->assignments[spill_candidate].phys_reg;

        if (intervals[spill_candidate].end > current_interval->end) {
            if (!mark_spilled(func, spill_candidate)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                free(order);
                free(active);
                return false;
            }
            assign_phys_reg(func, current, reclaimed_reg);
            active[spill_index] = current;
        } else {
            if (!mark_spilled(func, current)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                free(order);
                free(active);
                return false;
            }
        }
    }

    free(intervals);
    free(order);
    free(active);
    return true;
}

bool anvil_regalloc_linear_scan(anvil_mir_func_t *func, int num_phys_regs)
{
    if (!func || num_phys_regs <= 0) return false;

    anvil_regalloc_class_config_t config = {
        ANVIL_MIR_REG_GPR,
        num_phys_regs,
        NULL,
    };
    return anvil_regalloc_linear_scan_classes(func, &config, 1);
}
