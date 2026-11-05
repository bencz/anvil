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

/* A value's live positions as sorted, disjoint closed ranges: per block
 * from its live-in start (or first reference) to its live-out end (or last
 * reference).  The interval spans them all; between them (lifetime holes)
 * its register holds nothing.  `valid` false: the intervals stand alone. */
typedef struct {
    size_t start, end;
} live_segment_t;

typedef struct {
    bool valid;
    size_t *first, *count;  /* per vreg: its ranges[first .. first + count) */
    live_segment_t *ranges;
    const live_interval_t *intervals;
} live_segments_t;

/* Limit on the ranges kept (beyond it, intervals alone). */
enum { LIVE_SEGMENT_LIMIT = 1 << 22 };

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

static void live_segments_destroy(live_segments_t *segments)
{
    free(segments->first);
    free(segments->count);
    free(segments->ranges);
    segments->first = segments->count = NULL;
    segments->ranges = NULL;
    segments->valid = false;
}

/* Each value's ranges per block (see live_segments_t), blocks visited in
 * layout order; ranges of adjacent blocks merge.  Without memory (or past
 * LIVE_SEGMENT_LIMIT) `segments` stays invalid: intervals alone. */
static void build_live_segments(const anvil_mir_func_t *func, const anvil_mir_liveness_t *liveness, live_segments_t *segments)
{
    size_t n = func->num_vregs, blocks = func->num_blocks;
    size_t *order = malloc(blocks * sizeof(*order));
    size_t *low = malloc(n * sizeof(*low)), *high = malloc(n * sizeof(*high)), *stamp = malloc(n * sizeof(*stamp));
    segments->first = calloc(n, sizeof(size_t));
    segments->count = calloc(n, sizeof(size_t));
    bool ok = order && low && high && stamp && segments->first && segments->count;
    size_t num_order = 0, total = 0;
    for (size_t b = 0; ok && b < blocks; b++) {
        if (liveness->first_instr[b] == SIZE_MAX)
            continue;
        size_t at = num_order++;
        while (at > 0 && liveness->first_instr[order[at - 1]] > liveness->first_instr[b]) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = b;
    }
    /* Upper bound per value: one range per block it is live or named in. */
    for (size_t v = 0; ok && v < n; v++)
        stamp[v] = SIZE_MAX;
    for (int pass = 0; ok && pass < 2; pass++) {
        if (pass == 1) {
            if (total > LIVE_SEGMENT_LIMIT || !(segments->ranges = malloc((total ? total : 1) * sizeof(live_segment_t)))) {
                ok = false;
                break;
            }
            size_t offset = 0;
            for (size_t v = 0; v < n; v++) {
                size_t bound = segments->count[v];
                segments->first[v] = offset;
                segments->count[v] = 0;
                offset += bound;
                stamp[v] = SIZE_MAX;
            }
        }
        for (size_t o = 0; o < num_order; o++) {
            size_t block = order[o];
            for (size_t i = liveness->first_instr[block]; i <= liveness->last_instr[block] && i < func->num_instrs; i++) {
                const anvil_mir_instr_t *instr = &func->instrs[i];
                if (instr->block != block)
                    continue;
                anvil_mir_vreg_t refs[1 + 16];
                size_t positions[1 + 16], num_refs = 0;
                if (instr->def != ANVIL_MIR_NO_VREG && instr->def < n) {
                    refs[num_refs] = instr->def;
                    positions[num_refs++] = i * 2 + 1;
                }
                for (size_t u = 0; u < instr->num_uses; u++) {
                    anvil_mir_vreg_t use = instr->uses[u];
                    if (use >= n)
                        continue;
                    if (stamp[use] != block) {
                        stamp[use] = block;
                        low[use] = high[use] = i * 2;
                    } else {
                        if (i * 2 < low[use]) low[use] = i * 2;
                        if (i * 2 > high[use]) high[use] = i * 2;
                    }
                }
                for (size_t r = 0; r < num_refs; r++) {
                    anvil_mir_vreg_t v = refs[r];
                    if (stamp[v] != block) {
                        stamp[v] = block;
                        low[v] = high[v] = positions[r];
                    } else {
                        if (positions[r] < low[v]) low[v] = positions[r];
                        if (positions[r] > high[v]) high[v] = positions[r];
                    }
                }
            }
            /* Live out ends with the block's last instruction (2i + 1), so
             * a value live out of a block and one live into the next do
             * not meet. */
            size_t start = liveness->first_instr[block] * 2, end = liveness->last_instr[block] * 2 + 1;
            size_t row = block * liveness->words_per_block;
            for (size_t v = 0; v < n; v++) {
                uint64_t bit = UINT64_C(1) << (v % 64);
                bool in = (liveness->live_in[row + v / 64] & bit) != 0, out = (liveness->live_out[row + v / 64] & bit) != 0;
                bool named = stamp[v] == block;
                if (!in && !out && !named)
                    continue;
                if (pass == 0) {
                    segments->count[v]++;
                    total++;
                    continue;
                }
                live_segment_t range = {.start = in || !named ? start : low[v], .end = out || !named ? end : high[v]};
                live_segment_t *ranges = &segments->ranges[segments->first[v]];
                size_t *count = &segments->count[v];
                if (*count && range.start <= ranges[*count - 1].end + 1) {
                    if (range.end > ranges[*count - 1].end)
                        ranges[*count - 1].end = range.end;
                } else {
                    ranges[(*count)++] = range;
                }
            }
        }
    }
    free(order);
    free(low);
    free(high);
    free(stamp);
    if (!ok) {
        live_segments_destroy(segments);
        return;
    }
    for (size_t v = 0; v < n; v++)
        if (func->vregs[v].is_live_in && segments->count[v])
            segments->ranges[segments->first[v]].start = 0;
    segments->valid = true;
}

