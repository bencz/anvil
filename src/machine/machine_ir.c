/*
 * ANVIL - MachineIR container implementation.
 */

#include "machine_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *mir_strdup(const char *str)
{
    if (!str) str = "";

    size_t len = strlen(str) + 1;
    char *copy = malloc(len);
    if (!copy) return NULL;

    memcpy(copy, str, len);
    return copy;
}

anvil_mir_func_t *anvil_mir_func_create(const char *name)
{
    anvil_mir_func_t *func = calloc(1, sizeof(*func));
    if (!func) return NULL;

    func->name = mir_strdup(name ? name : "mir_func");
    if (!func->name) {
        free(func);
        return NULL;
    }

    func->current_block = ANVIL_MIR_NO_BLOCK;
    if (anvil_mir_add_block(func, "entry") == ANVIL_MIR_NO_BLOCK) {
        free(func->name);
        free(func);
        return NULL;
    }

    return func;
}

void anvil_mir_func_destroy(anvil_mir_func_t *func)
{
    if (!func) return;

    for (size_t i = 0; i < func->num_instrs; i++) {
        free(func->instrs[i].uses);
        free(func->instrs[i].symbol);
    }
    free(func->instrs);
    for (size_t i = 0; i < func->num_blocks; i++) {
        free(func->blocks[i].name);
    }
    free(func->blocks);
    for (size_t i = 0; i < func->num_string_literals; i++) {
        free(func->string_literals[i].label);
        free(func->string_literals[i].value);
    }
    free(func->string_literals);
    free(func->vregs);
    free(func->assignments);
    free(func->spill_slots);
    free(func->frame_slots);
    free(func->name);
    free(func);
}

const char *anvil_mir_func_name(const anvil_mir_func_t *func)
{
    return func ? func->name : NULL;
}

static bool mir_valid_block(const anvil_mir_func_t *func,
                            anvil_mir_block_t block)
{
    return func && block != ANVIL_MIR_NO_BLOCK && (size_t)block < func->num_blocks;
}

static bool reserve_blocks(anvil_mir_func_t *func, size_t needed)
{
    if (needed <= func->cap_blocks) return true;

    size_t new_cap = func->cap_blocks ? func->cap_blocks * 2 : 8;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }

    anvil_mir_block_data_t *grown = realloc(func->blocks,
                                            new_cap * sizeof(*grown));
    if (!grown) return false;

    func->blocks = grown;
    func->cap_blocks = new_cap;
    return true;
}

anvil_mir_block_t anvil_mir_add_block(anvil_mir_func_t *func,
                                      const char *name)
{
    if (!func || func->num_blocks >= UINT32_MAX) return ANVIL_MIR_NO_BLOCK;
    if (!reserve_blocks(func, func->num_blocks + 1)) return ANVIL_MIR_NO_BLOCK;

    char *name_copy = mir_strdup(name ? name : "");
    if (!name_copy) return ANVIL_MIR_NO_BLOCK;

    anvil_mir_block_t block = (anvil_mir_block_t)func->num_blocks++;
    func->blocks[block].name = name_copy;
    func->blocks[block].first_instr = SIZE_MAX;
    func->blocks[block].num_instrs = 0;
    func->current_block = block;
    return block;
}

bool anvil_mir_set_current_block(anvil_mir_func_t *func,
                                 anvil_mir_block_t block)
{
    if (!mir_valid_block(func, block)) return false;
    func->current_block = block;
    return true;
}

anvil_mir_block_t anvil_mir_current_block(const anvil_mir_func_t *func)
{
    return func ? func->current_block : ANVIL_MIR_NO_BLOCK;
}

size_t anvil_mir_num_blocks(const anvil_mir_func_t *func)
{
    return func ? func->num_blocks : 0;
}

bool anvil_mir_get_block_info(const anvil_mir_func_t *func,
                              anvil_mir_block_t block,
                              anvil_mir_block_info_t *out_info)
{
    if (!mir_valid_block(func, block) || !out_info) return false;

    const anvil_mir_block_data_t *data = &func->blocks[block];
    out_info->name = data->name;
    out_info->first_instr = data->first_instr == SIZE_MAX ? 0 : data->first_instr;
    out_info->num_instrs = data->num_instrs;
    return true;
}

bool anvil_mir_valid_vreg(const anvil_mir_func_t *func, anvil_mir_vreg_t vreg)
{
    return func && vreg != ANVIL_MIR_NO_VREG && (size_t)vreg < func->num_vregs;
}

static bool valid_reg_class(anvil_mir_reg_class_t reg_class)
{
    return reg_class >= ANVIL_MIR_REG_GPR &&
           reg_class < ANVIL_MIR_REG_CLASS_COUNT;
}

static bool reserve_vregs(anvil_mir_func_t *func, size_t needed)
{
    if (needed <= func->cap_vregs) return true;

    size_t new_cap = func->cap_vregs ? func->cap_vregs * 2 : 16;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }

    anvil_mir_vreg_info_t *grown = realloc(func->vregs,
                                           new_cap * sizeof(*grown));
    if (!grown) return false;

    func->vregs = grown;
    func->cap_vregs = new_cap;
    return true;
}

anvil_mir_vreg_t anvil_mir_add_vreg_typed(anvil_mir_func_t *func,
                                          anvil_mir_reg_class_t reg_class,
                                          uint16_t size_bits,
                                          bool is_signed)
{
    if (!func || func->num_vregs >= UINT32_MAX) return ANVIL_MIR_NO_VREG;
    if (!valid_reg_class(reg_class) || size_bits == 0) return ANVIL_MIR_NO_VREG;
    if (!reserve_vregs(func, func->num_vregs + 1)) return ANVIL_MIR_NO_VREG;

    anvil_mir_clear_allocations(func);

    anvil_mir_vreg_t vreg = (anvil_mir_vreg_t)func->num_vregs++;
    func->vregs[vreg].reg_class = reg_class;
    func->vregs[vreg].size_bits = size_bits;
    func->vregs[vreg].is_signed = is_signed;
    func->vregs[vreg].is_live_in = false;
    func->vregs[vreg].has_fixed_reg = false;
    func->vregs[vreg].fixed_phys_reg = -1;
    return vreg;
}

anvil_mir_vreg_t anvil_mir_add_vreg_ex(anvil_mir_func_t *func,
                                       anvil_mir_reg_class_t reg_class,
                                       uint16_t size_bits)
{
    return anvil_mir_add_vreg_typed(func, reg_class, size_bits, false);
}

anvil_mir_vreg_t anvil_mir_add_vreg(anvil_mir_func_t *func)
{
    return anvil_mir_add_vreg_ex(func, ANVIL_MIR_REG_GPR, 64);
}

const anvil_mir_vreg_info_t *
anvil_mir_get_vreg_info(const anvil_mir_func_t *func, anvil_mir_vreg_t vreg)
{
    if (!anvil_mir_valid_vreg(func, vreg)) return NULL;
    return &func->vregs[vreg];
}

bool anvil_mir_set_vreg_signed(anvil_mir_func_t *func,
                               anvil_mir_vreg_t vreg,
                               bool is_signed)
{
    if (!anvil_mir_valid_vreg(func, vreg)) return false;
    func->vregs[vreg].is_signed = is_signed;
    return true;
}

bool anvil_mir_set_live_in(anvil_mir_func_t *func, anvil_mir_vreg_t vreg,
                           bool is_live_in)
{
    if (!anvil_mir_valid_vreg(func, vreg)) return false;
    anvil_mir_clear_allocations(func);
    func->vregs[vreg].is_live_in = is_live_in;
    return true;
}

bool anvil_mir_set_fixed_reg(anvil_mir_func_t *func, anvil_mir_vreg_t vreg,
                             int phys_reg)
{
    if (!anvil_mir_valid_vreg(func, vreg) || phys_reg < 0) return false;

    anvil_mir_clear_allocations(func);
    func->vregs[vreg].has_fixed_reg = true;
    func->vregs[vreg].fixed_phys_reg = phys_reg;
    return true;
}

bool anvil_mir_clear_fixed_reg(anvil_mir_func_t *func, anvil_mir_vreg_t vreg)
{
    if (!anvil_mir_valid_vreg(func, vreg)) return false;

    anvil_mir_clear_allocations(func);
    func->vregs[vreg].has_fixed_reg = false;
    func->vregs[vreg].fixed_phys_reg = -1;
    return true;
}

static bool reserve_instrs(anvil_mir_func_t *func, size_t needed)
{
    if (needed <= func->cap_instrs) return true;

    size_t new_cap = func->cap_instrs ? func->cap_instrs * 2 : 16;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }

    anvil_mir_instr_t *grown = realloc(func->instrs,
                                       new_cap * sizeof(*grown));
    if (!grown) return false;

    func->instrs = grown;
    func->cap_instrs = new_cap;
    return true;
}

