/*
 * ANVIL - Linear-scan register allocation for MachineIR.
 */

#include "machine_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
    /* Positions of instructions that clobber some register of a class
     * (calls), for registers a call preserves only partially. */
    size_t *calls[ANVIL_MIR_REG_CLASS_COUNT];
    size_t num_calls[ANVIL_MIR_REG_CLASS_COUNT];
    uint64_t partial[ANVIL_MIR_REG_CLASS_COUNT];
    uint16_t preserved_bits[ANVIL_MIR_REG_CLASS_COUNT];
    /* anvil_regalloc_class_config_t.narrow_bits/narrow_regs per class. */
    uint16_t narrow_bits[ANVIL_MIR_REG_CLASS_COUNT];
    uint64_t narrow_regs[ANVIL_MIR_REG_CLASS_COUNT];
    const anvil_mir_func_t *func;
    /* For each clobbering instruction (row[index] != SIZE_MAX), the vregs
     * live across it: an interval spanning a clobber its value is not live
     * across (a hole) keeps a clobbered register.  NULL: every value whose
     * interval spans a clobber is taken to be live across it. */
    size_t *row;
    uint64_t *across;
    size_t words;
    /* Per register, the instructions that use it as printer scratch. */
    size_t scratch_offsets[CLOBBER_KEYS + 1];
    size_t *scratch;
} clobber_map_t;

/* Live-across sets beyond this many words are not kept (the conservative
 * whole-interval test applies instead). */
enum { CLOBBER_ACROSS_LIMIT_WORDS = 1u << 20 };

static void clobber_map_destroy(clobber_map_t *map)
{
    free(map->positions);
    map->positions = NULL;
    free(map->scratch);
    map->scratch = NULL;
    free(map->row);
    free(map->across);
    map->row = NULL;
    map->across = NULL;
    for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++) {
        free(map->calls[c]);
        map->calls[c] = NULL;
    }
}

/* Records the call positions and partially preserved registers. */
static bool clobber_map_add_partial(clobber_map_t *map, const anvil_mir_func_t *func, const anvil_regalloc_class_config_t *configs, size_t num_configs)
{
    map->func = func;
    for (size_t i = 0; i < num_configs; i++) {
        size_t c = configs[i].reg_class;
        if (c < ANVIL_MIR_REG_CLASS_COUNT) {
            map->narrow_bits[c] = configs[i].narrow_bits;
            map->narrow_regs[c] = configs[i].narrow_regs;
        }
        if (c >= ANVIL_MIR_REG_CLASS_COUNT || !configs[i].partially_preserved)
            continue;
        map->partial[c] = configs[i].partially_preserved;
        map->preserved_bits[c] = configs[i].preserved_bits;
        map->calls[c] = malloc((func->num_instrs ? func->num_instrs : 1) * sizeof(size_t));
        if (!map->calls[c])
            return false;
        for (size_t index = 0; index < func->num_instrs; index++)
            if (func->instrs[index].clobbers[c])
                map->calls[c][map->num_calls[c]++] = index * 2 + 1;
    }
    return true;
}

/* The instructions whose clobbers (or scratch) name each register, in
 * order: `offsets[key]` starts register key's run in the returned array. */
static size_t *index_by_register(const anvil_mir_func_t *func, bool scratch, size_t offsets[CLOBBER_KEYS + 1])
{
    size_t counts[CLOBBER_KEYS] = { 0 };
    size_t *positions = NULL;
    for (size_t pass = 0; pass < 2; pass++)
    {
        for (size_t index = 0; index < func->num_instrs; index++)
        {
            const uint64_t *masks = scratch ? func->instrs[index].scratch : func->instrs[index].clobbers;
            for (size_t reg_class = 0; reg_class < ANVIL_MIR_REG_CLASS_COUNT; reg_class++)
            {
                uint64_t mask = masks[reg_class];
                for (size_t reg = 0; mask; reg++, mask >>= 1)
                {
                    if (!(mask & 1))
                        continue;

                    size_t key = reg_class * CLOBBER_REGISTERS + reg;
                    if (pass)
                        positions[counts[key]] = index;
                    counts[key]++;
                }
            }
        }

        if (!pass)
        {
            size_t total = 0;
            for (size_t key = 0; key < CLOBBER_KEYS; key++)
            {
                size_t next = total + counts[key];
                offsets[key] = total;
                counts[key] = total;
                total = next;
            }
            offsets[CLOBBER_KEYS] = total;
            positions = calloc(total ? total : 1, sizeof(*positions));
            if (!positions)
                return NULL;
        }
    }
    return positions;
}