/* Extends intervals over the blocks their values are live into or out of,
 * records in `clobbers` the values live across each clobber, and builds
 * the values' `segments`. */
static bool note_cfg_live_ranges(anvil_mir_func_t *func, live_interval_t *intervals, clobber_map_t *clobbers, live_segments_t *segments)
{
    if (func->num_blocks == 0 || func->num_vregs == 0)
        return true;

    anvil_mir_liveness_t liveness;
    if (!anvil_mir_compute_liveness(func, &liveness))
        return false;
    build_live_segments(func, &liveness, segments);

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

/* Whether values a and b are live at a common position. */
static bool live_overlap(const live_segments_t *segments, anvil_mir_vreg_t a, anvil_mir_vreg_t b)
{
    const live_interval_t *ia = &segments->intervals[a], *ib = &segments->intervals[b];
    if (!ia->live || !ib->live || ia->start > ib->end || ib->start > ia->end)
        return false;
    if (!segments->valid)
        return true;
    size_t x = segments->first[a], x_end = x + segments->count[a];
    size_t y = segments->first[b], y_end = y + segments->count[b];
    while (x < x_end && y < y_end) {
        const live_segment_t *p = &segments->ranges[x], *q = &segments->ranges[y];
        if (p->start <= q->end && q->start <= p->end)
            return true;
        if (p->end < q->end)
            x++;
        else
            y++;
    }
    return false;
}

/* Whether an active value other than `except` holds `reg` of `reg_class`
 * where `current` lives. */
static bool register_held(const anvil_mir_func_t *func, const anvil_mir_vreg_t *active, size_t num_active, const live_segments_t *segments,
                          anvil_mir_reg_class_t reg_class, int reg, anvil_mir_vreg_t current, anvil_mir_vreg_t except)
{
    for (size_t a = 0; a < num_active; a++) {
        const anvil_regalloc_assignment_t *assignment = &func->assignments[active[a]];
        if (active[a] != except && !assignment->spilled && assignment->reg_class == reg_class && assignment->phys_reg == reg &&
            live_overlap(segments, active[a], current))
            return true;
    }
    return false;
}

/* The first free pool register for `interval`; `avoid` (or -1) is taken
 * only when nothing else is free. */
static int find_free_phys_reg(const anvil_mir_func_t *func,
                              const anvil_mir_vreg_t *active,
                              size_t num_active,
                              const live_segments_t *segments,
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

        bool used = register_held(func, active, num_active, segments, config->reg_class, candidate, interval->vreg, ANVIL_MIR_NO_VREG);

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
                               const live_interval_t *intervals, const live_segments_t *segments, const anvil_regalloc_class_config_t *config,
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
            assignment->reg_class != config->reg_class || !live_overlap(segments, other, displaced))
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
/* Spill weights: anvil_mir_use_weights over the positions the value holds
 * (the interval without lifetime holes).  NULL without memory. */

static double *spill_weights(const anvil_mir_func_t *func, const live_interval_t *intervals, const live_segments_t *segments)
{
    double *weights = anvil_mir_use_weights(func);
    if (!weights)
        return NULL;
    for (size_t value = 0; value < func->num_vregs; value++) {
        if (!intervals[value].live)
            continue;
        /* The positions it holds a register at: its holes hold none. */
        size_t length = intervals[value].end - intervals[value].start;
        if (segments->valid && segments->count[value]) {
            length = 0;
            for (size_t k = 0; k < segments->count[value]; k++)
                length += segments->ranges[segments->first[value] + k].end - segments->ranges[segments->first[value] + k].start;
        }
        weights[value] /= (double)(length + 2);
    }
    return weights;
}

/* The active value of `reg_class` to spill for `current`: recomputable
 * ones first, then the lightest (with `weights`) or the one ending last. */
static bool active_with_latest_end_in_class(const anvil_mir_func_t *func,
                                            const anvil_mir_vreg_t *active,
                                            size_t num_active,
                                            const live_segments_t *segments,
                                            const live_interval_t *intervals,
                                            const bool *recomputable,
                                            const double *weights,
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
        /* Its register, shared in lifetime holes, must come free. */
        if (register_held(func, active, num_active, segments, reg_class, func->assignments[vreg].phys_reg, current->vreg, vreg))
            continue;

        anvil_mir_vreg_t best = active[latest];
        bool cheaper = found && recomputable && recomputable[vreg] && !recomputable[best];
        bool dearer = found && recomputable && !recomputable[vreg] && recomputable[best];
        bool later = intervals[vreg].end > intervals[best].end;
        bool better = weights ? weights[vreg] < weights[best] || (weights[vreg] == weights[best] && later) : later;
        if (!found || cheaper || (!dearer && better)) {
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
                                          const live_segments_t *segments,
                                          anvil_mir_reg_class_t reg_class,
                                          int phys_reg,
                                          anvil_mir_vreg_t current,
                                          size_t *out_index)
{
    for (size_t i = 0; i < num_active; i++) {
        const anvil_regalloc_assignment_t *assignment =
            &func->assignments[active[i]];
        if (!assignment->spilled &&
            assignment->reg_class == reg_class &&
            assignment->phys_reg == phys_reg && live_overlap(segments, active[i], current)) {
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
enum { COPY_PARTNERS = 4 };

typedef struct {
    int fixed;
    /* Copy partners in the order their registers are tried (a two-address
     * result's first operand first). */
    anvil_mir_vreg_t partners[COPY_PARTNERS];
    uint8_t num_partners;
    anvil_mir_vreg_t avoid;     /* two-address: the second operand */
} copy_hint_t;

static void add_partner(copy_hint_t *hint, anvil_mir_vreg_t partner)
{
    for (size_t k = 0; k < hint->num_partners; k++)
        if (hint->partners[k] == partner)
            return;
    if (hint->num_partners < COPY_PARTNERS)
        hint->partners[hint->num_partners++] = partner;
}

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

/* A definition standing for all of each value's (NULL: none, or ones of
 * different kinds): every one a load, or every one an integer compare. */
static bool same_kind(const anvil_mir_instr_t *a, const anvil_mir_instr_t *b)
{
    bool ca = a->op >= ANVIL_MIR_OP_CMP_EQ && a->op <= ANVIL_MIR_OP_CMP_UGE, cb = b->op >= ANVIL_MIR_OP_CMP_EQ && b->op <= ANVIL_MIR_OP_CMP_UGE;
    return (a->op == ANVIL_MIR_OP_LOAD && b->op == ANVIL_MIR_OP_LOAD) || (ca && cb);
}

static const anvil_mir_instr_t **single_definitions(const anvil_mir_func_t *func)
{
    const anvil_mir_instr_t **defs = calloc(func->num_vregs ? func->num_vregs : 1, sizeof(*defs));
    uint8_t *count = calloc(func->num_vregs ? func->num_vregs : 1, 1);
    for (size_t i = 0; defs && count && i < func->num_instrs; i++) {
        anvil_mir_vreg_t d = func->instrs[i].def;
        if (d == ANVIL_MIR_NO_VREG || d >= func->num_vregs || count[d] == 2)
            continue;
        if (count[d]++ == 0)
            defs[d] = &func->instrs[i];
        else if (!defs[d] || !same_kind(defs[d], &func->instrs[i]))
            count[d] = 2, defs[d] = NULL;
        else
            count[d] = 1;
    }
    bool counted = count != NULL;
    free(count);
    if (!counted) {
        free(defs);
        return NULL;
    }
    return defs;
}

/* An extension to 32 bits or less of a value its definition leaves so
 * extended in the whole register (every printer: a compare's 0/1, a narrow
 * load extended by its signedness): the source's register already holds
 * the result, like a copy's. */
static bool extension_view(const anvil_mir_func_t *func, const anvil_mir_instr_t *const *defs, const anvil_mir_instr_t *instr)
{
    if (!defs || (instr->op != ANVIL_MIR_OP_ZEXT && instr->op != ANVIL_MIR_OP_SEXT) || instr->num_uses != 1 || instr->def == ANVIL_MIR_NO_VREG ||
        instr->def >= func->num_vregs || instr->uses[0] >= func->num_vregs)
        return false;
    const anvil_mir_vreg_info_t *d = &func->vregs[instr->def], *u = &func->vregs[instr->uses[0]];
    const anvil_mir_instr_t *source = defs[instr->uses[0]];
    if (!source || d->reg_class != ANVIL_MIR_REG_GPR || u->reg_class != ANVIL_MIR_REG_GPR || d->size_bits > 32 || u->size_bits >= d->size_bits)
        return false;
    bool compare = source->op >= ANVIL_MIR_OP_CMP_EQ && source->op <= ANVIL_MIR_OP_CMP_UGE && instr->op == ANVIL_MIR_OP_ZEXT && !u->registerless;
    bool load = source->op == ANVIL_MIR_OP_LOAD && u->size_bits <= 16 && u->is_signed == (instr->op == ANVIL_MIR_OP_SEXT);
    return compare || load;
}

static copy_hint_t *build_copy_hints(const anvil_mir_func_t *func, const anvil_regalloc_class_config_t *configs, size_t num_configs)
{
    copy_hint_t *hints = malloc(func->num_vregs * sizeof(*hints));
    if (!hints)
        return NULL;
    const anvil_mir_instr_t **defs = single_definitions(func);
    for (size_t value = 0; value < func->num_vregs; value++) {
        hints[value].fixed = -1;
        hints[value].num_partners = 0;
        hints[value].avoid = ANVIL_MIR_NO_VREG;
    }
    for (size_t index = 0; index < func->num_instrs; index++) {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        if (instr->def != ANVIL_MIR_NO_VREG && instr->num_uses >= 1 && overwrites_first_operand(instr->op)) {
            anvil_mir_vreg_t def = instr->def, first = instr->uses[0];
            const anvil_regalloc_class_config_t *config = config_for_class(configs, num_configs, func->vregs[def].reg_class);
            if (config && config->two_address && func->vregs[first].reg_class == func->vregs[def].reg_class &&
                !func->vregs[def].has_fixed_reg && hints[def].num_partners == 0) {
                add_partner(&hints[def], first);
                /* A result in the second operand's register makes the
                 * printer go through a temporary when the first lives on. */
                if (instr->num_uses >= 2 && instr->uses[1] != first)
                    hints[def].avoid = instr->uses[1];
            }
            continue;
        }
        /* A fused multiply-add's result prefers the addend's register:
         * accumulating forms write the addend, so the printer needs no copy
         * into the destination (four-operand forms take any register). */
        if ((instr->op == ANVIL_MIR_OP_VECTOR || instr->op == ANVIL_MIR_OP_SCALAR) && instr->ir_op == ANVIL_OP_FMA &&
            instr->def != ANVIL_MIR_NO_VREG && instr->num_uses == 3) {
            anvil_mir_vreg_t def = instr->def, addend = instr->uses[2];
            if (func->vregs[addend].reg_class == func->vregs[def].reg_class && !func->vregs[def].has_fixed_reg &&
                hints[def].num_partners == 0) {
                add_partner(&hints[def], addend);
                hints[def].avoid = instr->uses[1];
            }
            continue;
        }
        /* A truncation defines only the low bits, which the source's
         * register already holds (readers needing more extend explicitly). */
        bool truncation = (instr->op == ANVIL_MIR_OP_TRUNC && instr->def != ANVIL_MIR_NO_VREG && instr->num_uses == 1 &&
                           func->vregs[instr->def].reg_class == ANVIL_MIR_REG_GPR) ||
                          extension_view(func, defs, instr);
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
            add_partner(&hints[def], use);
            add_partner(&hints[use], def);
        }
    }
    free(defs);
    return hints;
}

/* `candidate` is in the class pool, survives the interval's clobbers and
 * no active interval holds it. */
static bool phys_reg_available(const anvil_mir_func_t *func, const anvil_mir_vreg_t *active, size_t num_active, const live_segments_t *segments,
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
    return !register_held(func, active, num_active, segments, config->reg_class, candidate, interval->vreg, ANVIL_MIR_NO_VREG);
}

/* A copy or truncation d = s reads s's register instead of its own when s
 * holds its value there throughout d's life: every definition of d copies
 * s, d lives only within s's interval, no clobber of the register
 * interrupts it and no definition of s does unless d is redefined from the
 * new s right after, unread in between (and the register can hold d:
 * narrow restrictions).  Only s and such views occupy the register then,
 * all with s's bits, and the copies move nothing. */
static void share_view_registers(anvil_mir_func_t *func, const live_interval_t *intervals, const live_segments_t *segments,
                                 const anvil_regalloc_class_config_t *configs, size_t num_configs, const clobber_map_t *clobbers)
{
    size_t n = func->num_vregs;
    /* The values in each register (by class), and the source each view
     * moved in reads: another value may hold s's register in s's lifetime
     * holes. */
    size_t *by_reg_offsets = calloc(CLOBBER_KEYS + 1, sizeof(size_t));
    anvil_mir_vreg_t *by_reg = malloc((n ? n : 1) * sizeof(*by_reg));
    anvil_mir_vreg_t *view_of = malloc((n ? n : 1) * sizeof(*view_of));
    anvil_mir_vreg_t *moved = malloc((n ? n : 1) * sizeof(*moved));
    size_t num_moved = 0;
    if (!by_reg_offsets || !by_reg || !view_of || !moved) {
        free(by_reg_offsets);
        free(by_reg);
        free(view_of);
        free(moved);
        return;
    }
    for (size_t v = 0; v < n; v++) {
        const anvil_regalloc_assignment_t *a = &func->assignments[v];
        view_of[v] = ANVIL_MIR_NO_VREG;
        if (!a->spilled && a->phys_reg >= 0 && a->phys_reg < CLOBBER_REGISTERS && !func->vregs[v].registerless)
            by_reg_offsets[(size_t)a->reg_class * CLOBBER_REGISTERS + (size_t)a->phys_reg + 1]++;
    }
    for (size_t k = 0; k < CLOBBER_KEYS; k++)
        by_reg_offsets[k + 1] += by_reg_offsets[k];
    {
        size_t *fill = calloc(CLOBBER_KEYS, sizeof(size_t));
        if (!fill) {
            free(by_reg_offsets);
            free(by_reg);
            free(view_of);
            free(moved);
            return;
        }
        for (size_t v = 0; v < n; v++) {
            const anvil_regalloc_assignment_t *a = &func->assignments[v];
            if (!a->spilled && a->phys_reg >= 0 && a->phys_reg < CLOBBER_REGISTERS && !func->vregs[v].registerless) {
                size_t key = (size_t)a->reg_class * CLOBBER_REGISTERS + (size_t)a->phys_reg;
                by_reg[by_reg_offsets[key] + fill[key]++] = (anvil_mir_vreg_t)v;
            }
        }
        free(fill);
    }
    size_t *offsets = calloc(n + 1, sizeof(*offsets));
    size_t *positions = func->num_instrs ? malloc(func->num_instrs * sizeof(*positions)) : NULL;
    if (!offsets || (func->num_instrs && !positions)) {
        free(offsets);
        free(positions);
        free(by_reg_offsets);
        free(by_reg);
        free(view_of);
        free(moved);
        return;
    }
    /* The definition positions of every vreg. */
    for (size_t i = 0; i < func->num_instrs; i++)
        if (func->instrs[i].def != ANVIL_MIR_NO_VREG)
            offsets[func->instrs[i].def + 1]++;
    for (size_t v = 0; v < n; v++)
        offsets[v + 1] += offsets[v];
    size_t *fill = calloc(n ? n : 1, sizeof(*fill));
    if (!fill) {
        free(offsets);
        free(positions);
        free(by_reg_offsets);
        free(by_reg);
        free(view_of);
        free(moved);
        return;
    }
    for (size_t i = 0; i < func->num_instrs; i++) {
        anvil_mir_vreg_t def = func->instrs[i].def;
        if (def != ANVIL_MIR_NO_VREG)
            positions[offsets[def] + fill[def]++] = i * 2 + 1;
    }
    free(fill);

    const anvil_mir_instr_t **defs = single_definitions(func);
    for (size_t i = 0; i < func->num_instrs; i++) {
        const anvil_mir_instr_t *instr = &func->instrs[i];
        if ((instr->op != ANVIL_MIR_OP_COPY && instr->op != ANVIL_MIR_OP_TRUNC && !extension_view(func, defs, instr)) || instr->num_uses != 1 ||
            instr->def == ANVIL_MIR_NO_VREG)
            continue;
        anvil_mir_vreg_t d = instr->def, src = instr->uses[0];
        const anvil_mir_vreg_info_t *vd = &func->vregs[d], *vs = &func->vregs[src];
        anvil_regalloc_assignment_t *ad = &func->assignments[d];
        const anvil_regalloc_assignment_t *as = &func->assignments[src];
        if (d == src || vd->reg_class != vs->reg_class || vd->has_fixed_reg || vd->is_live_in || vd->registerless || vs->registerless || ad->spilled ||
            as->spilled || ad->phys_reg < 0 ||
            as->phys_reg < 0 || ad->phys_reg == as->phys_reg)
            continue;
        const live_interval_t *id = &intervals[d], *is = &intervals[src];
        if (!id->live || !is->live || is->start > id->start || is->end < id->end)
            continue;
        /* Every definition of d copies s (a test block copied into a loop
         * latch repeats it). */
        bool views = true;
        for (size_t at = offsets[d]; at < offsets[d + 1] && views; at++) {
            const anvil_mir_instr_t *def = &func->instrs[positions[at] / 2];
            views = (def->op == ANVIL_MIR_OP_COPY || def->op == ANVIL_MIR_OP_TRUNC || extension_view(func, defs, def)) && def->num_uses == 1 &&
                    def->uses[0] == src;
        }
        /* s keeps its value while d lives, but for redefinitions of s that
         * d follows from the new s in the same block, unread in between:
         * the register then holds s's bits again before d is read. */
        bool redefined = !views;
        for (size_t at = offsets[src]; at < offsets[src + 1] && !redefined; at++) {
            if (positions[at] <= id->start || positions[at] > id->end)
                continue;
            size_t j = positions[at] / 2 + 1;
            redefined = true;
            for (; j < func->num_instrs && func->instrs[j].block == func->instrs[j - 1].block; j++) {
                const anvil_mir_instr_t *next = &func->instrs[j];
                bool reads = false;
                for (size_t u = 0; u < next->num_uses; u++)
                    reads |= next->uses[u] == d;
                if (next->def == d) {
                    redefined = reads || !((next->op == ANVIL_MIR_OP_COPY || next->op == ANVIL_MIR_OP_TRUNC) && next->uses[0] == src);
                    break;
                }
                if (reads || next->def == src)
                    break;
            }
        }
        const anvil_regalloc_class_config_t *config = config_for_class(configs, num_configs, vd->reg_class);
        bool narrow_ok = !config || !config->narrow_bits || vd->size_bits > config->narrow_bits ||
                         (as->phys_reg < 64 && ((config->narrow_regs >> as->phys_reg) & 1));
        if (redefined || !narrow_ok || !register_survives(clobbers, vd->reg_class, as->phys_reg, id) || as->phys_reg >= CLOBBER_REGISTERS)
            continue;
        size_t key = (size_t)vd->reg_class * CLOBBER_REGISTERS + (size_t)as->phys_reg;
        bool taken = false;
        for (size_t k = by_reg_offsets[key]; k < by_reg_offsets[key + 1] && !taken; k++) {
            anvil_mir_vreg_t other = by_reg[k];
            taken = other != src && other != d && view_of[other] != src && func->assignments[other].phys_reg == as->phys_reg &&
                    !func->assignments[other].spilled && live_overlap(segments, other, d);
        }
        /* Views moved here earlier are not in the lists. */
        for (size_t k = 0; k < num_moved && !taken; k++) {
            anvil_mir_vreg_t other = moved[k];
            taken = other != d && view_of[other] != src && func->assignments[other].reg_class == vd->reg_class &&
                    func->assignments[other].phys_reg == as->phys_reg && live_overlap(segments, other, d);
        }
        if (taken)
            continue;
        ad->phys_reg = as->phys_reg;
        view_of[d] = src;
        moved[num_moved++] = d;
    }
    free(offsets);
    free(positions);
    free(by_reg_offsets);
    free(by_reg);
    free(view_of);
    free(moved);
    free(defs);
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
    live_segments_t segments = { 0 };
    if (!build_clobber_map(func, &clobbers) || !clobber_map_add_partial(&clobbers, func, configs, num_configs))
    {
        clobber_map_destroy(&clobbers);
        live_segments_destroy(&segments);
        anvil_mir_clear_allocations(func);
        return false;
    }

    /* Hints only steer the choice; without memory for them allocation
     * proceeds unhinted. */
    copy_hint_t *hints = build_copy_hints(func, configs, num_configs);
    bool *recomputable = recomputable_values(func);
    double *weights = NULL;
    live_interval_t *intervals = calloc(func->num_vregs, sizeof(*intervals));
    anvil_mir_vreg_t *order = malloc(func->num_vregs * sizeof(*order));
    anvil_mir_vreg_t *active = malloc(func->num_vregs * sizeof(*active));
    if (!intervals || !order || !active) {
        free(intervals);
        clobber_map_destroy(&clobbers);
        live_segments_destroy(&segments);
        free(order);
        free(active);
        free(hints);
        free(recomputable);
        free(weights);
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

    segments.intervals = intervals;
    bool noted = note_cfg_live_ranges(func, intervals, &clobbers, &segments);
    if (!noted) {
        free(intervals);
        clobber_map_destroy(&clobbers);
        live_segments_destroy(&segments);
        free(order);
        free(active);
        free(hints);
        free(recomputable);
        free(weights);
        anvil_mir_clear_allocations(func);
        return false;
    }

    for (size_t value = 0; value < func->num_vregs; value++)
    {
        if (func->vregs[value].is_live_in && intervals[value].live)
            intervals[value].start = 0;
    }

    sort_vregs_by_start(order, func->num_vregs, intervals);
    /* Without memory for them the furthest end decides alone. */
    weights = spill_weights(func, intervals, &segments);

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

        /* Never in a register: a name, holding nothing. */
        if (func->vregs[current].registerless && !func->vregs[current].has_fixed_reg && config && config->num_phys_regs > 0) {
            assign_phys_reg(func, current, config->phys_regs[0]);
            continue;
        }

        if (func->vregs[current].has_fixed_reg) {
            int fixed_reg = func->vregs[current].fixed_phys_reg;
            if (!register_survives(&clobbers, reg_class, fixed_reg, current_interval)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                clobber_map_destroy(&clobbers);
        live_segments_destroy(&segments);
                free(order);
                free(active);
        free(hints);
        free(recomputable);
        free(weights);
                return false;
            }

            /* Each active holder of the register live where this value
             * is (others share it in their lifetime holes). */
            size_t conflict_index = 0;
            bool displaced = false;
            while (active_with_phys_reg_in_class(func, active, num_active, &segments,
                                                 reg_class, fixed_reg, current,
                                                 &conflict_index)) {
                anvil_mir_vreg_t conflict = active[conflict_index];
                if (func->vregs[conflict].has_fixed_reg) {
                    anvil_mir_clear_allocations(func);
                    free(intervals);
                    clobber_map_destroy(&clobbers);
        live_segments_destroy(&segments);
                    free(order);
                    free(active);
        free(hints);
        free(recomputable);
        free(weights);
                    return false;
                }

                int moved = relocation_register(func, order, i, intervals, &segments, config, &clobbers, conflict, fixed_reg);
                if (moved >= 0)
                    assign_phys_reg(func, conflict, moved);
                else if (mark_spilled(func, conflict)) {
                    active[conflict_index] = active[--num_active];
                } else {
                    anvil_mir_clear_allocations(func);
                    free(intervals);
                    clobber_map_destroy(&clobbers);
        live_segments_destroy(&segments);
                    free(order);
                    free(active);
        free(hints);
        free(recomputable);
        free(weights);
                    return false;
                }
                displaced = true;
            }
            (void)displaced;
            assign_phys_reg(func, current, fixed_reg);
            active[num_active++] = current;
            continue;
        }

        int free_reg = -1;
        if (hints) {
            const copy_hint_t *hint = &hints[current];
            if (phys_reg_available(func, active, num_active, &segments, config, &clobbers, current_interval, hint->fixed))
                free_reg = hint->fixed;
            for (size_t k = 0; free_reg < 0 && k < hint->num_partners; k++) {
                anvil_mir_vreg_t partner = hint->partners[k];
                if (!func->assignments[partner].spilled &&
                    phys_reg_available(func, active, num_active, &segments, config, &clobbers, current_interval, func->assignments[partner].phys_reg))
                    free_reg = func->assignments[partner].phys_reg;
                /* The register a partner the value flows straight into
                 * wants (a compare extended for a return). */
                else if (hints[partner].fixed >= 0 && func->vregs[partner].reg_class == reg_class && intervals[partner].live &&
                         intervals[partner].start + 2 >= current_interval->end && intervals[partner].start <= current_interval->end + 2 &&
                         phys_reg_available(func, active, num_active, &segments, config, &clobbers, current_interval, hints[partner].fixed))
                    free_reg = hints[partner].fixed;
            }
            /* A chain of copies (a PHI's value moved on into another
             * PHI): the partners' partners. */
            for (size_t k = 0; free_reg < 0 && k < hint->num_partners; k++) {
                const copy_hint_t *next = &hints[hint->partners[k]];
                for (size_t m = 0; free_reg < 0 && m < next->num_partners; m++) {
                    anvil_mir_vreg_t partner = next->partners[m];
                    if (partner != current && !func->assignments[partner].spilled &&
                        func->vregs[partner].reg_class == reg_class &&
                        phys_reg_available(func, active, num_active, &segments, config, &clobbers, current_interval, func->assignments[partner].phys_reg))
                        free_reg = func->assignments[partner].phys_reg;
                }
            }
        }
        if (free_reg < 0) {
            anvil_mir_vreg_t avoid = hints ? hints[current].avoid : ANVIL_MIR_NO_VREG;
            int avoid_reg = avoid != ANVIL_MIR_NO_VREG && !func->assignments[avoid].spilled ? func->assignments[avoid].phys_reg : -1;
            free_reg = find_free_phys_reg(func, active, num_active, &segments, config, &clobbers, current_interval, avoid_reg);
        }
        if (free_reg >= 0) {
            assign_phys_reg(func, current, free_reg);
            active[num_active++] = current;
            continue;
        }

        /* A value only some registers can hold (narrow) and all of those
         * taken: move a holder that can live elsewhere to a register free
         * over its whole interval, rather than spill anything. */
        if (config && config->narrow_bits && func->vregs[current].size_bits <= config->narrow_bits) {
            for (size_t a = 0; a < num_active && free_reg < 0; a++) {
                anvil_mir_vreg_t holder = active[a];
                int reg = func->assignments[holder].phys_reg;
                if (func->assignments[holder].spilled || func->assignments[holder].reg_class != reg_class || func->vregs[holder].has_fixed_reg ||
                    func->vregs[holder].size_bits <= config->narrow_bits || reg < 0 || reg >= 64 || !((config->narrow_regs >> reg) & 1) ||
                    !register_survives(&clobbers, reg_class, reg, current_interval))
                    continue;
                bool shared = register_held(func, active, num_active, &segments, reg_class, reg, current, holder);
                int moved = shared ? -1 : relocation_register(func, order, i, intervals, &segments, config, &clobbers, holder, reg);
                if (moved < 0 || ((config->narrow_regs >> moved) & 1))
                    continue;
                assign_phys_reg(func, holder, moved);
                free_reg = reg;
            }
            if (free_reg >= 0) {
                assign_phys_reg(func, current, free_reg);
                active[num_active++] = current;
                continue;
            }
        }

        size_t spill_index = 0;
        if ((recomputable && recomputable[current]) ||
            !active_with_latest_end_in_class(func, active, num_active, &segments,
                                             intervals, recomputable, weights, reg_class, &clobbers, current_interval,
                                             &spill_index)) {
            if (!mark_spilled(func, current)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                clobber_map_destroy(&clobbers);
        live_segments_destroy(&segments);
                free(order);
                free(active);
        free(hints);
        free(recomputable);
        free(weights);
                return false;
            }
            continue;
        }

        anvil_mir_vreg_t spill_candidate = active[spill_index];
        int reclaimed_reg = func->assignments[spill_candidate].phys_reg;
        bool spill_other = weights ? weights[spill_candidate] < weights[current] ||
                                         (weights[spill_candidate] == weights[current] && intervals[spill_candidate].end > current_interval->end)
                                   : intervals[spill_candidate].end > current_interval->end;
        if (spill_other || (recomputable && recomputable[spill_candidate])) {
            if (!mark_spilled(func, spill_candidate)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                clobber_map_destroy(&clobbers);
        live_segments_destroy(&segments);
                free(order);
                free(active);
        free(hints);
        free(recomputable);
        free(weights);
                return false;
            }
            assign_phys_reg(func, current, reclaimed_reg);
            active[spill_index] = current;
        } else {
            if (!mark_spilled(func, current)) {
                anvil_mir_clear_allocations(func);
                free(intervals);
                clobber_map_destroy(&clobbers);
        live_segments_destroy(&segments);
                free(order);
                free(active);
        free(hints);
        free(recomputable);
        free(weights);
                return false;
            }
        }
    }

    share_view_registers(func, intervals, &segments, configs, num_configs, &clobbers);
    bool compacted = reuse_spill_slots(func, intervals, order);
    free(intervals);
    clobber_map_destroy(&clobbers);
        live_segments_destroy(&segments);
    free(order);
    free(active);
        free(hints);
        free(recomputable);
        free(weights);
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