static bool add_instr_full(anvil_mir_func_t *func, anvil_mir_opcode_t op,
                           anvil_mir_vreg_t def,
                           const anvil_mir_vreg_t *uses,
                           size_t num_uses,
                           bool has_imm,
                           int64_t imm,
                           const char *symbol,
                           anvil_mir_block_t true_block,
                           anvil_mir_block_t false_block)
{
    if (!func || op == ANVIL_MIR_OP_INVALID) return false;
    if (!mir_valid_block(func, func->current_block)) return false;
    if (def != ANVIL_MIR_NO_VREG && !anvil_mir_valid_vreg(func, def)) {
        return false;
    }
    if (true_block != ANVIL_MIR_NO_BLOCK && !mir_valid_block(func, true_block)) {
        return false;
    }
    if (false_block != ANVIL_MIR_NO_BLOCK && !mir_valid_block(func, false_block)) {
        return false;
    }
    if (num_uses > 0 && !uses) return false;

    for (size_t i = 0; i < num_uses; i++) {
        if (!anvil_mir_valid_vreg(func, uses[i])) return false;
    }

    anvil_mir_vreg_t *uses_copy = NULL;
    if (num_uses > 0) {
        uses_copy = malloc(num_uses * sizeof(*uses_copy));
        if (!uses_copy) return false;
        memcpy(uses_copy, uses, num_uses * sizeof(*uses_copy));
    }

    char *symbol_copy = NULL;
    if (symbol) {
        symbol_copy = mir_strdup(symbol);
        if (!symbol_copy) {
            free(uses_copy);
            return false;
        }
    }

    if (!reserve_instrs(func, func->num_instrs + 1)) {
        free(uses_copy);
        free(symbol_copy);
        return false;
    }

    size_t instr_index = func->num_instrs++;
    anvil_mir_instr_t *instr = &func->instrs[instr_index];
    instr->op = op;
    instr->def = def;
    instr->uses = uses_copy;
    instr->num_uses = num_uses;
    instr->block = func->current_block;
    instr->true_block = true_block;
    instr->false_block = false_block;
    instr->has_imm = has_imm;
    instr->imm = imm;
    instr->symbol = symbol_copy;
    instr->spill_slot = -1;
    instr->frame_slot = -1;

    anvil_mir_block_data_t *block = &func->blocks[func->current_block];
    if (block->num_instrs == 0) {
        block->first_instr = instr_index;
    }
    block->num_instrs++;

    anvil_mir_clear_allocations(func);
    return true;
}

bool anvil_mir_add_instr(anvil_mir_func_t *func, anvil_mir_opcode_t op,
                         anvil_mir_vreg_t def,
                         const anvil_mir_vreg_t *uses,
                         size_t num_uses)
{
    return add_instr_full(func, op, def, uses, num_uses, false, 0, NULL,
                          ANVIL_MIR_NO_BLOCK, ANVIL_MIR_NO_BLOCK);
}

bool anvil_mir_add_instr_imm(anvil_mir_func_t *func, anvil_mir_opcode_t op,
                             anvil_mir_vreg_t def, int64_t imm)
{
    return add_instr_full(func, op, def, NULL, 0, true, imm, NULL,
                          ANVIL_MIR_NO_BLOCK, ANVIL_MIR_NO_BLOCK);
}

bool anvil_mir_add_instr_imm_uses(anvil_mir_func_t *func,
                                  anvil_mir_opcode_t op,
                                  anvil_mir_vreg_t def,
                                  const anvil_mir_vreg_t *uses,
                                  size_t num_uses,
                                  int64_t imm)
{
    return add_instr_full(func, op, def, uses, num_uses, true, imm, NULL,
                          ANVIL_MIR_NO_BLOCK, ANVIL_MIR_NO_BLOCK);
}

bool anvil_mir_add_instr_symbol(anvil_mir_func_t *func,
                                anvil_mir_opcode_t op,
                                anvil_mir_vreg_t def,
                                const anvil_mir_vreg_t *uses,
                                size_t num_uses,
                                const char *symbol)
{
    return add_instr_full(func, op, def, uses, num_uses, false, 0, symbol,
                          ANVIL_MIR_NO_BLOCK, ANVIL_MIR_NO_BLOCK);
}

bool anvil_mir_add_instr_symbol_imm(anvil_mir_func_t *func,
                                    anvil_mir_opcode_t op,
                                    anvil_mir_vreg_t def,
                                    const anvil_mir_vreg_t *uses,
                                    size_t num_uses,
                                    const char *symbol,
                                    int64_t imm)
{
    return add_instr_full(func, op, def, uses, num_uses, true, imm,
                          symbol, ANVIL_MIR_NO_BLOCK,
                          ANVIL_MIR_NO_BLOCK);
}

static bool reserve_frame_slots(anvil_mir_func_t *func, size_t needed)
{
    if (needed <= func->cap_frame_slots) return true;

    size_t new_cap = func->cap_frame_slots ? func->cap_frame_slots * 2 : 8;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }

    anvil_mir_frame_slot_info_t *grown =
        realloc(func->frame_slots, new_cap * sizeof(*grown));
    if (!grown) return false;

    func->frame_slots = grown;
    func->cap_frame_slots = new_cap;
    return true;
}

int anvil_mir_add_frame_slot(anvil_mir_func_t *func,
                             uint16_t size_bits,
                             uint16_t align_bytes)
{
    if (!func || size_bits == 0 || align_bytes == 0) return -1;
    if (func->num_frame_slots > (size_t)INT_MAX) return -1;
    if (!reserve_frame_slots(func, func->num_frame_slots + 1)) return -1;

    int slot = (int)func->num_frame_slots++;
    func->frame_slots[slot].size_bits = size_bits;
    func->frame_slots[slot].align_bytes = align_bytes;
    return slot;
}

bool anvil_mir_add_frame_addr(anvil_mir_func_t *func,
                              anvil_mir_vreg_t def,
                              int frame_slot)
{
    if (!func || frame_slot < 0 ||
        (size_t)frame_slot >= func->num_frame_slots) {
        return false;
    }
    if (!add_instr_full(func, ANVIL_MIR_OP_FRAME_ADDR, def, NULL, 0,
                        false, 0, NULL,
                        ANVIL_MIR_NO_BLOCK, ANVIL_MIR_NO_BLOCK)) {
        return false;
    }

    func->instrs[func->num_instrs - 1].frame_slot = frame_slot;
    return true;
}

static bool reserve_string_literals(anvil_mir_func_t *func, size_t needed)
{
    if (needed <= func->cap_string_literals) return true;

    size_t new_cap = func->cap_string_literals
        ? func->cap_string_literals * 2
        : 8;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }

    anvil_mir_string_literal_t *grown =
        realloc(func->string_literals, new_cap * sizeof(*grown));
    if (!grown) return false;

    func->string_literals = grown;
    func->cap_string_literals = new_cap;
    return true;
}

static bool mir_label_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

static char *mir_sanitized_label_component(const char *text)
{
    if (!text || !text[0]) text = "func";

    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;

    for (size_t i = 0; i < len; i++) {
        copy[i] = mir_label_char_ok(text[i]) ? text[i] : '_';
    }
    copy[len] = '\0';
    return copy;
}

int anvil_mir_add_string_literal(anvil_mir_func_t *func,
                                 const char *value,
                                 const char **out_label)
{
    if (!func || func->num_string_literals > (size_t)INT_MAX) return -1;
    if (!reserve_string_literals(func, func->num_string_literals + 1)) {
        return -1;
    }

    char *safe_name = mir_sanitized_label_component(func->name);
    if (!safe_name) return -1;

    int needed = snprintf(NULL, 0, ".Lstr_%s_%zu",
                          safe_name, func->num_string_literals);
    if (needed < 0) {
        free(safe_name);
        return -1;
    }

    char *label = malloc((size_t)needed + 1);
    if (!label) {
        free(safe_name);
        return -1;
    }
    snprintf(label, (size_t)needed + 1, ".Lstr_%s_%zu",
             safe_name, func->num_string_literals);
    free(safe_name);

    char *value_copy = mir_strdup(value ? value : "");
    if (!value_copy) {
        free(label);
        return -1;
    }

    size_t index = func->num_string_literals++;
    func->string_literals[index].label = label;
    func->string_literals[index].value = value_copy;
    if (out_label) *out_label = func->string_literals[index].label;
    return (int)index;
}

bool anvil_mir_add_branch(anvil_mir_func_t *func,
                          anvil_mir_block_t target)
{
    return add_instr_full(func, ANVIL_MIR_OP_BR, ANVIL_MIR_NO_VREG,
                          NULL, 0, false, 0, NULL,
                          target, ANVIL_MIR_NO_BLOCK);
}

bool anvil_mir_add_cond_branch(anvil_mir_func_t *func,
                               anvil_mir_vreg_t cond,
                               anvil_mir_block_t true_block,
                               anvil_mir_block_t false_block)
{
    anvil_mir_vreg_t uses[] = { cond };
    return add_instr_full(func, ANVIL_MIR_OP_BR_COND, ANVIL_MIR_NO_VREG,
                          uses, 1, false, 0, NULL,
                          true_block, false_block);
}

size_t anvil_mir_num_vregs(const anvil_mir_func_t *func)
{
    return func ? func->num_vregs : 0;
}

size_t anvil_mir_num_instrs(const anvil_mir_func_t *func)
{
    return func ? func->num_instrs : 0;
}