static bool build_clobber_map(const anvil_mir_func_t *func, clobber_map_t *map)
{
    if (func->num_instrs > (SIZE_MAX - 2) / 2)
        return false;

    map->func = func;
    map->positions = index_by_register(func, false, map->offsets);
    map->scratch = index_by_register(func, true, map->scratch_offsets);
    if (!map->positions || !map->scratch)
        return false;
    /* Clobbers sit between an instruction's uses and its result. */
    for (size_t i = 0; i < map->offsets[CLOBBER_KEYS]; i++)
        map->positions[i] = map->positions[i] * 2 + 1;
    return true;
}

static bool instr_clobbers(const anvil_mir_instr_t *instr)
{
    for (size_t reg_class = 0; reg_class < ANVIL_MIR_REG_CLASS_COUNT; reg_class++) {
        if (instr->clobbers[reg_class] || instr->scratch[reg_class])
            return true;
    }
    return false;
}

/* Walks each block backward from its live-out set, recording at every
 * clobbering instruction the values live after it other than its own
 * result.  Without memory (or past the size limit) the map keeps none. */
static void clobber_map_add_across(clobber_map_t *map, const anvil_mir_func_t *func, const anvil_mir_liveness_t *liveness)
{
    size_t rows = 0, words = liveness->words_per_block;
    for (size_t index = 0; index < func->num_instrs; index++)
        rows += instr_clobbers(&func->instrs[index]);
    if (!rows || !words || rows > CLOBBER_ACROSS_LIMIT_WORDS / words)
        return;
    size_t *row = malloc(func->num_instrs * sizeof(*row));
    uint64_t *across = malloc(rows * words * sizeof(*across));
    uint64_t *live = malloc(words * sizeof(*live));
    if (!row || !across || !live) {
        free(row);
        free(across);
        free(live);
        return;
    }
    size_t next = 0;
    for (size_t index = 0; index < func->num_instrs; index++)
        row[index] = instr_clobbers(&func->instrs[index]) ? next++ : SIZE_MAX;
    /* A clobber no block walk reaches keeps everything live. */
    memset(across, 0xff, rows * words * sizeof(*across));
    for (size_t block = 0; block < func->num_blocks; block++) {
        if (liveness->first_instr[block] == SIZE_MAX)
            continue;
        memcpy(live, &liveness->live_out[block * words], words * sizeof(*live));
        for (size_t index = liveness->last_instr[block] + 1; index-- > liveness->first_instr[block];) {
            const anvil_mir_instr_t *instr = &func->instrs[index];
            if (instr->block != block)
                continue;
            if (instr->def != ANVIL_MIR_NO_VREG && instr->def < func->num_vregs)
                live[instr->def / 64] &= ~(UINT64_C(1) << (instr->def % 64));
            if (row[index] != SIZE_MAX)
                memcpy(&across[row[index] * words], live, words * sizeof(*live));
            for (size_t use = 0; use < instr->num_uses; use++) {
                if (instr->uses[use] < func->num_vregs)
                    live[instr->uses[use] / 64] |= UINT64_C(1) << (instr->uses[use] % 64);
            }
        }
    }
    free(live);
    map->row = row;
    map->across = across;
    map->words = words;
}

/* Whether the clobber at `position` (an odd position: instruction index * 2
 * + 1) can destroy `interval`'s value. */
static bool clobber_hits(const clobber_map_t *map, size_t position, const live_interval_t *interval)
{
    size_t row = map->row ? map->row[position / 2] : SIZE_MAX;
    return row == SIZE_MAX || (map->across[row * map->words + interval->vreg / 64] >> (interval->vreg % 64) & 1);
}

