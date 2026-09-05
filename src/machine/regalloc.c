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

enum { CLOBBER_REGISTERS = 64, CLOBBER_KEYS = ANVIL_MIR_REG_CLASS_COUNT * CLOBBER_REGISTERS };

typedef struct {
    size_t offsets[CLOBBER_KEYS + 1];
    size_t *positions;
} clobber_map_t;

static bool build_clobber_map(const anvil_mir_func_t *func, clobber_map_t *map)
{
    size_t counts[CLOBBER_KEYS] = { 0 };
    if (func->num_instrs > (SIZE_MAX - 2) / 2)
        return false;

    for (size_t pass = 0; pass < 2; pass++)
    {
        for (size_t index = 0; index < func->num_instrs; index++)
        {
            for (size_t reg_class = 0; reg_class < ANVIL_MIR_REG_CLASS_COUNT; reg_class++)
            {
                uint64_t mask = func->instrs[index].clobbers[reg_class];
                for (size_t reg = 0; mask; reg++, mask >>= 1)
                {
                    if (!(mask & 1))
                        continue;

                    size_t key = reg_class * CLOBBER_REGISTERS + reg;
                    if (counts[key] == SIZE_MAX)
                        return false;

                    if (pass)
                        map->positions[counts[key]] = index * 2 + 1;

                    counts[key]++;
                }
            }
        }

        if (!pass)
        {
            size_t total = 0;
            for (size_t key = 0; key < CLOBBER_KEYS; key++)
            {
                if (counts[key] > SIZE_MAX - total)
                    return false;

                size_t next = total + counts[key];
                map->offsets[key] = total;
                counts[key] = total;
                total = next;
            }
            map->offsets[CLOBBER_KEYS] = total;
            if (total > SIZE_MAX / sizeof(*map->positions))
                return false;

            map->positions = calloc(total ? total : 1, sizeof(*map->positions));
            if (!map->positions)
                return false;
        }
    }

    return true;
}

static bool register_survives(const clobber_map_t *map, anvil_mir_reg_class_t reg_class, int reg, const live_interval_t *interval)
{
    if (reg < 0)
        return false;
    if (reg >= CLOBBER_REGISTERS)
        return true;

    size_t key = (size_t)reg_class * CLOBBER_REGISTERS + (size_t)reg;
    size_t first = map->offsets[key];
    size_t end = map->offsets[key + 1];
    while (first < end)
    {
        size_t middle = first + (end - first) / 2;
        if (map->positions[middle] <= interval->start)
            first = middle + 1;
        else
            end = middle;
    }

    return first == map->offsets[key + 1] || map->positions[first] >= interval->end;
}

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