bool anvil_mir_get_instr_info(const anvil_mir_func_t *func, size_t index,
                              anvil_mir_instr_info_t *out_info)
{
    if (!func || !out_info || index >= func->num_instrs) return false;

    const anvil_mir_instr_t *instr = &func->instrs[index];
    out_info->op = instr->op;
    out_info->def = instr->def;
    out_info->num_uses = instr->num_uses;
    out_info->block = instr->block;
    out_info->true_block = instr->true_block;
    out_info->false_block = instr->false_block;
    out_info->has_imm = instr->has_imm;
    out_info->imm = instr->imm;
    out_info->symbol = instr->symbol;
    out_info->spill_slot = instr->spill_slot;
    out_info->frame_slot = instr->frame_slot;
    return true;
}

anvil_mir_vreg_t anvil_mir_get_instr_use(const anvil_mir_func_t *func,
                                         size_t instr_index,
                                         size_t use_index)
{
    if (!func || instr_index >= func->num_instrs) return ANVIL_MIR_NO_VREG;

    const anvil_mir_instr_t *instr = &func->instrs[instr_index];
    if (use_index >= instr->num_uses) return ANVIL_MIR_NO_VREG;
    return instr->uses[use_index];
}

static bool mir_vregs_coalescible(const anvil_mir_func_t *func,
                                  anvil_mir_vreg_t dst,
                                  anvil_mir_vreg_t src)
{
    if (!anvil_mir_valid_vreg(func, dst) ||
        !anvil_mir_valid_vreg(func, src) ||
        dst == src) {
        return false;
    }

    const anvil_mir_vreg_info_t *dst_info = &func->vregs[dst];
    const anvil_mir_vreg_info_t *src_info = &func->vregs[src];
    if (dst_info->has_fixed_reg || src_info->has_fixed_reg) return false;
    return dst_info->reg_class == src_info->reg_class &&
           dst_info->size_bits == src_info->size_bits;
}

static void recompute_block_instr_ranges(anvil_mir_func_t *func);

static void remove_instr_at(anvil_mir_func_t *func, size_t index)
{
    if (!func || index >= func->num_instrs) return;
    free(func->instrs[index].uses);
    free(func->instrs[index].symbol);
    if (index + 1 < func->num_instrs) {
        memmove(&func->instrs[index], &func->instrs[index + 1],
                (func->num_instrs - index - 1) * sizeof(*func->instrs));
    }
    func->num_instrs--;
    recompute_block_instr_ranges(func);
}

bool anvil_mir_coalesce_copies(anvil_mir_func_t *func)
{
    if (!func) return false;
    if (func->num_vregs == 0 || func->num_instrs == 0) return true;

    size_t *def_counts = calloc(func->num_vregs, sizeof(*def_counts));
    if (!def_counts) return false;

    for (size_t i = 0; i < func->num_instrs; i++) {
        anvil_mir_vreg_t def = func->instrs[i].def;
        if (anvil_mir_valid_vreg(func, def)) {
            def_counts[def]++;
        }
    }

    bool changed = false;
    for (size_t i = 0; i < func->num_instrs; i++) {
        anvil_mir_instr_t *instr = &func->instrs[i];
        if (instr->op != ANVIL_MIR_OP_COPY || instr->num_uses != 1) continue;

        anvil_mir_vreg_t dst = instr->def;
        anvil_mir_vreg_t src = instr->uses[0];
        if (!mir_vregs_coalescible(func, dst, src)) continue;
        if (def_counts[dst] != 1 || def_counts[src] != 1) continue;

        /*
         * Without dominator information, a textual instruction order is not
         * enough to prove that a COPY reaches a use in another block.  Keep
         * this pass deliberately local: instruction order within one block
         * gives us the dominance proof we need, while cross-block copies are
         * left for a future CFG-aware coalescer.
         */
        bool all_uses_after_copy_in_block = true;
        for (size_t j = 0; j < func->num_instrs; j++) {
            for (size_t u = 0; u < func->instrs[j].num_uses; u++) {
                if (func->instrs[j].uses[u] != dst) continue;
                if (j <= i || func->instrs[j].block != instr->block) {
                    all_uses_after_copy_in_block = false;
                    break;
                }
            }
            if (!all_uses_after_copy_in_block) break;
        }
        if (!all_uses_after_copy_in_block) continue;

        bool rewrote_use = false;
        for (size_t j = i + 1; j < func->num_instrs; j++) {
            if (func->instrs[j].block != instr->block) continue;
            for (size_t u = 0; u < func->instrs[j].num_uses; u++) {
                if (func->instrs[j].uses[u] == dst) {
                    func->instrs[j].uses[u] = src;
                    rewrote_use = true;
                }
            }
        }

        if (rewrote_use) {
            remove_instr_at(func, i);
            i--;
            changed = true;
        }
    }

    free(def_counts);
    if (changed) anvil_mir_clear_allocations(func);
    return true;
}

static bool mir_verify_fail(char *error, size_t error_len,
                            const char *fmt, ...)
{
    if (error && error_len > 0) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, error_len, fmt, args);
        va_end(args);
    }
    return false;
}

static bool mir_op_is_terminator(anvil_mir_opcode_t op)
{
    return op == ANVIL_MIR_OP_RET ||
           op == ANVIL_MIR_OP_BR ||
           op == ANVIL_MIR_OP_BR_COND;
}

static bool mir_verify_num_uses(const anvil_mir_instr_t *instr,
                                size_t expected,
                                size_t index,
                                char *error,
                                size_t error_len)
{
    if (instr->num_uses == expected) return true;
    return mir_verify_fail(error, error_len,
                           "instruction %zu has %zu uses, expected %zu",
                           index, instr->num_uses, expected);
}

static bool mir_verify_has_def(const anvil_mir_instr_t *instr,
                               size_t index,
                               char *error,
                               size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG) return true;
    return mir_verify_fail(error, error_len,
                           "instruction %zu requires a definition", index);
}

static bool mir_verify_no_def(const anvil_mir_instr_t *instr,
                              size_t index,
                              char *error,
                              size_t error_len)
{
    if (instr->def == ANVIL_MIR_NO_VREG) return true;
    return mir_verify_fail(error, error_len,
                           "instruction %zu must not define a vreg", index);
}

static const anvil_mir_vreg_info_t *mir_verify_vreg_info(
    const anvil_mir_func_t *func,
    anvil_mir_vreg_t vreg,
    size_t index,
    char *error,
    size_t error_len)
{
    if (anvil_mir_valid_vreg(func, vreg)) return &func->vregs[vreg];
    mir_verify_fail(error, error_len,
                    "instruction %zu references invalid vreg %u",
                    index, vreg);
    return NULL;
}

static bool mir_verify_def_class(const anvil_mir_func_t *func,
                                 const anvil_mir_instr_t *instr,
                                 size_t index,
                                 anvil_mir_reg_class_t reg_class,
                                 char *error,
                                 size_t error_len)
{
    const anvil_mir_vreg_info_t *info =
        mir_verify_vreg_info(func, instr->def, index, error, error_len);
    if (!info) return false;
    if (info->reg_class == reg_class) return true;
    return mir_verify_fail(error, error_len,
                           "instruction %zu definition has wrong register class",
                           index);
}

static bool mir_verify_use_class(const anvil_mir_func_t *func,
                                 const anvil_mir_instr_t *instr,
                                 size_t index,
                                 size_t use_index,
                                 anvil_mir_reg_class_t reg_class,
                                 char *error,
                                 size_t error_len)
{
    const anvil_mir_vreg_info_t *info =
        mir_verify_vreg_info(func, instr->uses[use_index], index,
                             error, error_len);
    if (!info) return false;
    if (info->reg_class == reg_class) return true;
    return mir_verify_fail(error, error_len,
                           "instruction %zu use %zu has wrong register class",
                           index, use_index);
}

static bool mir_verify_same_class_uses(const anvil_mir_func_t *func,
                                       const anvil_mir_instr_t *instr,
                                       size_t index,
                                       bool def_must_match,
                                       char *error,
                                       size_t error_len)
{
    if (instr->num_uses == 0) return true;

    const anvil_mir_vreg_info_t *first =
        mir_verify_vreg_info(func, instr->uses[0], index, error, error_len);
    if (!first) return false;

    for (size_t u = 1; u < instr->num_uses; u++) {
        const anvil_mir_vreg_info_t *info =
            mir_verify_vreg_info(func, instr->uses[u], index, error, error_len);
        if (!info) return false;
        if (info->reg_class != first->reg_class ||
            info->size_bits != first->size_bits) {
            return mir_verify_fail(error, error_len,
                                   "instruction %zu register class mismatch",
                                   index);
        }
    }

    if (def_must_match) {
        const anvil_mir_vreg_info_t *def =
            mir_verify_vreg_info(func, instr->def, index, error, error_len);
        if (!def) return false;
        if (def->reg_class != first->reg_class ||
            def->size_bits != first->size_bits) {
            return mir_verify_fail(error, error_len,
                                   "instruction %zu register class mismatch",
                                   index);
        }
    }

    return true;
}