static bool register_survives(const clobber_map_t *map, anvil_mir_reg_class_t reg_class, int reg, const live_interval_t *interval)
{
    if (reg < 0)
        return false;
    if (reg >= CLOBBER_REGISTERS)
        return true;
    if (map->func && map->narrow_bits[reg_class] && interval->vreg < map->func->num_vregs &&
        map->func->vregs[interval->vreg].size_bits <= map->narrow_bits[reg_class] && !(map->narrow_regs[reg_class] >> reg & 1))
        return false;

    /* A value wider than a partially preserved register keeps must not
     * cross any call. */
    if (map->func && (map->partial[reg_class] >> reg & 1) && interval->vreg < map->func->num_vregs &&
        map->func->vregs[interval->vreg].size_bits > map->preserved_bits[reg_class]) {
        const size_t *calls = map->calls[reg_class];
        size_t low = 0, high = map->num_calls[reg_class];
        while (low < high) {
            size_t middle = low + (high - low) / 2;
            if (calls[middle] <= interval->start)
                low = middle + 1;
            else
                high = middle;
        }
        for (; low < map->num_calls[reg_class] && calls[low] < interval->end; low++) {
            if (clobber_hits(map, calls[low], interval))
                return false;
        }
    }

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

    for (; first < map->offsets[key + 1] && map->positions[first] < interval->end; first++) {
        if (clobber_hits(map, map->positions[first], interval))
            return false;
    }

    /* Printer scratch at instruction i also excludes its operands (read
     * at 2i) and its result (written at 2i + 1). */
    first = map->scratch_offsets[key];
    end = map->scratch_offsets[key + 1];
    while (first < end)
    {
        size_t middle = first + (end - first) / 2;
        if (map->scratch[middle] * 2 + 1 < interval->start)
            first = middle + 1;
        else
            end = middle;
    }
    for (; first < map->scratch_offsets[key + 1] && map->scratch[first] * 2 <= interval->end; first++) {
        size_t index = map->scratch[first];
        const anvil_mir_instr_t *instr = &map->func->instrs[index];
        if (instr->def == interval->vreg)
            return false;
        for (size_t use = 0; use < instr->num_uses; use++)
            if (instr->uses[use] == interval->vreg)
                return false;
        if (index * 2 + 1 > interval->start && index * 2 + 1 < interval->end && clobber_hits(map, index * 2 + 1, interval))
            return false;
    }
    return true;
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

/* Extends intervals over the blocks their values are live into or out of,
 * and records in `clobbers` the values live across each clobber. */
static bool note_cfg_live_ranges(anvil_mir_func_t *func, live_interval_t *intervals, clobber_map_t *clobbers)
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

    clobber_map_add_across(clobbers, func, &liveness);
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

/* The first free pool register for `interval`; `avoid` (or -1) is taken
 * only when nothing else is free. */
static int find_free_phys_reg(const anvil_mir_func_t *func,
                              const anvil_mir_vreg_t *active,
                              size_t num_active,
                              const anvil_regalloc_class_config_t *config,
                              const clobber_map_t *clobbers,
                              const live_interval_t *interval,
                              int avoid)
{
    if (!config || config->num_phys_regs <= 0) return -1;

    int fallback = -1;
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

        if (!used && candidate != avoid) {
            return candidate;
        }
        if (!used)
            fallback = candidate;
    }

    return fallback;
}

/* A fixed-register interval displaces an allocated one: move the displaced
 * value to another register that is free over its whole interval instead
 * of spilling it. Intervals already processed are the only ones holding
 * registers, so one pass over them finds the registers it overlaps. */