static bool note_cfg_live_ranges(anvil_mir_func_t *func, live_interval_t *intervals)
{
    if (func->num_blocks == 0 || func->num_vregs == 0)
        return true;

    anvil_mir_liveness_t liveness;
    if (!anvil_mir_compute_liveness(func, &liveness))
        return false;

    for (size_t block = 0; block < func->num_blocks; block++)
    {
        if (liveness.first_instr[block] == SIZE_MAX)
            continue;

        if (liveness.last_instr[block] > (SIZE_MAX - 2) / 2)
        {
            anvil_mir_liveness_destroy(&liveness);
            return false;
        }

        size_t start = liveness.first_instr[block] * 2;
        size_t end = liveness.last_instr[block] * 2 + 2;
        size_t row = block * liveness.words_per_block;
        for (size_t value = 0; value < func->num_vregs; value++)
        {
            uint64_t bit = UINT64_C(1) << (value % 64);
            size_t cell = row + value / 64;
            if (liveness.live_in[cell] & bit)
                note_live_range(intervals, (anvil_mir_vreg_t)value, start);

            if (liveness.live_out[cell] & bit)
                note_live_range(intervals, (anvil_mir_vreg_t)value, end);
        }
    }

    anvil_mir_liveness_destroy(&liveness);
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

static void sift_interval_heap(anvil_mir_vreg_t *order, size_t root, size_t count, const live_interval_t *intervals)
{
    while (root < count / 2)
    {
        size_t child = root * 2 + 1;
        if (child + 1 < count && compare_intervals(intervals, order[child], order[child + 1]) < 0)
            child++;

        if (compare_intervals(intervals, order[root], order[child]) >= 0)
            break;

        anvil_mir_vreg_t value = order[root];
        order[root] = order[child];
        order[child] = value;
        root = child;
    }
}

static void sort_vregs_by_start(anvil_mir_vreg_t *order, size_t count, const live_interval_t *intervals)
{
    if (count < 2)
        return;

    for (size_t root = count / 2; root > 0; root--)
        sift_interval_heap(order, root - 1, count, intervals);

    for (size_t remaining = count; remaining > 1; remaining--)
    {
        anvil_mir_vreg_t value = order[remaining - 1];
        order[remaining - 1] = order[0];
        order[0] = value;
        sift_interval_heap(order, 0, remaining - 1, intervals);
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
                              const anvil_regalloc_class_config_t *config,
                              const clobber_map_t *clobbers,
                              const live_interval_t *interval)
{
    if (!config || config->num_phys_regs <= 0) return -1;

    for (int i = 0; i < config->num_phys_regs; i++) {
        int candidate = phys_reg_at(config, i);
        if (!register_survives(clobbers, config->reg_class, candidate, interval))
            continue;

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

typedef struct {
    unsigned key;
    size_t *slots;
    size_t count;
    size_t capacity;
} spill_slot_group_t;

static bool earlier_slot(const size_t *ends, size_t left, size_t right)
{
    return ends[left] < ends[right] || (ends[left] == ends[right] && left < right);
}

static void update_slot_heap(spill_slot_group_t *group, const size_t *ends)
{
    size_t root = 0;
    while (root < group->count / 2)
    {
        size_t child = root * 2 + 1;
        if (child + 1 < group->count && earlier_slot(ends, group->slots[child + 1], group->slots[child]))
            child++;
        if (!earlier_slot(ends, group->slots[child], group->slots[root]))
            break;

        size_t slot = group->slots[root];
        group->slots[root] = group->slots[child];
        group->slots[child] = slot;
        root = child;
    }
}

static bool append_slot_heap(spill_slot_group_t *group, const size_t *ends, size_t slot)
{
    if (group->count == group->capacity)
    {
        size_t capacity = group->capacity ? group->capacity * 2 : 4;
        if (capacity < group->capacity || capacity > SIZE_MAX / sizeof(*group->slots))
            return false;

        size_t *grown = realloc(group->slots, capacity * sizeof(*grown));
        if (!grown)
            return false;

        group->slots = grown;
        group->capacity = capacity;
    }

    size_t index = group->count++;
    while (index)
    {
        size_t parent = (index - 1) / 2;
        if (!earlier_slot(ends, slot, group->slots[parent]))
            break;

        group->slots[index] = group->slots[parent];
        index = parent;
    }

    group->slots[index] = slot;
    return true;
}

/* A min-heap per class/width reuses storage only after a convex interval ends.
 * CFG liveness has already extended intervals over backedges and block exits. */
static bool reuse_spill_slots(anvil_mir_func_t *func, const live_interval_t *intervals, const anvil_mir_vreg_t *order)
{
    if (func->num_spills < 2)
        return true;

    size_t capacity = 4;
    while (capacity / 2 < func->num_spills)
    {
        if (capacity > SIZE_MAX / (2 * sizeof(spill_slot_group_t)))
            return false;

        capacity *= 2;
    }

    spill_slot_group_t *groups = calloc(capacity, sizeof(*groups));
    size_t *ends = calloc(func->num_spills, sizeof(*ends));
    if (!groups || !ends)
    {
        free(groups);
        free(ends);
        return false;
    }

    bool ok = true;
    size_t count = func->num_pinned_spills;
    for (size_t index = 0; index < func->num_vregs; index++)
    {
        anvil_mir_vreg_t vreg = order[index];
        anvil_regalloc_assignment_t *assignment = &func->assignments[vreg];
        if (!assignment->spilled)
            continue;

        const anvil_mir_vreg_info_t *info = &func->vregs[vreg];
        unsigned key = ((unsigned)info->reg_class << 16) | info->size_bits;
        size_t bucket = ((size_t)key * UINT32_C(0x9e3779b1)) & (capacity - 1);
        while (groups[bucket].key && groups[bucket].key != key)
            bucket = (bucket + 1) & (capacity - 1);

        spill_slot_group_t *group = &groups[bucket];
        group->key = key;
        size_t slot;
        if (group->count && ends[group->slots[0]] < intervals[vreg].start)
        {
            slot = group->slots[0];
            ends[slot] = intervals[vreg].end;
            update_slot_heap(group, ends);
        }
        else
        {
            slot = count++;
            ends[slot] = intervals[vreg].end;
            func->spill_slots[slot].reg_class = info->reg_class;
            func->spill_slots[slot].size_bits = info->size_bits;
            if (!append_slot_heap(group, ends, slot))
            {
                ok = false;
                break;
            }
        }

        assignment->spill_slot = (int)slot;
    }

    if (ok)
        func->num_spills = count;

    for (size_t index = 0; index < capacity; index++)
        free(groups[index].slots);

    free(groups);
    free(ends);
    return ok;
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
                                            const clobber_map_t *clobbers,
                                            const live_interval_t *current,
                                            size_t *out_index)
{
    bool found = false;
    size_t latest = 0;

    for (size_t i = 0; i < num_active; i++) {
        anvil_mir_vreg_t vreg = active[i];
        if (func->assignments[vreg].reg_class != reg_class) continue;
        if (func->vregs[vreg].has_fixed_reg) continue;
        if (!register_survives(clobbers, reg_class, func->assignments[vreg].phys_reg, current))
            continue;

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

static bool linear_scan_classes_once(
    anvil_mir_func_t *func,
    const anvil_regalloc_class_config_t *configs,
    size_t num_configs)
{
    if (!func || !configs || num_configs == 0) return false;
    if (!anvil_mir_prepare_assignments(func)) return false;
    if (func->num_vregs == 0) return true;

    clobber_map_t clobbers = { 0 };
    if (!build_clobber_map(func, &clobbers))
    {
        free(clobbers.positions);
        anvil_mir_clear_allocations(func);
        return false;
    }

    live_interval_t *intervals = calloc(func->num_vregs, sizeof(*intervals));
    anvil_mir_vreg_t *order = malloc(func->num_vregs * sizeof(*order));
    anvil_mir_vreg_t *active = malloc(func->num_vregs * sizeof(*active));
    if (!intervals || !order || !active) {
        free(intervals);
        free(clobbers.positions);
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
        free(clobbers.positions);
        free(order);
        free(active);
        anvil_mir_clear_allocations(func);
        return false;
    }

    for (size_t value = 0; value < func->num_vregs; value++)
    {
        if (func->vregs[value].is_live_in && intervals[value].live)
            intervals[value].start = 0;
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
            if (!register_survives(&clobbers, reg_class, fixed_reg, current_interval)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                free(clobbers.positions);
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
                    free(clobbers.positions);
                    free(order);
                    free(active);
                    return false;
                }

                if (!mark_spilled(func, conflict)) {
                    anvil_mir_clear_allocations(func);
                    free(intervals);
                    free(clobbers.positions);
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

        int free_reg = find_free_phys_reg(func, active, num_active, config, &clobbers, current_interval);
        if (free_reg >= 0) {
            assign_phys_reg(func, current, free_reg);
            active[num_active++] = current;
            continue;
        }

        size_t spill_index = 0;
        if (!active_with_latest_end_in_class(func, active, num_active,
                                             intervals, reg_class, &clobbers, current_interval,
                                             &spill_index)) {
            if (!mark_spilled(func, current)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                free(clobbers.positions);
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
                free(clobbers.positions);
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
                free(clobbers.positions);
                free(order);
                free(active);
                return false;
            }
        }
    }

    bool compacted = reuse_spill_slots(func, intervals, order);
    free(intervals);
    free(clobbers.positions);
    free(order);
    free(active);
    if (!compacted)
        anvil_mir_clear_allocations(func);

    return compacted;
}

bool anvil_regalloc_linear_scan_classes(anvil_mir_func_t *func, const anvil_regalloc_class_config_t *configs, size_t num_configs)
{
    if (!linear_scan_classes_once(func, configs, num_configs))
        return false;

    bool changed = false;
    if (!anvil_mir_split_spilled_intervals(func, &changed))
        return false;

    return !changed || linear_scan_classes_once(func, configs, num_configs);
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