static bool mir_verify_cast(const anvil_mir_func_t *func,
                            const anvil_mir_instr_t *instr,
                            size_t index, char *error, size_t error_len)
{
    if (!mir_verify_has_def(instr, index, error, error_len) ||
        !mir_verify_num_uses(instr, 1, index, error, error_len)) return false;
    const anvil_mir_vreg_info_t *dst =
        mir_verify_vreg_info(func, instr->def, index, error, error_len);
    const anvil_mir_vreg_info_t *src =
        mir_verify_vreg_info(func, instr->uses[0], index, error, error_len);
    if (!dst || !src) return false;
    bool valid = false;
    switch (instr->op) {
        case ANVIL_MIR_OP_ZEXT:
        case ANVIL_MIR_OP_SEXT:
            valid = src->reg_class == ANVIL_MIR_REG_GPR &&
                    dst->reg_class == ANVIL_MIR_REG_GPR &&
                    src->size_bits < dst->size_bits;
            break;
        case ANVIL_MIR_OP_TRUNC:
            valid = src->reg_class == ANVIL_MIR_REG_GPR &&
                    dst->reg_class == ANVIL_MIR_REG_GPR &&
                    src->size_bits > dst->size_bits;
            break;
        case ANVIL_MIR_OP_BITCAST:
            valid = src->size_bits == dst->size_bits;
            break;
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP:
            valid = src->reg_class == ANVIL_MIR_REG_GPR &&
                    dst->reg_class == ANVIL_MIR_REG_FPR;
            break;
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI:
            valid = src->reg_class == ANVIL_MIR_REG_FPR &&
                    dst->reg_class == ANVIL_MIR_REG_GPR;
            break;
        case ANVIL_MIR_OP_FPEXT:
            valid = src->reg_class == ANVIL_MIR_REG_FPR &&
                    dst->reg_class == ANVIL_MIR_REG_FPR &&
                    src->size_bits < dst->size_bits;
            break;
        case ANVIL_MIR_OP_FPTRUNC:
            valid = src->reg_class == ANVIL_MIR_REG_FPR &&
                    dst->reg_class == ANVIL_MIR_REG_FPR &&
                    src->size_bits > dst->size_bits;
            break;
        default:
            break;
    }
    return valid || mir_verify_fail(error, error_len,
                                    "instruction %zu has invalid cast widths or classes",
                                    index);
}

static bool mir_verify_blocks(const anvil_mir_func_t *func,
                              char *error, size_t error_len)
{
    bool *saw_instr = calloc(func->num_blocks, sizeof(*saw_instr));
    bool *saw_terminator = calloc(func->num_blocks, sizeof(*saw_terminator));
    if (!saw_instr || !saw_terminator) {
        free(saw_instr); free(saw_terminator);
        return mir_verify_fail(error, error_len,
                               "out of memory verifying MachineIR blocks");
    }
    for (size_t i = 0; i < func->num_instrs; i++) {
        anvil_mir_block_t block = func->instrs[i].block;
        if (saw_terminator[block]) {
            const char *name = func->blocks[block].name
                ? func->blocks[block].name
                : "";
            free(saw_instr); free(saw_terminator);
            return mir_verify_fail(error, error_len,
                                   "block %s has instructions after terminator",
                                   name);
        }
        saw_instr[block] = true;
        saw_terminator[block] = mir_op_is_terminator(func->instrs[i].op);
    }
    for (size_t block = 0; block < func->num_blocks; block++) {
        if (!saw_instr[block] || saw_terminator[block]) continue;
        const char *name = func->blocks[block].name
            ? func->blocks[block].name : "";
        free(saw_instr); free(saw_terminator);
        return mir_verify_fail(error, error_len,
                               "block %s is missing a terminator", name);
    }
    free(saw_instr); free(saw_terminator);
    return true;
}