static int relocation_register(const anvil_mir_func_t *func, const anvil_mir_vreg_t *order, size_t processed,
                               const live_interval_t *intervals, const anvil_regalloc_class_config_t *config,
                               const clobber_map_t *clobbers, anvil_mir_vreg_t displaced, int taken)
{
    if (!config || config->num_phys_regs <= 0)
        return -1;

    const live_interval_t *range = &intervals[displaced];
    uint64_t busy = 0;
    for (size_t index = 0; index < processed; index++)
    {
        anvil_mir_vreg_t other = order[index];
        const anvil_regalloc_assignment_t *assignment = &func->assignments[other];
        if (other == displaced || assignment->spilled || assignment->phys_reg < 0 || assignment->phys_reg >= 64 ||
            assignment->reg_class != config->reg_class || !intervals[other].live ||
            intervals[other].start > range->end || intervals[other].end < range->start)
            continue;

        busy |= UINT64_C(1) << assignment->phys_reg;
    }

    for (int index = 0; index < config->num_phys_regs; index++)
    {
        int candidate = phys_reg_at(config, index);
        if (candidate != taken && candidate >= 0 && candidate < 64 && !(busy & (UINT64_C(1) << candidate)) &&
            register_survives(clobbers, config->reg_class, candidate, range))
            return candidate;
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

/* The active value to give up its register for `current`: a recomputable
 * one first (a spill of it costs a recomputation per use, no memory
 * traffic), else the one ending last. */
static bool active_with_latest_end_in_class(const anvil_mir_func_t *func,
                                            const anvil_mir_vreg_t *active,
                                            size_t num_active,
                                            const live_interval_t *intervals,
                                            const bool *recomputable,
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

        bool cheaper = found && recomputable && recomputable[vreg] && !recomputable[active[latest]];
        bool dearer = found && recomputable && !recomputable[vreg] && recomputable[active[latest]];
        if (!found || cheaper || (!dearer && intervals[vreg].end > intervals[active[latest]].end)) {
            latest = i;
            found = true;
        }
    }

    if (found) *out_index = latest;
    return found;
}

/* Values whose only definition reads nothing (anvil_mir_instr_recomputable:
 * constants, symbol and frame addresses).  NULL without memory. */
static bool *recomputable_values(const anvil_mir_func_t *func)
{
    bool *values = calloc(func->num_vregs, sizeof(*values));
    uint8_t *definitions = calloc(func->num_vregs, sizeof(*definitions));
    if (!values || !definitions) {
        free(values);
        free(definitions);
        return NULL;
    }
    for (size_t index = 0; index < func->num_instrs; index++) {
        anvil_mir_vreg_t def = func->instrs[index].def;
        if (def == ANVIL_MIR_NO_VREG || def >= func->num_vregs)
            continue;
        if (definitions[def] < 2)
            definitions[def]++;
        values[def] = definitions[def] == 1 && !func->vregs[def].is_live_in && anvil_mir_instr_recomputable(func, index);
    }
    free(definitions);
    return values;
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

/* Copies between a value and a fixed register (arguments, results,
 * returns) or between two values (PHI moves) disappear when both sides get
 * one register, so each value remembers the register or the partner its
 * copies want.  In a two-address class an arithmetic result also wants its
 * first operand's register (free once that operand dies there). */
typedef struct {
    int fixed;
    anvil_mir_vreg_t partner;
    anvil_mir_vreg_t avoid;     /* two-address: the second operand */
} copy_hint_t;

static bool overwrites_first_operand(anvil_mir_opcode_t op)
{
    switch (op) {
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_MUL:
    case ANVIL_MIR_OP_DIV:
    case ANVIL_MIR_OP_FDIV:
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR:
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
    case ANVIL_MIR_OP_NEG:
    case ANVIL_MIR_OP_NOT:
    case ANVIL_MIR_OP_FABS:
        return true;
    default:
        return false;
    }
}

static copy_hint_t *build_copy_hints(const anvil_mir_func_t *func, const anvil_regalloc_class_config_t *configs, size_t num_configs)
{
    copy_hint_t *hints = malloc(func->num_vregs * sizeof(*hints));
    if (!hints)
        return NULL;
    for (size_t value = 0; value < func->num_vregs; value++) {
        hints[value].fixed = -1;
        hints[value].partner = ANVIL_MIR_NO_VREG;
        hints[value].avoid = ANVIL_MIR_NO_VREG;
    }
    for (size_t index = 0; index < func->num_instrs; index++) {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        if (instr->def != ANVIL_MIR_NO_VREG && instr->num_uses >= 1 && overwrites_first_operand(instr->op)) {
            anvil_mir_vreg_t def = instr->def, first = instr->uses[0];
            const anvil_regalloc_class_config_t *config = config_for_class(configs, num_configs, func->vregs[def].reg_class);
            if (config && config->two_address && func->vregs[first].reg_class == func->vregs[def].reg_class &&
                !func->vregs[def].has_fixed_reg && hints[def].partner == ANVIL_MIR_NO_VREG) {
                hints[def].partner = first;
                /* A result in the second operand's register makes the
                 * printer go through a temporary when the first lives on. */
                if (instr->num_uses >= 2 && instr->uses[1] != first)
                    hints[def].avoid = instr->uses[1];
            }
            continue;
        }
        /* A truncation defines only the low bits, which the source's
         * register already holds (readers needing more extend explicitly). */
        bool truncation = instr->op == ANVIL_MIR_OP_TRUNC && instr->def != ANVIL_MIR_NO_VREG && instr->num_uses == 1 &&
                          func->vregs[instr->def].reg_class == ANVIL_MIR_REG_GPR;
        if ((instr->op != ANVIL_MIR_OP_COPY && !truncation) || instr->def == ANVIL_MIR_NO_VREG || instr->num_uses != 1)
            continue;
        anvil_mir_vreg_t def = instr->def, use = instr->uses[0];
        const anvil_mir_vreg_info_t *d = &func->vregs[def], *u = &func->vregs[use];
        if (d->reg_class != u->reg_class)
            continue;
        if (d->has_fixed_reg && !u->has_fixed_reg) {
            if (hints[use].fixed < 0)
                hints[use].fixed = d->fixed_phys_reg;
        } else if (u->has_fixed_reg && !d->has_fixed_reg) {
            if (hints[def].fixed < 0)
                hints[def].fixed = u->fixed_phys_reg;
        } else if (!d->has_fixed_reg && !u->has_fixed_reg) {
            if (hints[def].partner == ANVIL_MIR_NO_VREG)
                hints[def].partner = use;
            if (hints[use].partner == ANVIL_MIR_NO_VREG)
                hints[use].partner = def;
        }
    }
    return hints;
}

/* `candidate` is in the class pool, survives the interval's clobbers and
 * no active interval holds it. */
static bool phys_reg_available(const anvil_mir_func_t *func, const anvil_mir_vreg_t *active, size_t num_active,
                               const anvil_regalloc_class_config_t *config, const clobber_map_t *clobbers,
                               const live_interval_t *interval, int candidate)
{
    if (!config || candidate < 0)
        return false;
    bool in_pool = false;
    for (int i = 0; i < config->num_phys_regs && !in_pool; i++)
        in_pool = phys_reg_at(config, i) == candidate;
    if (!in_pool || !register_survives(clobbers, config->reg_class, candidate, interval))
        return false;
    for (size_t a = 0; a < num_active; a++) {
        const anvil_regalloc_assignment_t *assignment = &func->assignments[active[a]];
        if (!assignment->spilled && assignment->reg_class == config->reg_class && assignment->phys_reg == candidate)
            return false;
    }
    return true;
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
    if (!build_clobber_map(func, &clobbers) || !clobber_map_add_partial(&clobbers, func, configs, num_configs))
    {
        clobber_map_destroy(&clobbers);
        anvil_mir_clear_allocations(func);
        return false;
    }

    /* Hints only steer the choice; without memory for them allocation
     * proceeds unhinted. */
    copy_hint_t *hints = build_copy_hints(func, configs, num_configs);
    bool *recomputable = recomputable_values(func);
    live_interval_t *intervals = calloc(func->num_vregs, sizeof(*intervals));
    anvil_mir_vreg_t *order = malloc(func->num_vregs * sizeof(*order));
    anvil_mir_vreg_t *active = malloc(func->num_vregs * sizeof(*active));
    if (!intervals || !order || !active) {
        free(intervals);
        clobber_map_destroy(&clobbers);
        free(order);
        free(active);
        free(hints);
        free(recomputable);
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

    if (!note_cfg_live_ranges(func, intervals, &clobbers)) {
        free(intervals);
        clobber_map_destroy(&clobbers);
        free(order);
        free(active);
        free(hints);
        free(recomputable);
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
                clobber_map_destroy(&clobbers);
                free(order);
                free(active);
        free(hints);
        free(recomputable);
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
                    clobber_map_destroy(&clobbers);
                    free(order);
                    free(active);
        free(hints);
        free(recomputable);
                    return false;
                }

                int moved = relocation_register(func, order, i, intervals, config, &clobbers, conflict, fixed_reg);
                if (moved >= 0)
                    assign_phys_reg(func, conflict, moved);
                else if (!mark_spilled(func, conflict)) {
                    anvil_mir_clear_allocations(func);
                    free(intervals);
                    clobber_map_destroy(&clobbers);
                    free(order);
                    free(active);
        free(hints);
        free(recomputable);
                    return false;
                }
                assign_phys_reg(func, current, fixed_reg);
                if (moved >= 0)
                    active[num_active++] = current;
                else
                    active[conflict_index] = current;
            } else {
                assign_phys_reg(func, current, fixed_reg);
                active[num_active++] = current;
            }
            continue;
        }

        int free_reg = -1;
        if (hints) {
            const copy_hint_t *hint = &hints[current];
            if (phys_reg_available(func, active, num_active, config, &clobbers, current_interval, hint->fixed))
                free_reg = hint->fixed;
            else if (hint->partner != ANVIL_MIR_NO_VREG && !func->assignments[hint->partner].spilled &&
                     phys_reg_available(func, active, num_active, config, &clobbers, current_interval,
                                        func->assignments[hint->partner].phys_reg))
                free_reg = func->assignments[hint->partner].phys_reg;
        }
        if (free_reg < 0) {
            anvil_mir_vreg_t avoid = hints ? hints[current].avoid : ANVIL_MIR_NO_VREG;
            int avoid_reg = avoid != ANVIL_MIR_NO_VREG && !func->assignments[avoid].spilled ? func->assignments[avoid].phys_reg : -1;
            free_reg = find_free_phys_reg(func, active, num_active, config, &clobbers, current_interval, avoid_reg);
        }
        if (free_reg >= 0) {
            assign_phys_reg(func, current, free_reg);
            active[num_active++] = current;
            continue;
        }

        size_t spill_index = 0;
        if ((recomputable && recomputable[current]) ||
            !active_with_latest_end_in_class(func, active, num_active,
                                             intervals, recomputable, reg_class, &clobbers, current_interval,
                                             &spill_index)) {
            if (!mark_spilled(func, current)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                clobber_map_destroy(&clobbers);
                free(order);
                free(active);
        free(hints);
        free(recomputable);
                return false;
            }
            continue;
        }

        anvil_mir_vreg_t spill_candidate = active[spill_index];
        int reclaimed_reg = func->assignments[spill_candidate].phys_reg;

        if (intervals[spill_candidate].end > current_interval->end || (recomputable && recomputable[spill_candidate])) {
            if (!mark_spilled(func, spill_candidate)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                clobber_map_destroy(&clobbers);
                free(order);
                free(active);
        free(hints);
        free(recomputable);
                return false;
            }
            assign_phys_reg(func, current, reclaimed_reg);
            active[spill_index] = current;
        } else {
            if (!mark_spilled(func, current)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                clobber_map_destroy(&clobbers);
                free(order);
                free(active);
        free(hints);
        free(recomputable);
                return false;
            }
        }
    }

    bool compacted = reuse_spill_slots(func, intervals, order);
    free(intervals);
    clobber_map_destroy(&clobbers);
    free(order);
    free(active);
        free(hints);
        free(recomputable);
    if (!compacted)
        anvil_mir_clear_allocations(func);

    return compacted;
}