static bool mir_verify_instr(const anvil_mir_func_t *func,
                             const anvil_mir_instr_t *instr,
                             size_t index,
                             char *error,
                             size_t error_len)
{
    if (!mir_valid_block(func, instr->block)) {
        return mir_verify_fail(error, error_len,
                               "instruction %zu references invalid block",
                               index);
    }

    if (instr->def != ANVIL_MIR_NO_VREG &&
        !mir_verify_vreg_info(func, instr->def, index, error, error_len)) {
        return false;
    }
    for (size_t u = 0; u < instr->num_uses; u++) {
        if (!mir_verify_vreg_info(func, instr->uses[u], index,
                                  error, error_len)) {
            return false;
        }
    }

    switch (instr->op) {
        case ANVIL_MIR_OP_MOV:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 0, index, error, error_len);

        case ANVIL_MIR_OP_COPY:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 1, index, error, error_len) &&
                   mir_verify_same_class_uses(func, instr, index, true,
                                              error, error_len);

        case ANVIL_MIR_OP_NOT:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 1, index, error, error_len) &&
                   mir_verify_def_class(func, instr, index,
                                        ANVIL_MIR_REG_GPR, error, error_len) &&
                   mir_verify_same_class_uses(func, instr, index, true,
                                              error, error_len);

        case ANVIL_MIR_OP_NEG:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 1, index, error, error_len) &&
                   mir_verify_same_class_uses(func, instr, index, true,
                                              error, error_len);

        case ANVIL_MIR_OP_FABS:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 1, index, error, error_len) &&
                   mir_verify_def_class(func, instr, index,
                                        ANVIL_MIR_REG_FPR, error, error_len) &&
                   mir_verify_same_class_uses(func, instr, index, true,
                                              error, error_len);

        case ANVIL_MIR_OP_ZEXT:
        case ANVIL_MIR_OP_SEXT:
        case ANVIL_MIR_OP_TRUNC:
        case ANVIL_MIR_OP_BITCAST:
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP:
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI:
        case ANVIL_MIR_OP_FPEXT:
        case ANVIL_MIR_OP_FPTRUNC:
            return mir_verify_cast(func, instr, index, error, error_len);

        case ANVIL_MIR_OP_ADD:
        case ANVIL_MIR_OP_SUB:
        case ANVIL_MIR_OP_MUL:
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_SDIV:
        case ANVIL_MIR_OP_UDIV:
        case ANVIL_MIR_OP_FDIV:
        case ANVIL_MIR_OP_SMOD:
        case ANVIL_MIR_OP_UMOD:
        case ANVIL_MIR_OP_AND:
        case ANVIL_MIR_OP_OR:
        case ANVIL_MIR_OP_XOR:
        case ANVIL_MIR_OP_SHL:
        case ANVIL_MIR_OP_SHR:
        case ANVIL_MIR_OP_SAR:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 2, index, error, error_len) &&
                   mir_verify_same_class_uses(func, instr, index, true,
                                              error, error_len);

        case ANVIL_MIR_OP_CMP:
        case ANVIL_MIR_OP_CMP_EQ:
        case ANVIL_MIR_OP_CMP_NE:
        case ANVIL_MIR_OP_CMP_LT:
        case ANVIL_MIR_OP_CMP_LE:
        case ANVIL_MIR_OP_CMP_GT:
        case ANVIL_MIR_OP_CMP_GE:
        case ANVIL_MIR_OP_CMP_ULT:
        case ANVIL_MIR_OP_CMP_ULE:
        case ANVIL_MIR_OP_CMP_UGT:
        case ANVIL_MIR_OP_CMP_UGE: {
            if (!mir_verify_has_def(instr, index, error, error_len) ||
                !mir_verify_num_uses(instr, 2, index, error, error_len) ||
                !mir_verify_def_class(func, instr, index, ANVIL_MIR_REG_GPR,
                                      error, error_len) ||
                !mir_verify_same_class_uses(func, instr, index, false,
                                            error, error_len)) return false;
            return func->vregs[instr->def].size_bits == 8 ||
                   mir_verify_fail(error, error_len,
                       "instruction %zu comparison result must be GPR8", index);
        }

        case ANVIL_MIR_OP_FCMP: {
            if (!mir_verify_has_def(instr, index, error, error_len) ||
                !mir_verify_num_uses(instr, 2, index, error, error_len) ||
                !mir_verify_def_class(func, instr, index, ANVIL_MIR_REG_GPR,
                                      error, error_len) ||
                !instr->has_imm || instr->imm < 0 || instr->imm > 15) return false;
            const anvil_mir_vreg_info_t *def = &func->vregs[instr->def];
            const anvil_mir_vreg_info_t *lhs = &func->vregs[instr->uses[0]];
            const anvil_mir_vreg_info_t *rhs = &func->vregs[instr->uses[1]];
            return (def->size_bits == 8 &&
                    lhs->reg_class == ANVIL_MIR_REG_FPR &&
                    rhs->reg_class == ANVIL_MIR_REG_FPR &&
                    lhs->size_bits == rhs->size_bits) ||
                   mir_verify_fail(error, error_len,
                       "instruction %zu has invalid FCMP classes or widths", index);
        }

        case ANVIL_MIR_OP_SELECT:
            if (!mir_verify_has_def(instr, index, error, error_len) ||
                !mir_verify_num_uses(instr, 3, index, error, error_len) ||
                !mir_verify_use_class(func, instr, index, 0,
                                      ANVIL_MIR_REG_GPR, error, error_len)) {
                return false;
            } else {
                const anvil_mir_vreg_info_t *def =
                    mir_verify_vreg_info(func, instr->def, index,
                                         error, error_len);
                const anvil_mir_vreg_info_t *then_info =
                    mir_verify_vreg_info(func, instr->uses[1], index,
                                         error, error_len);
                const anvil_mir_vreg_info_t *else_info =
                    mir_verify_vreg_info(func, instr->uses[2], index,
                                         error, error_len);
                const anvil_mir_vreg_info_t *cond_info =
                    mir_verify_vreg_info(func, instr->uses[0], index,
                                         error, error_len);
                if (!def || !then_info || !else_info || !cond_info) return false;
                if (cond_info->size_bits != 8)
                    return mir_verify_fail(error, error_len,
                        "instruction %zu select condition must be GPR8", index);
                if (def->reg_class == then_info->reg_class &&
                    def->reg_class == else_info->reg_class) {
                    return true;
                }
                return mir_verify_fail(error, error_len,
                                       "instruction %zu register class mismatch",
                                       index);
            }

        case ANVIL_MIR_OP_SYMBOL_ADDR:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 0, index, error, error_len) &&
                   mir_verify_def_class(func, instr, index, ANVIL_MIR_REG_GPR,
                                        error, error_len) &&
                   instr->symbol && instr->symbol[0];

        case ANVIL_MIR_OP_LOAD:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 1, index, error, error_len) &&
                   mir_verify_use_class(func, instr, index, 0,
                                        ANVIL_MIR_REG_GPR, error, error_len);

        case ANVIL_MIR_OP_STORE:
            return mir_verify_no_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 2, index, error, error_len) &&
                   mir_verify_use_class(func, instr, index, 1,
                                        ANVIL_MIR_REG_GPR, error, error_len);

        case ANVIL_MIR_OP_FRAME_ADDR:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 0, index, error, error_len) &&
                   mir_verify_def_class(func, instr, index, ANVIL_MIR_REG_GPR,
                                        error, error_len) &&
                   instr->frame_slot >= 0 &&
                   (size_t)instr->frame_slot < func->num_frame_slots;

        case ANVIL_MIR_OP_DYN_ALLOCA:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 1, index, error, error_len) &&
                   mir_verify_def_class(func, instr, index, ANVIL_MIR_REG_GPR,
                                        error, error_len) &&
                   mir_verify_use_class(func, instr, index, 0,
                                        ANVIL_MIR_REG_GPR, error, error_len) &&
                   instr->has_imm && instr->imm > 0;

        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 0, index, error, error_len) &&
                   instr->has_imm && instr->imm >= 0;

        case ANVIL_MIR_OP_CALL:
            if (instr->has_imm && (instr->imm < 0 || instr->imm > 8)) {
                return mir_verify_fail(error, error_len,
                    "instruction %zu has an invalid variadic vector-register count",
                    index);
            }
            if (instr->symbol && instr->symbol[0]) {
                return mir_verify_num_uses(instr, instr->num_uses, index,
                                           error, error_len);
            }
            return instr->num_uses >= 1 &&
                   mir_verify_use_class(func, instr, index, 0,
                                        ANVIL_MIR_REG_GPR, error, error_len);

        case ANVIL_MIR_OP_CALL_STACK_ARG:
            return mir_verify_no_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 1, index, error, error_len) &&
                   instr->has_imm && instr->imm >= 0;

        case ANVIL_MIR_OP_RET:
            return mir_verify_no_def(instr, index, error, error_len) &&
                   instr->num_uses <= 1;

        case ANVIL_MIR_OP_BR:
            return mir_verify_no_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 0, index, error, error_len) &&
                   mir_valid_block(func, instr->true_block);

        case ANVIL_MIR_OP_BR_COND:
            if (!mir_verify_no_def(instr, index, error, error_len) ||
                !mir_verify_num_uses(instr, 1, index, error, error_len) ||
                !mir_verify_use_class(func, instr, index, 0,
                                      ANVIL_MIR_REG_GPR, error, error_len) ||
                !mir_valid_block(func, instr->true_block) ||
                !mir_valid_block(func, instr->false_block)) return false;
            return func->vregs[instr->uses[0]].size_bits == 8 ||
                   mir_verify_fail(error, error_len,
                       "instruction %zu branch condition must be GPR8", index);

        case ANVIL_MIR_OP_SPILL_LOAD:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 0, index, error, error_len) &&
                   instr->spill_slot >= 0 &&
                   (size_t)instr->spill_slot < func->num_spills;

        case ANVIL_MIR_OP_SPILL_STORE:
            return mir_verify_no_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 1, index, error, error_len) &&
                   instr->spill_slot >= 0 &&
                   (size_t)instr->spill_slot < func->num_spills;

        case ANVIL_MIR_OP_KEEPALIVE:
            return mir_verify_no_def(instr, index, error, error_len) &&
                   instr->num_uses > 0 && !instr->symbol;

        case ANVIL_MIR_OP_CALL_RESULT: {
            const anvil_mir_vreg_info_t *def =
                mir_verify_vreg_info(func, instr->def, index, error, error_len);
            size_t bundle_start = index;
            while (bundle_start > 0 &&
                   func->instrs[bundle_start - 1].block == instr->block &&
                   func->instrs[bundle_start - 1].op ==
                       ANVIL_MIR_OP_CALL_RESULT) {
                bundle_start--;
            }
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 0, index, error, error_len) &&
                   def && def->has_fixed_reg && bundle_start > 0 &&
                   func->instrs[bundle_start - 1].block == instr->block &&
                   func->instrs[bundle_start - 1].op == ANVIL_MIR_OP_CALL;
        }

        case ANVIL_MIR_OP_RET_VALUE_PART: {
            size_t bundle_end = index + 1;
            while (bundle_end < func->num_instrs &&
                   func->instrs[bundle_end].block == instr->block &&
                   func->instrs[bundle_end].op ==
                       ANVIL_MIR_OP_RET_VALUE_PART) {
                bundle_end++;
            }
            return mir_verify_no_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 1, index, error, error_len) &&
                   bundle_end < func->num_instrs &&
                   func->instrs[bundle_end].block == instr->block &&
                   func->instrs[bundle_end].op == ANVIL_MIR_OP_RET;
        }

        case ANVIL_MIR_OP_INVALID:
        default:
            return mir_verify_fail(error, error_len,
                                   "instruction %zu has invalid opcode", index);
    }
}