/* Classes in which no allocatable register survives the calls of cold
 * blocks (SysV XMM): only their values gain from copies around those calls;
 * elsewhere a register the calls preserve holds the value with no copy. */
static void cold_split_classes(const anvil_mir_func_t *func, const anvil_regalloc_class_config_t *configs, size_t num_configs, bool *split)
{
    uint64_t clobbered[ANVIL_MIR_REG_CLASS_COUNT] = {0};
    for (size_t index = 0; index < func->num_instrs; index++) {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        if (instr->op == ANVIL_MIR_OP_CALL && instr->block < func->num_blocks && func->blocks[instr->block].cold) {
            for (size_t reg_class = 0; reg_class < ANVIL_MIR_REG_CLASS_COUNT; reg_class++)
                clobbered[reg_class] |= instr->clobbers[reg_class];
        }
    }
    for (size_t reg_class = 0; reg_class < ANVIL_MIR_REG_CLASS_COUNT; reg_class++) {
        const anvil_regalloc_class_config_t *config = config_for_class(configs, num_configs, (anvil_mir_reg_class_t)reg_class);
        bool survivor = false;
        for (int index = 0; config && index < config->num_phys_regs; index++) {
            int reg = phys_reg_at(config, index);
            survivor |= reg >= 0 && reg < 64 && !(clobbered[reg_class] >> reg & 1) && !(config->partially_preserved >> reg & 1);
        }
        split[reg_class] = !survivor;
    }
}