static bool mir_verify_cfg_and_assignment(const anvil_mir_func_t *func,
                                          char *error, size_t error_len)
{
    size_t blocks = func->num_blocks;
    size_t vregs = func->num_vregs;
    if (blocks == 0 || vregs > SIZE_MAX - 63) {
        return mir_verify_fail(error, error_len, "MachineIR CFG is too large");
    }
    size_t words = vregs == 0 ? 0 : (vregs + 63) / 64;
    if (blocks > SIZE_MAX / (2 * sizeof(anvil_mir_block_t)) ||
        blocks > SIZE_MAX / (2 * sizeof(size_t)) ||
        blocks > SIZE_MAX / sizeof(size_t) ||
        blocks == SIZE_MAX ||
        blocks + 1 > SIZE_MAX / sizeof(size_t) ||
        (words && blocks > (SIZE_MAX / sizeof(uint64_t)) / words)) {
        return mir_verify_fail(error, error_len, "MachineIR CFG is too large");
    }
    anvil_mir_block_t *succ = malloc(blocks * 2 * sizeof(*succ));
    uint8_t *succ_count = calloc(blocks, sizeof(*succ_count));
    size_t *pred_count = calloc(blocks, sizeof(*pred_count));
    size_t *pred_offset = calloc(blocks + 1, sizeof(*pred_offset));
    size_t *last_instr = malloc(blocks * sizeof(*last_instr));
    bool *reachable = calloc(blocks, sizeof(*reachable));
    bool *seen_block = calloc(blocks, sizeof(*seen_block));
    size_t *queue = malloc(blocks * sizeof(*queue));
    if (!succ || !succ_count || !pred_count || !pred_offset || !last_instr ||
        !reachable || !seen_block || !queue) {
        free(succ); free(succ_count); free(pred_count); free(pred_offset);
        free(last_instr); free(reachable); free(seen_block); free(queue);
        return mir_verify_fail(error, error_len, "out of memory verifying MachineIR CFG");
    }
    for (size_t b = 0; b < blocks; b++) last_instr[b] = SIZE_MAX;
    for (size_t i = 0; i < func->num_instrs; i++) {
        anvil_mir_block_t block = func->instrs[i].block;
        seen_block[block] = true;
        last_instr[block] = i;
    }
    for (size_t b = 0; b < blocks; b++) {
        size_t last = last_instr[b];
        if (last == SIZE_MAX) continue;
        const anvil_mir_instr_t *term = &func->instrs[last];
        if (term->op == ANVIL_MIR_OP_BR) {
            succ[b * 2] = term->true_block;
            succ_count[b] = 1;
        } else if (term->op == ANVIL_MIR_OP_BR_COND) {
            succ[b * 2] = term->true_block;
            succ_count[b] = 1;
            if (term->false_block != term->true_block) {
                succ[b * 2 + 1] = term->false_block;
                succ_count[b] = 2;
            }
        }
        for (size_t s = 0; s < succ_count[b]; s++) {
            pred_count[succ[b * 2 + s]]++;
        }
    }
    size_t edge_count = 0;
    for (size_t b = 0; b < blocks; b++) {
        pred_offset[b] = edge_count;
        if (pred_count[b] > SIZE_MAX - edge_count) {
            free(succ); free(succ_count); free(pred_count); free(pred_offset);
            free(last_instr); free(reachable); free(seen_block); free(queue);
            return mir_verify_fail(error, error_len, "MachineIR CFG is too large");
        }
        edge_count += pred_count[b];
    }
    pred_offset[blocks] = edge_count;
    size_t *pred = edge_count ? malloc(edge_count * sizeof(*pred)) : NULL;
    size_t *pred_cursor = calloc(blocks, sizeof(*pred_cursor));
    if ((edge_count && !pred) || !pred_cursor) {
        free(succ); free(succ_count); free(pred_count); free(pred_offset);
        free(last_instr); free(reachable); free(seen_block); free(queue);
        free(pred); free(pred_cursor);
        return mir_verify_fail(error, error_len, "out of memory verifying MachineIR CFG");
    }
    for (size_t from = 0; from < blocks; from++) {
        for (size_t s = 0; s < succ_count[from]; s++) {
            size_t to = succ[from * 2 + s];
            pred[pred_offset[to] + pred_cursor[to]++] = from;
        }
    }
    reachable[0] = true;
    size_t qhead = 0, qtail = 0;
    queue[qtail++] = 0;
    while (qhead < qtail) {
        size_t from = queue[qhead++];
        for (size_t s = 0; s < succ_count[from]; s++) {
            size_t to = succ[from * 2 + s];
            if (!reachable[to]) {
                reachable[to] = true;
                queue[qtail++] = to;
            }
        }
    }
    for (size_t b = 0; b < blocks; b++) {
        if (!reachable[b]) {
            free(succ); free(succ_count); free(pred_count); free(pred_offset);
            free(last_instr); free(reachable); free(seen_block); free(queue);
            free(pred); free(pred_cursor);
            return mir_verify_fail(error, error_len, "block %s is unreachable",
                func->blocks[b].name ? func->blocks[b].name : "");
        }
        if (!seen_block[b]) {
            free(succ); free(succ_count); free(pred_count); free(pred_offset);
            free(last_instr); free(reachable); free(seen_block); free(queue);
            free(pred); free(pred_cursor);
            return mir_verify_fail(error, error_len, "reachable block %s is empty",
                func->blocks[b].name ? func->blocks[b].name : "");
        }
    }
    free(last_instr); free(reachable); free(seen_block); free(pred_cursor);

    if (vregs == 0) {
        free(succ); free(succ_count); free(pred_count); free(pred_offset);
        free(queue); free(pred);
        return true;
    }
    uint64_t *in = calloc(blocks * words, sizeof(*in));
    uint64_t *out = calloc(blocks * words, sizeof(*out));
    uint64_t *gen = calloc(blocks * words, sizeof(*gen));
    uint64_t *state = malloc(words * sizeof(*state));
    bool *queued = calloc(blocks, sizeof(*queued));
    if (!in || !out || !gen || !state || !queued) {
        free(succ); free(succ_count); free(pred_count); free(pred_offset);
        free(queue); free(pred); free(in); free(out); free(gen); free(state);
        free(queued);
        return mir_verify_fail(error, error_len,
                               "out of memory verifying MachineIR dataflow");
    }
    uint64_t final_mask = vregs % 64 ? (UINT64_C(1) << (vregs % 64)) - 1
                                     : UINT64_MAX;
    for (size_t b = 1; b < blocks; b++) {
        for (size_t w = 0; w < words; w++) in[b * words + w] = UINT64_MAX;
        in[b * words + words - 1] &= final_mask;
    }
    for (size_t v = 0; v < vregs; v++) {
        if (func->vregs[v].is_live_in) {
            in[v / 64] |= UINT64_C(1) << (v % 64);
        }
    }
    for (size_t i = 0; i < func->num_instrs; i++) {
        const anvil_mir_instr_t *instr = &func->instrs[i];
        if (instr->def != ANVIL_MIR_NO_VREG) {
            gen[(size_t)instr->block * words + instr->def / 64] |=
                UINT64_C(1) << (instr->def % 64);
        }
    }
    /* Must-def analysis uses the greatest fixed point for loop headers: all
       non-entry facts start true and are removed by predecessor intersections. */
    for (size_t b = 0; b < blocks; b++) {
        for (size_t w = 0; w < words; w++) {
            out[b * words + w] = in[b * words + w] | gen[b * words + w];
        }
    }
    qhead = 0; qtail = 0;
    for (size_t b = 0; b < blocks; b++) {
        queue[b] = b;
        queued[b] = true;
    }
    size_t qcount = blocks;
    while (qcount > 0) {
        size_t b = queue[qhead];
        qhead = (qhead + 1) % blocks;
        qcount--;
        queued[b] = false;
        if (b != 0) {
            memcpy(state, &out[pred[pred_offset[b]] * words],
                   words * sizeof(*state));
            for (size_t pi = pred_offset[b] + 1; pi < pred_offset[b + 1]; pi++) {
                const uint64_t *pred_out = &out[pred[pi] * words];
                for (size_t w = 0; w < words; w++) state[w] &= pred_out[w];
            }
            memcpy(&in[b * words], state, words * sizeof(*state));
        }
        bool changed = false;
        for (size_t w = 0; w < words; w++) {
            uint64_t next = in[b * words + w] | gen[b * words + w];
            if (out[b * words + w] != next) {
                out[b * words + w] = next;
                changed = true;
            }
        }
        if (changed) {
            for (size_t s = 0; s < succ_count[b]; s++) {
                size_t to = succ[b * 2 + s];
                if (!queued[to]) {
                    queue[qtail] = to;
                    qtail = (qtail + 1) % blocks;
                    qcount++;
                    queued[to] = true;
                }
            }
        }
    }

    uint64_t *block_state = malloc(blocks * words * sizeof(*block_state));
    if (!block_state) {
        free(succ); free(succ_count); free(pred_count); free(pred_offset);
        free(queue); free(pred); free(in); free(out); free(gen); free(state);
        free(queued);
        return mir_verify_fail(error, error_len,
                               "out of memory verifying MachineIR dataflow");
    }
    memcpy(block_state, in, blocks * words * sizeof(*block_state));
    for (size_t i = 0; i < func->num_instrs; i++) {
        const anvil_mir_instr_t *instr = &func->instrs[i];
        uint64_t *block_bits = &block_state[(size_t)instr->block * words];
        for (size_t u = 0; u < instr->num_uses; u++) {
            anvil_mir_vreg_t use = instr->uses[u];
            if ((block_bits[use / 64] & (UINT64_C(1) << (use % 64))) == 0) {
                free(succ); free(succ_count); free(pred_count); free(pred_offset);
                free(queue); free(pred); free(in); free(out); free(gen); free(state);
                free(queued); free(block_state);
                return mir_verify_fail(error, error_len,
                    "instruction %zu uses vreg %u before definite assignment",
                    i, use);
            }
        }
        if (instr->def != ANVIL_MIR_NO_VREG) {
            block_bits[instr->def / 64] |= UINT64_C(1) << (instr->def % 64);
        }
    }
    free(succ); free(succ_count); free(pred_count); free(pred_offset);
    free(queue); free(pred); free(in); free(out); free(gen); free(state);
    free(queued); free(block_state);
    return true;
}

bool anvil_mir_verify(const anvil_mir_func_t *func,
                      char *error,
                      size_t error_len)
{
    if (error && error_len > 0) error[0] = '\0';
    if (!func) {
        return mir_verify_fail(error, error_len, "MachineIR function is null");
    }
    if (func->num_blocks == 0) {
        return mir_verify_fail(error, error_len, "MachineIR has no blocks");
    }

    for (size_t i = 0; i < func->num_vregs; i++) {
        const anvil_mir_vreg_info_t *info = &func->vregs[i];
        if (!valid_reg_class(info->reg_class) || info->size_bits == 0) {
            return mir_verify_fail(error, error_len,
                                   "vreg %zu has invalid metadata", i);
        }
        if (info->has_fixed_reg && info->fixed_phys_reg < 0) {
            return mir_verify_fail(error, error_len,
                                   "vreg %zu has invalid fixed register", i);
        }
        if (info->is_live_in && !info->has_fixed_reg) {
            return mir_verify_fail(error, error_len,
                                   "vreg %zu is a live-in without a fixed register", i);
        }
    }

    for (size_t i = 0; i < func->num_instrs; i++) {
        if (!mir_verify_instr(func, &func->instrs[i], i, error, error_len)) {
            if (error && error_len > 0 && error[0] == '\0') {
                snprintf(error, error_len,
                         "instruction %zu failed MachineIR verification", i);
            }
            return false;
        }
    }

    if (!mir_verify_blocks(func, error, error_len)) return false;
    return mir_verify_cfg_and_assignment(func, error, error_len);
}

static void free_instr_contents(anvil_mir_instr_t *instr)
{
    if (!instr) return;
    free(instr->uses);
    free(instr->symbol);
}