bool anvil_regalloc_linear_scan_classes(anvil_mir_func_t *func, const anvil_regalloc_class_config_t *configs, size_t num_configs)
{
    bool split[ANVIL_MIR_REG_CLASS_COUNT];
    if (func)
        cold_split_classes(func, configs, num_configs, split);
    if (!func || !anvil_mir_sink_compares(func) || !anvil_mir_split_around_cold_calls(func, split) || !linear_scan_classes_once(func, configs, num_configs))
        return false;

    bool changed = false;
    if (!anvil_mir_split_spilled_intervals(func, &changed))
        return false;

    return !changed || linear_scan_classes_once(func, configs, num_configs);
}

bool anvil_regalloc_allocate(anvil_mir_func_t *func, const anvil_regalloc_class_config_t *configs, size_t num_configs,
                             const anvil_regalloc_class_config_t *scratch_configs, size_t num_scratch_configs)
{
    if (!func || !configs || num_configs == 0 || num_configs > ANVIL_MIR_REG_CLASS_COUNT || !scratch_configs ||
        num_scratch_configs > ANVIL_MIR_REG_CLASS_COUNT || !anvil_regalloc_linear_scan_classes(func, configs, num_configs))
        return false;

    /* Reloads keep their class's narrow-register restriction. */
    anvil_regalloc_class_config_t reloads[ANVIL_MIR_REG_CLASS_COUNT];
    for (size_t j = 0; j < num_scratch_configs; j++) {
        reloads[j] = scratch_configs[j];
        for (size_t i = 0; i < num_configs; i++) {
            if (configs[i].reg_class == reloads[j].reg_class && !reloads[j].narrow_bits) {
                reloads[j].narrow_bits = configs[i].narrow_bits;
                reloads[j].narrow_regs = configs[i].narrow_regs;
            }
        }
    }
    scratch_configs = reloads;

    /* Where reloads do not fit, the values in the way spill, a few rounds
     * (each spill adds reloads of its own). */
    for (int round = 0; round < 8; round++) {
        if (anvil_mir_materialize_spills(func, scratch_configs, num_scratch_configs))
            return true;
        bool changed = false;
        if (!anvil_mir_spill_for_scratch(func, scratch_configs, num_scratch_configs, &changed) || !changed)
            break;
    }

    /* Then each pool without its class's first 1, 2, ... scratch registers
     * (in their configured order), until the reloads fit; at the end none
     * of the scratch registers holds a value. */
    int reserve_limit = 0;
    for (size_t j = 0; j < num_scratch_configs; j++)
        if (scratch_configs[j].num_phys_regs > reserve_limit)
            reserve_limit = scratch_configs[j].num_phys_regs;
    anvil_regalloc_class_config_t reduced[ANVIL_MIR_REG_CLASS_COUNT];
    int *registers[ANVIL_MIR_REG_CLASS_COUNT] = { 0 };
    bool ok = true, allocated = false;
    for (size_t i = 0; ok && i < num_configs; i++) {
        registers[i] = malloc((configs[i].num_phys_regs ? (size_t)configs[i].num_phys_regs : 1) * sizeof(int));
        ok = registers[i] != NULL;
    }
    for (int reserve = 1; ok && !allocated && reserve <= reserve_limit; reserve++) {
        for (size_t i = 0; ok && i < num_configs; i++) {
            const anvil_regalloc_class_config_t *scratch = NULL;
            for (size_t j = 0; j < num_scratch_configs; j++)
                if (scratch_configs[j].reg_class == configs[i].reg_class)
                    scratch = &scratch_configs[j];
            int reserved_count = scratch ? (reserve < scratch->num_phys_regs ? reserve : scratch->num_phys_regs) : 0;
            int count = 0;
            for (int r = 0; r < configs[i].num_phys_regs; r++) {
                int reg = phys_reg_at(&configs[i], r);
                bool reserved = false;
                for (int k = 0; k < reserved_count; k++)
                    reserved |= phys_reg_at(scratch, k) == reg;
                if (!reserved)
                    registers[i][count++] = reg;
            }
            reduced[i] = configs[i];
            reduced[i].phys_regs = registers[i];
            reduced[i].num_phys_regs = count;
            ok = count > 0;
        }
        ok = ok && anvil_regalloc_linear_scan_classes(func, reduced, num_configs);
        allocated = ok && anvil_mir_materialize_spills(func, scratch_configs, num_scratch_configs);
    }
    for (size_t i = 0; i < num_configs; i++)
        free(registers[i]);
    return ok && allocated;
}

bool anvil_regalloc_linear_scan(anvil_mir_func_t *func, int num_phys_regs)
{
    if (!func || num_phys_regs <= 0) return false;

    anvil_regalloc_class_config_t config = {
        .reg_class = ANVIL_MIR_REG_GPR,
        .num_phys_regs = num_phys_regs,
    };
    return anvil_regalloc_linear_scan_classes(func, &config, 1);
}