static void free_instr_array(anvil_mir_instr_t *instrs, size_t count)
{
    if (!instrs) return;
    for (size_t i = 0; i < count; i++) {
        free_instr_contents(&instrs[i]);
    }
    free(instrs);
}

static bool reserve_spill_slots(anvil_mir_func_t *func, size_t needed)
{
    if (needed <= func->cap_spills) return true;

    size_t new_cap = func->cap_spills ? func->cap_spills * 2 : 8;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }

    anvil_mir_spill_slot_info_t *grown =
        realloc(func->spill_slots, new_cap * sizeof(*grown));
    if (!grown) return false;

    func->spill_slots = grown;
    func->cap_spills = new_cap;
    return true;
}

int anvil_mir_allocate_spill_slot(anvil_mir_func_t *func,
                                  anvil_mir_reg_class_t reg_class,
                                  uint16_t size_bits)
{
    if (!func || !valid_reg_class(reg_class) || size_bits == 0) return -1;
    if (func->num_spills > (size_t)INT_MAX) return -1;
    if (!reserve_spill_slots(func, func->num_spills + 1)) return -1;

    int slot = (int)func->num_spills++;
    func->spill_slots[slot].reg_class = reg_class;
    func->spill_slots[slot].size_bits = size_bits;
    return slot;
}

bool anvil_mir_get_spill_slot_info(const anvil_mir_func_t *func,
                                   int spill_slot,
                                   anvil_mir_spill_slot_info_t *out_info)
{
    if (!func || !out_info || spill_slot < 0 ||
        (size_t)spill_slot >= func->num_spills) {
        return false;
    }

    *out_info = func->spill_slots[spill_slot];
    return true;
}

size_t anvil_mir_num_frame_slots(const anvil_mir_func_t *func)
{
    return func ? func->num_frame_slots : 0;
}

bool anvil_mir_get_frame_slot_info(const anvil_mir_func_t *func,
                                   int frame_slot,
                                   anvil_mir_frame_slot_info_t *out_info)
{
    if (!func || !out_info || frame_slot < 0 ||
        (size_t)frame_slot >= func->num_frame_slots) {
        return false;
    }

    *out_info = func->frame_slots[frame_slot];
    return true;
}

size_t anvil_mir_num_string_literals(const anvil_mir_func_t *func)
{
    return func ? func->num_string_literals : 0;
}

bool anvil_mir_get_string_literal_info(
    const anvil_mir_func_t *func,
    size_t index,
    anvil_mir_string_literal_info_t *out_info)
{
    if (!func || !out_info || index >= func->num_string_literals) {
        return false;
    }

    out_info->label = func->string_literals[index].label;
    out_info->value = func->string_literals[index].value;
    return true;
}

static const anvil_regalloc_class_config_t *
spill_config_for_class(const anvil_regalloc_class_config_t *configs,
                       size_t num_configs,
                       anvil_mir_reg_class_t reg_class)
{
    for (size_t i = 0; i < num_configs; i++) {
        if (configs[i].reg_class == reg_class) return &configs[i];
    }
    return NULL;
}

static int config_phys_reg_at(const anvil_regalloc_class_config_t *config,
                              size_t index)
{
    return config->phys_regs ? config->phys_regs[index] : (int)index;
}

static bool assignment_is_spilled(const anvil_mir_func_t *func,
                                  anvil_mir_vreg_t vreg)
{
    return anvil_mir_valid_vreg(func, vreg) &&
           func->assignments &&
           func->assignments[vreg].spilled;
}

static bool scratch_phys_live_at_instr(const anvil_mir_func_t *func,
                                       size_t original_vregs,
                                       anvil_mir_reg_class_t reg_class,
                                       int phys_reg,
                                       size_t instr_index)
{
    for (size_t v = 0; v < original_vregs; v++) {
        const anvil_regalloc_assignment_t *assignment = &func->assignments[v];
        if (assignment->spilled || assignment->reg_class != reg_class ||
            assignment->phys_reg != phys_reg) {
            continue;
        }
        size_t first = SIZE_MAX;
        size_t last = 0;
        for (size_t i = 0; i < func->num_instrs; i++) {
            const anvil_mir_instr_t *instr = &func->instrs[i];
            bool occurs = instr->def == (anvil_mir_vreg_t)v;
            for (size_t u = 0; !occurs && u < instr->num_uses; u++) {
                occurs = instr->uses[u] == (anvil_mir_vreg_t)v;
            }
            if (!occurs) continue;
            if (first == SIZE_MAX) first = i;
            last = i;
        }
        if (first != SIZE_MAX && first <= instr_index && instr_index <= last) {
            return true;
        }
    }
    return false;
}

static bool instruction_scratch_needs(const anvil_mir_func_t *func,
                                      const anvil_mir_instr_t *instr,
                                      size_t needs[ANVIL_MIR_REG_CLASS_COUNT])
{
    for (size_t i = 0; i < ANVIL_MIR_REG_CLASS_COUNT; i++) {
        needs[i] = 0;
    }

    for (size_t u = 0; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = instr->uses[u];
        if (!assignment_is_spilled(func, use)) continue;

        anvil_mir_reg_class_t reg_class = func->vregs[use].reg_class;
        if (!valid_reg_class(reg_class)) return false;
        needs[reg_class]++;
    }

    if (assignment_is_spilled(func, instr->def)) {
        anvil_mir_reg_class_t reg_class = func->vregs[instr->def].reg_class;
        if (!valid_reg_class(reg_class)) return false;
        needs[reg_class]++;
    }

    return true;
}

static bool scratch_configs_cover_spills(
    const anvil_mir_func_t *func,
    const anvil_regalloc_class_config_t *scratch_configs,
    size_t num_scratch_configs)
{
    for (size_t i = 0; i < func->num_instrs; i++) {
        size_t needs[ANVIL_MIR_REG_CLASS_COUNT];
        if (!instruction_scratch_needs(func, &func->instrs[i], needs)) {
            return false;
        }

        for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++) {
            if (needs[c] == 0) continue;

            const anvil_regalloc_class_config_t *config =
                spill_config_for_class(scratch_configs, num_scratch_configs,
                                       (anvil_mir_reg_class_t)c);
            if (!config || config->num_phys_regs < 0 ||
                needs[c] > (size_t)config->num_phys_regs) {
                return false;
            }
        }
    }

    return true;
}

static anvil_mir_vreg_t add_spill_temp_vreg(anvil_mir_func_t *func,
                                            anvil_mir_reg_class_t reg_class,
                                            uint16_t size_bits,
                                            bool is_signed,
                                            int phys_reg)
{
    if (!func || !func->assignments || func->num_vregs >= UINT32_MAX) {
        return ANVIL_MIR_NO_VREG;
    }
    if (!valid_reg_class(reg_class) || size_bits == 0 || phys_reg < 0) {
        return ANVIL_MIR_NO_VREG;
    }
    if (!reserve_vregs(func, func->num_vregs + 1)) return ANVIL_MIR_NO_VREG;

    anvil_regalloc_assignment_t *assignments =
        realloc(func->assignments, (func->num_vregs + 1) * sizeof(*assignments));
    if (!assignments) return ANVIL_MIR_NO_VREG;
    func->assignments = assignments;

    anvil_mir_vreg_t vreg = (anvil_mir_vreg_t)func->num_vregs++;
    func->vregs[vreg].reg_class = reg_class;
    func->vregs[vreg].size_bits = size_bits;
    func->vregs[vreg].is_signed = is_signed;
    func->vregs[vreg].is_live_in = false;
    func->vregs[vreg].has_fixed_reg = true;
    func->vregs[vreg].fixed_phys_reg = phys_reg;

    func->assignments[vreg].reg_class = reg_class;
    func->assignments[vreg].spilled = false;
    func->assignments[vreg].phys_reg = phys_reg;
    func->assignments[vreg].spill_slot = -1;
    return vreg;
}

static bool reserve_materialized_instrs(anvil_mir_instr_t **instrs,
                                        size_t *cap_instrs,
                                        size_t needed)
{
    if (needed <= *cap_instrs) return true;

    size_t new_cap = *cap_instrs ? *cap_instrs * 2 : 32;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return false;
        new_cap *= 2;
    }

    anvil_mir_instr_t *grown = realloc(*instrs, new_cap * sizeof(*grown));
    if (!grown) return false;

    *instrs = grown;
    *cap_instrs = new_cap;
    return true;
}

static bool append_owned_materialized_instr(anvil_mir_instr_t **instrs,
                                            size_t *num_instrs,
                                            size_t *cap_instrs,
                                            anvil_mir_instr_t *instr)
{
    if (!reserve_materialized_instrs(instrs, cap_instrs, *num_instrs + 1)) {
        return false;
    }

    (*instrs)[(*num_instrs)++] = *instr;
    memset(instr, 0, sizeof(*instr));
    instr->def = ANVIL_MIR_NO_VREG;
    instr->true_block = ANVIL_MIR_NO_BLOCK;
    instr->false_block = ANVIL_MIR_NO_BLOCK;
    instr->spill_slot = -1;
    instr->frame_slot = -1;
    return true;
}

static bool clone_instr_for_materialization(const anvil_mir_instr_t *src,
                                            anvil_mir_instr_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    *dst = *src;
    dst->uses = NULL;
    dst->symbol = NULL;

    if (src->num_uses > 0) {
        dst->uses = malloc(src->num_uses * sizeof(*dst->uses));
        if (!dst->uses) return false;
        memcpy(dst->uses, src->uses, src->num_uses * sizeof(*dst->uses));
    }

    if (src->symbol) {
        dst->symbol = mir_strdup(src->symbol);
        if (!dst->symbol) {
            free(dst->uses);
            dst->uses = NULL;
            return false;
        }
    }

    return true;
}

static bool append_spill_load(anvil_mir_instr_t **instrs,
                              size_t *num_instrs,
                              size_t *cap_instrs,
                              anvil_mir_block_t block,
                              anvil_mir_vreg_t temp,
                              int spill_slot)
{
    anvil_mir_instr_t instr;
    memset(&instr, 0, sizeof(instr));
    instr.op = ANVIL_MIR_OP_SPILL_LOAD;
    instr.def = temp;
    instr.block = block;
    instr.true_block = ANVIL_MIR_NO_BLOCK;
    instr.false_block = ANVIL_MIR_NO_BLOCK;
    instr.spill_slot = spill_slot;
    instr.frame_slot = -1;
    return append_owned_materialized_instr(instrs, num_instrs, cap_instrs,
                                           &instr);
}

static bool append_spill_store(anvil_mir_instr_t **instrs,
                               size_t *num_instrs,
                               size_t *cap_instrs,
                               anvil_mir_block_t block,
                               anvil_mir_vreg_t temp,
                               int spill_slot)
{
    anvil_mir_instr_t instr;
    memset(&instr, 0, sizeof(instr));
    instr.op = ANVIL_MIR_OP_SPILL_STORE;
    instr.def = ANVIL_MIR_NO_VREG;
    instr.uses = malloc(sizeof(*instr.uses));
    if (!instr.uses) return false;
    instr.uses[0] = temp;
    instr.num_uses = 1;
    instr.block = block;
    instr.true_block = ANVIL_MIR_NO_BLOCK;
    instr.false_block = ANVIL_MIR_NO_BLOCK;
    instr.spill_slot = spill_slot;
    instr.frame_slot = -1;

    if (!append_owned_materialized_instr(instrs, num_instrs, cap_instrs,
                                         &instr)) {
        free_instr_contents(&instr);
        return false;
    }
    return true;
}

static bool next_scratch_phys(const anvil_regalloc_class_config_t *configs,
                              size_t num_configs,
                              anvil_mir_reg_class_t reg_class,
                              size_t used[ANVIL_MIR_REG_CLASS_COUNT],
                              const anvil_mir_func_t *func,
                              size_t original_vregs,
                              size_t instr_index,
                              int *out_phys)
{
    const anvil_regalloc_class_config_t *config =
        spill_config_for_class(configs, num_configs, reg_class);
    if (!config || config->num_phys_regs <= 0) return false;

    size_t index = used[reg_class];
    while (index < (size_t)config->num_phys_regs) {
        int candidate = config_phys_reg_at(config, index++);
        if (candidate < 0 || scratch_phys_live_at_instr(
                func, original_vregs, reg_class, candidate, instr_index)) {
            continue;
        }
        used[reg_class] = index;
        *out_phys = candidate;
        return true;
    }
    used[reg_class] = index;
    return false;
}

static void recompute_block_instr_ranges(anvil_mir_func_t *func)
{
    for (size_t b = 0; b < func->num_blocks; b++) {
        func->blocks[b].first_instr = SIZE_MAX;
        func->blocks[b].num_instrs = 0;
    }

    for (size_t i = 0; i < func->num_instrs; i++) {
        anvil_mir_block_t block = func->instrs[i].block;
        if (!mir_valid_block(func, block)) continue;

        if (func->blocks[block].num_instrs == 0) {
            func->blocks[block].first_instr = i;
        }
        func->blocks[block].num_instrs++;
    }
}

bool anvil_mir_materialize_spills(
    anvil_mir_func_t *func,
    const anvil_regalloc_class_config_t *scratch_configs,
    size_t num_scratch_configs)
{
    if (!func || !func->assignments) return false;
    if (func->num_spills == 0) return true;
    if (!scratch_configs || num_scratch_configs == 0) return false;

    size_t original_vregs = func->num_vregs;
    if (!scratch_configs_cover_spills(func, scratch_configs,
                                      num_scratch_configs)) {
        return false;
    }
    anvil_mir_instr_t *new_instrs = NULL;
    size_t new_num_instrs = 0;
    size_t new_cap_instrs = 0;

    for (size_t i = 0; i < func->num_instrs; i++) {
        const anvil_mir_instr_t *old = &func->instrs[i];
        size_t scratch_used[ANVIL_MIR_REG_CLASS_COUNT] = { 0 };

        anvil_mir_instr_t patched;
        if (!clone_instr_for_materialization(old, &patched)) {
            free_instr_array(new_instrs, new_num_instrs);
            return false;
        }

        for (size_t u = 0; u < patched.num_uses; u++) {
            anvil_mir_vreg_t use = patched.uses[u];
            if (!assignment_is_spilled(func, use)) continue;

            const anvil_mir_vreg_info_t *info = &func->vregs[use];
            int phys_reg = -1;
            if (!next_scratch_phys(scratch_configs, num_scratch_configs,
                                   info->reg_class, scratch_used, func,
                                   original_vregs, i, &phys_reg)) {
                free_instr_contents(&patched);
                free_instr_array(new_instrs, new_num_instrs);
                return false;
            }

            anvil_mir_vreg_t temp =
                add_spill_temp_vreg(func, info->reg_class,
                                    info->size_bits, info->is_signed,
                                    phys_reg);
            if (temp == ANVIL_MIR_NO_VREG ||
                !append_spill_load(&new_instrs, &new_num_instrs,
                                   &new_cap_instrs, old->block, temp,
                                   func->assignments[use].spill_slot)) {
                free_instr_contents(&patched);
                free_instr_array(new_instrs, new_num_instrs);
                return false;
            }
            patched.uses[u] = temp;
        }

        bool spilled_def = assignment_is_spilled(func, patched.def);
        anvil_mir_vreg_t def_temp = ANVIL_MIR_NO_VREG;
        int def_spill_slot = -1;
        if (spilled_def) {
            anvil_mir_vreg_t original_def = patched.def;
            const anvil_mir_vreg_info_t *info = &func->vregs[original_def];
            int phys_reg = -1;
            if (!next_scratch_phys(scratch_configs, num_scratch_configs,
                                   info->reg_class, scratch_used, func,
                                   original_vregs, i, &phys_reg)) {
                free_instr_contents(&patched);
                free_instr_array(new_instrs, new_num_instrs);
                return false;
            }

            def_temp = add_spill_temp_vreg(func, info->reg_class,
                                           info->size_bits, info->is_signed,
                                           phys_reg);
            if (def_temp == ANVIL_MIR_NO_VREG) {
                free_instr_contents(&patched);
                free_instr_array(new_instrs, new_num_instrs);
                return false;
            }
            def_spill_slot = func->assignments[original_def].spill_slot;
            patched.def = def_temp;
        }

        if (!append_owned_materialized_instr(&new_instrs, &new_num_instrs,
                                             &new_cap_instrs, &patched)) {
            free_instr_contents(&patched);
            free_instr_array(new_instrs, new_num_instrs);
            return false;
        }

        if (spilled_def &&
            !append_spill_store(&new_instrs, &new_num_instrs, &new_cap_instrs,
                                old->block, def_temp, def_spill_slot)) {
            free_instr_array(new_instrs, new_num_instrs);
            return false;
        }
    }

    free_instr_array(func->instrs, func->num_instrs);
    func->instrs = new_instrs;
    func->num_instrs = new_num_instrs;
    func->cap_instrs = new_cap_instrs;
    recompute_block_instr_ranges(func);
    return true;
}

void anvil_mir_clear_allocations(anvil_mir_func_t *func)
{
    if (!func) return;
    free(func->assignments);
    func->assignments = NULL;
    free(func->spill_slots);
    func->spill_slots = NULL;
    func->num_spills = 0;
    func->cap_spills = 0;
}

bool anvil_mir_prepare_assignments(anvil_mir_func_t *func)
{
    if (!func) return false;

    anvil_mir_clear_allocations(func);
    if (func->num_vregs == 0) return true;

    func->assignments = calloc(func->num_vregs, sizeof(*func->assignments));
    if (!func->assignments) return false;

    for (size_t i = 0; i < func->num_vregs; i++) {
        func->assignments[i].reg_class = func->vregs[i].reg_class;
        func->assignments[i].phys_reg = -1;
        func->assignments[i].spill_slot = -1;
    }

    return true;
}

const anvil_regalloc_assignment_t *
anvil_mir_get_assignment(const anvil_mir_func_t *func, anvil_mir_vreg_t vreg)
{
    if (!anvil_mir_valid_vreg(func, vreg) || !func->assignments) return NULL;
    return &func->assignments[vreg];
}

size_t anvil_mir_num_spills(const anvil_mir_func_t *func)
{
    return func ? func->num_spills : 0;
}
