/*
 * ANVIL - MachineIR container implementation.
 */

#include "machine_internal.h"
#include "anvil/anvil_internal.h"

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

    func->linkage = ANVIL_LINK_EXTERNAL;
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

void anvil_mir_func_set_linkage(anvil_mir_func_t *func, anvil_linkage_t linkage)
{
    if (func)
        func->linkage = linkage;
}

anvil_linkage_t anvil_mir_func_linkage(const anvil_mir_func_t *func)
{
    return func ? func->linkage : ANVIL_LINK_EXTERNAL;
}

void anvil_mir_func_set_cpu_features(anvil_mir_func_t *func, anvil_cpu_features_t features)
{
    if (func)
        func->cpu_features = features;
}

anvil_cpu_features_t anvil_mir_func_cpu_features(const anvil_mir_func_t *func)
{
    return func ? func->cpu_features : 0;
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
    func->blocks[block].cold = false;
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

bool anvil_mir_loop_header_blocks(const anvil_mir_func_t *func, bool *headers)
{
    if (!func || !headers)
        return false;
    for (size_t block = 0; block < func->num_blocks; block++)
        headers[block] = false;
    for (size_t index = 0; index < func->num_instrs; index++) {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        /* A cold block's jump back into its loop body is no loop edge. */
        if ((instr->op != ANVIL_MIR_OP_BR && instr->op != ANVIL_MIR_OP_BR_COND) || !mir_valid_block(func, instr->block) ||
            func->blocks[instr->block].cold)
            continue;
        anvil_mir_block_t targets[] = {instr->true_block, instr->false_block};
        for (size_t target = 0; target < 2; target++) {
            if (mir_valid_block(func, targets[target]) && targets[target] <= instr->block)
                headers[targets[target]] = true;
        }
    }
    return true;
}

bool anvil_mir_needs_frame_record(const anvil_mir_func_t *func)
{
    if (!func)
        return true;
    for (size_t index = 0; index < func->num_instrs; index++) {
        switch (func->instrs[index].op) {
        case ANVIL_MIR_OP_CALL:
        case ANVIL_MIR_OP_DYN_ALLOCA:
        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        case ANVIL_MIR_OP_VA_START:
            return true;
        default:
            break;
        }
    }
    return false;
}

bool anvil_mir_set_block_cold(anvil_mir_func_t *func, anvil_mir_block_t block, bool cold)
{
    if (!mir_valid_block(func, block)) return false;
    func->blocks[block].cold = cold;
    return true;
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
    memset(instr, 0, sizeof(*instr));
    instr->op = op;
    instr->def = def;
    instr->uses = uses_copy;
    instr->num_uses = num_uses;
    instr->block = func->current_block;
    instr->true_block = true_block;
    instr->false_block = false_block;
    instr->has_imm = has_imm;
    instr->imm = imm;
    instr->call_cc = ANVIL_CC_DEFAULT;
    instr->memory_access = (anvil_memory_access_t){ 0 };
    instr->call_effects = op == ANVIL_MIR_OP_CALL ? ANVIL_EFFECT_ALL : 0;
    memset(instr->clobbers, 0, sizeof(instr->clobbers));
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
    if (op == ANVIL_MIR_OP_CALL) return false;
    return add_instr_full(func, op, def, uses, num_uses, false, 0, NULL,
                          ANVIL_MIR_NO_BLOCK, ANVIL_MIR_NO_BLOCK);
}

bool anvil_mir_add_instr_imm(anvil_mir_func_t *func, anvil_mir_opcode_t op,
                             anvil_mir_vreg_t def, int64_t imm)
{
    if (op == ANVIL_MIR_OP_CALL) return false;
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
    if (op == ANVIL_MIR_OP_CALL) return false;
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
    if (op == ANVIL_MIR_OP_CALL) return false;
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
    if (op == ANVIL_MIR_OP_CALL) return false;
    return add_instr_full(func, op, def, uses, num_uses, true, imm,
                          symbol, ANVIL_MIR_NO_BLOCK,
                          ANVIL_MIR_NO_BLOCK);
}

bool anvil_mir_add_call(anvil_mir_func_t *func,
                        anvil_mir_vreg_t def,
                        const anvil_mir_vreg_t *uses,
                        size_t num_uses,
                        const char *symbol,
                        anvil_cc_t call_cc,
                        bool has_abi_imm,
                        int64_t abi_imm)
{
    if (call_cc <= ANVIL_CC_DEFAULT || call_cc > ANVIL_CC_MVS) return false;
    if (!add_instr_full(func, ANVIL_MIR_OP_CALL, def, uses, num_uses,
                        has_abi_imm, abi_imm, symbol,
                        ANVIL_MIR_NO_BLOCK, ANVIL_MIR_NO_BLOCK)) {
        return false;
    }
    func->instrs[func->num_instrs - 1].call_cc = call_cc;
    return true;
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
                             size_t size_bytes,
                             uint32_t align_bytes)
{
    if (!func || size_bytes == 0 || size_bytes > (size_t)INT_MAX || align_bytes == 0 ||
        (align_bytes & (align_bytes - 1)) != 0) return -1;
    if (func->num_frame_slots > (size_t)INT_MAX) return -1;
    if (!reserve_frame_slots(func, func->num_frame_slots + 1)) return -1;

    int slot = (int)func->num_frame_slots++;
    func->frame_slots[slot].size_bytes = size_bytes;
    func->frame_slots[slot].align_bytes = align_bytes;
    return slot;
}

bool anvil_mir_layout_frame_slots(const anvil_mir_func_t *func,
                                  int start,
                                  uint32_t default_align,
                                  uint32_t max_align,
                                  bool record_end,
                                  int *offsets,
                                  int *end)
{
    if (!func || start < 0 || !end || (func->num_frame_slots && !offsets))
        return false;
    int64_t offset = start;
    for (size_t i = 0; i < func->num_frame_slots; i++) {
        const anvil_mir_frame_slot_info_t *slot = &func->frame_slots[i];
        uint32_t align = slot->align_bytes ? slot->align_bytes : default_align;
        if (align == 0)
            align = 1;
        if (align > max_align)
            return false;
        offset = (offset + align - 1) & ~(int64_t)(align - 1);
        int64_t slot_end = offset + (int64_t)slot->size_bytes;
        if (slot_end > INT_MAX)
            return false;
        offsets[i] = (int)(record_end ? slot_end : offset);
        offset = slot_end;
    }
    *end = (int)offset;
    return true;
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
    out_info->index_scale = instr->index_scale;
    out_info->symbol_got = instr->symbol_got;
    out_info->fcmp_pred = instr->fcmp_pred;
    out_info->widening = instr->widening;
    out_info->call_cc = instr->call_cc;
    out_info->memory_access = instr->memory_access;
    out_info->ir_op = instr->ir_op;
    out_info->atomic = instr->atomic;
    out_info->named_gpr = instr->named_gpr;
    out_info->named_fpr = instr->named_fpr;
    out_info->named_stack_bytes = instr->named_stack_bytes;
    out_info->call_effects = instr->call_effects;
    memcpy(out_info->clobbers, instr->clobbers, sizeof(out_info->clobbers));
    memcpy(out_info->scratch, instr->scratch, sizeof(out_info->scratch));
    out_info->symbol = instr->symbol;
    out_info->spill_slot = instr->spill_slot;
    out_info->frame_slot = instr->frame_slot;
    return true;
}

bool anvil_mir_add_va_start(anvil_mir_func_t *func, anvil_mir_vreg_t def, int frame_slot, unsigned named_gpr, unsigned named_fpr, size_t named_stack_bytes)
{
    if (!func || frame_slot < 0 || (size_t)frame_slot >= func->num_frame_slots ||
        !anvil_mir_add_instr_imm(func, ANVIL_MIR_OP_VA_START, def, 0))
        return false;

    anvil_mir_instr_t *instruction = &func->instrs[func->num_instrs - 1];
    instruction->frame_slot = frame_slot;
    instruction->named_gpr = named_gpr;
    instruction->named_fpr = named_fpr;
    instruction->named_stack_bytes = named_stack_bytes;
    return true;
}

bool anvil_mir_add_atomic(anvil_mir_func_t *func, anvil_op_t operation, anvil_mir_vreg_t def,
                          const anvil_mir_vreg_t *uses, size_t count, const anvil_atomic_info_t *info)
{
    if (!anvil_atomic_info_valid(operation, info) || !anvil_mir_add_instr(func, ANVIL_MIR_OP_ATOMIC, def, uses, count))
        return false;

    anvil_mir_instr_t *instr = &func->instrs[func->num_instrs - 1];
    instr->ir_op = operation;
    instr->atomic = *info;
    return true;
}

static bool add_ir_operation(anvil_mir_func_t *func, anvil_mir_opcode_t op, anvil_op_t operation, anvil_mir_vreg_t def,
                             const anvil_mir_vreg_t *uses, size_t count, unsigned bits)
{
    if (!anvil_mir_add_instr_imm_uses(func, op, def, uses, count, (int64_t)bits))
        return false;
    func->instrs[func->num_instrs - 1].ir_op = operation;
    return true;
}

bool anvil_mir_add_vector(anvil_mir_func_t *func, anvil_op_t operation, anvil_mir_vreg_t def, const anvil_mir_vreg_t *uses,
                          size_t count, unsigned lane_bits)
{
    return add_ir_operation(func, ANVIL_MIR_OP_VECTOR, operation, def, uses, count, lane_bits);
}

bool anvil_mir_add_scalar(anvil_mir_func_t *func, anvil_op_t operation, anvil_mir_vreg_t def, const anvil_mir_vreg_t *uses,
                          size_t count, unsigned bits)
{
    return add_ir_operation(func, ANVIL_MIR_OP_SCALAR, operation, def, uses, count, bits);
}

bool anvil_mir_annotate_call_effects(anvil_mir_func_t *func, size_t first, unsigned effects)
{
    if (!func || first > func->num_instrs || (effects & ~ANVIL_EFFECT_ALL))
        return false;

    for (size_t index = first; index < func->num_instrs; index++)
    {
        if (func->instrs[index].op == ANVIL_MIR_OP_CALL)
            func->instrs[index].call_effects = effects;
    }

    return true;
}

bool anvil_mir_set_instr_clobbers(anvil_mir_func_t *func, size_t index, anvil_mir_reg_class_t reg_class, uint64_t mask)
{
    if (!func || index >= func->num_instrs || (unsigned)reg_class >= ANVIL_MIR_REG_CLASS_COUNT || func->assignments)
        return false;

    func->instrs[index].clobbers[reg_class] = mask;
    return true;
}

bool anvil_mir_set_instr_scratch(anvil_mir_func_t *func, size_t index, anvil_mir_reg_class_t reg_class, uint64_t mask)
{
    if (!func || index >= func->num_instrs || (unsigned)reg_class >= ANVIL_MIR_REG_CLASS_COUNT || func->assignments)
        return false;

    func->instrs[index].scratch[reg_class] = mask;
    return true;
}

bool anvil_mir_annotate_memory(anvil_mir_func_t *func, size_t first, const anvil_memory_access_t *access)
{
    if (!func || !access || first > func->num_instrs)
        return false;

    size_t alignment = access->alignment;
    if (alignment && (alignment & (alignment - 1)) != 0)
        return false;

    if (!alignment && !access->is_volatile)
        return true;

    for (size_t index = first; index < func->num_instrs; index++)
    {
        anvil_mir_instr_t *instr = &func->instrs[index];
        if (instr->op != ANVIL_MIR_OP_LOAD && instr->op != ANVIL_MIR_OP_STORE)
            continue;

        instr->memory_access = *access;
        if (instr->op == ANVIL_MIR_OP_STORE && instr->num_uses == 0)
            return false;

        anvil_mir_vreg_t value = instr->op == ANVIL_MIR_OP_LOAD ? instr->def : instr->uses[0];
        if (!anvil_mir_valid_vreg(func, value))
            return false;

        size_t width = (func->vregs[value].size_bits + 7u) / 8u;
        while (instr->memory_access.alignment > width)
            instr->memory_access.alignment /= 2;
    }

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

static bool instr_has_clobbers(const anvil_mir_instr_t *instr)
{
    for (size_t reg_class = 0; reg_class < ANVIL_MIR_REG_CLASS_COUNT; reg_class++)
    {
        if (instr->clobbers[reg_class])
            return true;
    }

    return false;
}

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

static void remove_instr_at(anvil_mir_func_t *func, size_t index);

/* For the COPY at `copy` (x = COPY t, t defined once): when t's definition
 * is earlier in the same block, every use of t lies between it and the copy,
 * and x is neither read nor written strictly between them, the definition
 * writes x instead and the copy goes. */
static bool mir_retarget_copy_source(anvil_mir_func_t *func, size_t copy)
{
    const anvil_mir_instr_t *move = &func->instrs[copy];
    anvil_mir_vreg_t dst = move->def, src = move->uses[0];
    anvil_mir_block_t block = move->block;
    size_t def = SIZE_MAX;
    for (size_t j = copy; j-- > 0;) {
        if (func->instrs[j].block != block)
            continue;
        if (func->instrs[j].def == src) {
            def = j;
            break;
        }
    }
    if (def == SIZE_MAX || func->vregs[src].is_live_in || instr_has_clobbers(&func->instrs[def]))
        return false;

    /* t is read nowhere outside (def, copy]. */
    for (size_t j = 0; j < func->num_instrs; j++) {
        const anvil_mir_instr_t *other = &func->instrs[j];
        bool inside = other->block == block && j > def && j <= copy;
        for (size_t u = 0; u < other->num_uses && !inside; u++)
            if (other->uses[u] == src)
                return false;
    }
    /* x keeps its old value until the copy: nothing in between touches it. */
    for (size_t j = def + 1; j < copy; j++) {
        const anvil_mir_instr_t *other = &func->instrs[j];
        if (other->block != block)
            continue;
        if (other->def == dst)
            return false;
        for (size_t u = 0; u < other->num_uses; u++)
            if (other->uses[u] == dst)
                return false;
    }

    func->instrs[def].def = dst;
    for (size_t j = def + 1; j < copy; j++) {
        anvil_mir_instr_t *other = &func->instrs[j];
        if (other->block != block)
            continue;
        for (size_t u = 0; u < other->num_uses; u++)
            if (other->uses[u] == src)
                other->uses[u] = dst;
    }
    remove_instr_at(func, copy);
    return true;
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
        if (instr_has_clobbers(instr))
            continue;

        anvil_mir_vreg_t dst = instr->def;
        anvil_mir_vreg_t src = instr->uses[0];
        if (!mir_vregs_coalescible(func, dst, src)) continue;

        /* `t = op ...; x = COPY t` with x defined elsewhere too (a PHI move):
         * op may write x directly when t lives only from op to the copy and
         * nothing in between reads or writes x. */
        if (def_counts[src] == 1 && def_counts[dst] > 1 && mir_retarget_copy_source(func, i)) {
            def_counts[src] = 0;
            i--;
            changed = true;
            continue;
        }
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

static bool mir_verify_index_scale(const anvil_mir_instr_t *instr, size_t index, char *error, size_t error_len)
{
    uint8_t scale = instr->index_scale;
    if (scale == 0 || scale == 1 || scale == 2 || scale == 4 || scale == 8)
        return true;
    return anvil_format_failure(error, error_len, "MachineIR memory instruction %zu has index scale %u", index, scale);
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
    return anvil_format_failure(error, error_len,
                           "instruction %zu has %zu uses, expected %zu",
                           index, instr->num_uses, expected);
}

static bool mir_verify_has_def(const anvil_mir_instr_t *instr,
                               size_t index,
                               char *error,
                               size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG) return true;
    return anvil_format_failure(error, error_len,
                           "instruction %zu requires a definition", index);
}

static bool mir_verify_no_def(const anvil_mir_instr_t *instr,
                              size_t index,
                              char *error,
                              size_t error_len)
{
    if (instr->def == ANVIL_MIR_NO_VREG) return true;
    return anvil_format_failure(error, error_len,
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
    anvil_format_failure(error, error_len,
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
    return anvil_format_failure(error, error_len,
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
    return anvil_format_failure(error, error_len,
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
            return anvil_format_failure(error, error_len,
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
            return anvil_format_failure(error, error_len,
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
    return valid || anvil_format_failure(error, error_len,
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
        return anvil_format_failure(error, error_len,
                               "out of memory verifying MachineIR blocks");
    }
    for (size_t i = 0; i < func->num_instrs; i++) {
        anvil_mir_block_t block = func->instrs[i].block;
        if (saw_terminator[block]) {
            const char *name = func->blocks[block].name
                ? func->blocks[block].name
                : "";
            free(saw_instr); free(saw_terminator);
            return anvil_format_failure(error, error_len,
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
        return anvil_format_failure(error, error_len,
                               "block %s is missing a terminator", name);
    }
    free(saw_instr); free(saw_terminator);
    return true;
}

static bool mir_verify_atomic(const anvil_mir_func_t *func, const anvil_mir_instr_t *instr, size_t index, char *error, size_t error_len)
{
    if (!anvil_atomic_info_valid(instr->ir_op, &instr->atomic) || instr->has_imm || instr->symbol)
        return anvil_format_failure(error, error_len, "instruction %zu has invalid atomic metadata", index);

    size_t count = 0;
    switch (instr->ir_op)
    {
        case ANVIL_OP_ATOMIC_LOAD:
            count = 1;
            break;
        case ANVIL_OP_ATOMIC_STORE:
        case ANVIL_OP_ATOMIC_RMW:
            count = 2;
            break;
        case ANVIL_OP_ATOMIC_CMPXCHG:
            count = 3;
            break;
        default:
            break;
    }

    bool has_result = instr->ir_op != ANVIL_OP_ATOMIC_STORE && instr->ir_op != ANVIL_OP_ATOMIC_FENCE;
    if (instr->num_uses != count || (instr->def != ANVIL_MIR_NO_VREG) != has_result)
        return anvil_format_failure(error, error_len, "instruction %zu has invalid atomic operands", index);

    if (!count)
        return true;

    const anvil_mir_vreg_info_t *pointer = &func->vregs[instr->uses[0]];
    if (pointer->reg_class != ANVIL_MIR_REG_GPR || (pointer->size_bits != 32 && pointer->size_bits != 64))
        return anvil_format_failure(error, error_len, "instruction %zu has invalid atomic address", index);

    const anvil_mir_vreg_info_t *value = &func->vregs[has_result ? instr->def : instr->uses[1]];
    if (value->reg_class != ANVIL_MIR_REG_GPR ||
        (value->size_bits != 8 && value->size_bits != 16 && value->size_bits != 32 && value->size_bits != 64))
        return anvil_format_failure(error, error_len, "instruction %zu has invalid atomic value width", index);

    for (size_t operand = 1; operand < count; operand++)
    {
        const anvil_mir_vreg_info_t *input = &func->vregs[instr->uses[operand]];
        if (input->reg_class != value->reg_class || input->size_bits != value->size_bits)
            return anvil_format_failure(error, error_len, "instruction %zu has mismatched atomic value widths", index);
    }

    return true;
}

static bool mir_verify_instr(const anvil_mir_func_t *func,
                             const anvil_mir_instr_t *instr,
                             size_t index,
                             char *error,
                             size_t error_len)
{
    size_t alignment = instr->memory_access.alignment;
    if ((instr->call_effects & ~ANVIL_EFFECT_ALL) || (instr->call_effects && instr->op != ANVIL_MIR_OP_CALL))
        return anvil_format_failure(error, error_len, "instruction %zu has invalid call effects", index);

    if ((alignment || instr->memory_access.is_volatile) &&
        instr->op != ANVIL_MIR_OP_LOAD && instr->op != ANVIL_MIR_OP_STORE)
    {
        return anvil_format_failure(error, error_len, "instruction %zu has memory attributes on a non-memory operation", index);
    }

    if (alignment && (alignment & (alignment - 1)) != 0)
        return anvil_format_failure(error, error_len, "instruction %zu has invalid memory alignment", index);

    if (!mir_valid_block(func, instr->block)) {
        return anvil_format_failure(error, error_len,
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

        case ANVIL_MIR_OP_VECTOR: {
            /* A 128-bit def; every use is a vector of the def's class, or
             * a scalar (broadcast value or uniform count).  Which forms an
             * operation accepts is the target's legality. */
            bool reduction = anvil_op_is_reduction(instr->ir_op);
            if (!mir_verify_has_def(instr, index, error, error_len) || !instr->has_imm ||
                (instr->imm != 8 && instr->imm != 16 && instr->imm != 32 && instr->imm != 64) || instr->num_uses == 0 ||
                instr->num_uses > 3 || (reduction ? func->vregs[instr->def].size_bits != instr->imm : func->vregs[instr->def].size_bits != 128))
                return anvil_format_failure(error, error_len, "vector instruction %zu needs a 128-bit def and 8-64 bit lanes", index);
            for (size_t u = 0; u < instr->num_uses; u++) {
                const anvil_mir_vreg_info_t *use = mir_verify_vreg_info(func, instr->uses[u], index, error, error_len);
                if (!use)
                    return false;
                if (use->size_bits == 128 && !reduction && use->reg_class != func->vregs[instr->def].reg_class)
                    return anvil_format_failure(error, error_len, "vector instruction %zu mixes register classes", index);
            }
            return true;
        }

        case ANVIL_MIR_OP_SCALAR:
            if (!mir_verify_has_def(instr, index, error, error_len) || !instr->has_imm || instr->num_uses == 0 || instr->num_uses > 3 ||
                func->vregs[instr->def].size_bits != instr->imm)
                return anvil_format_failure(error, error_len, "scalar instruction %zu needs 1-3 uses and its width as imm", index);
            /* A rotate's count is any general register, as a shift's. */
            if (instr->ir_op == ANVIL_OP_ROTL) {
                const anvil_mir_vreg_info_t *value = mir_verify_vreg_info(func, instr->uses[0], index, error, error_len);
                const anvil_mir_vreg_info_t *count = instr->num_uses == 2 ? mir_verify_vreg_info(func, instr->uses[1], index, error, error_len) : NULL;
                if (value && count && value->reg_class == ANVIL_MIR_REG_GPR && count->reg_class == ANVIL_MIR_REG_GPR &&
                    func->vregs[instr->def].reg_class == ANVIL_MIR_REG_GPR && value->size_bits == instr->imm)
                    return true;
                return anvil_format_failure(error, error_len, "rotate %zu needs a value of its width and a general count", index);
            }
            return mir_verify_same_class_uses(func, instr, index, true, error, error_len);

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
                   anvil_format_failure(error, error_len,
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
                   anvil_format_failure(error, error_len,
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
                    return anvil_format_failure(error, error_len,
                        "instruction %zu select condition must be GPR8", index);
                if (def->reg_class == then_info->reg_class &&
                    def->reg_class == else_info->reg_class) {
                    return true;
                }
                return anvil_format_failure(error, error_len,
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
                   mir_verify_index_scale(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, instr->index_scale ? 2 : 1, index, error, error_len) &&
                   mir_verify_use_class(func, instr, index, 0,
                                        ANVIL_MIR_REG_GPR, error, error_len) &&
                   (!instr->index_scale || mir_verify_use_class(func, instr, index, 1, ANVIL_MIR_REG_GPR, error, error_len));

        case ANVIL_MIR_OP_STORE:
            return mir_verify_no_def(instr, index, error, error_len) &&
                   mir_verify_index_scale(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, instr->index_scale ? 3 : 2, index, error, error_len) &&
                   mir_verify_use_class(func, instr, index, 1,
                                        ANVIL_MIR_REG_GPR, error, error_len) &&
                   (!instr->index_scale || mir_verify_use_class(func, instr, index, 2, ANVIL_MIR_REG_GPR, error, error_len));

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

        case ANVIL_MIR_OP_ATOMIC:
            return mir_verify_atomic(func, instr, index, error, error_len);

        case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        case ANVIL_MIR_OP_VA_START:
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 0, index, error, error_len) &&
                   instr->has_imm && instr->imm >= 0;

        case ANVIL_MIR_OP_CALL:
            if (instr->call_cc <= ANVIL_CC_DEFAULT ||
                instr->call_cc > ANVIL_CC_MVS) {
                return anvil_format_failure(error, error_len,
                    "instruction %zu has an invalid calling convention",
                    index);
            }
            /* The immediate is an ABI-specific payload checked by each
               target's legalizer. */
            if (instr->has_imm && instr->imm < 0) {
                return anvil_format_failure(error, error_len,
                    "instruction %zu has a negative call ABI payload",
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
                   anvil_format_failure(error, error_len,
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
            /* Where the value comes from (a fixed result register, or a
               location the emitter reads) is the target legalizer's
               contract. */
            return mir_verify_has_def(instr, index, error, error_len) &&
                   mir_verify_num_uses(instr, 0, index, error, error_len) &&
                   def && bundle_start > 0 &&
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
            return anvil_format_failure(error, error_len,
                                   "instruction %zu has invalid opcode", index);
    }
}

static bool mir_verify_cfg_and_assignment(const anvil_mir_func_t *func,
                                          char *error, size_t error_len)
{
    size_t blocks = func->num_blocks;
    size_t vregs = func->num_vregs;
    if (blocks == 0 || vregs > SIZE_MAX - 63) {
        return anvil_format_failure(error, error_len, "MachineIR CFG is too large");
    }
    size_t words = vregs == 0 ? 0 : (vregs + 63) / 64;
    if (blocks > SIZE_MAX / (2 * sizeof(anvil_mir_block_t)) ||
        blocks > SIZE_MAX / (2 * sizeof(size_t)) ||
        blocks > SIZE_MAX / sizeof(size_t) ||
        blocks == SIZE_MAX ||
        blocks + 1 > SIZE_MAX / sizeof(size_t) ||
        (words && blocks > (SIZE_MAX / sizeof(uint64_t)) / words)) {
        return anvil_format_failure(error, error_len, "MachineIR CFG is too large");
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
        return anvil_format_failure(error, error_len, "out of memory verifying MachineIR CFG");
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
            return anvil_format_failure(error, error_len, "MachineIR CFG is too large");
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
        return anvil_format_failure(error, error_len, "out of memory verifying MachineIR CFG");
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
            return anvil_format_failure(error, error_len, "block %s is unreachable",
                func->blocks[b].name ? func->blocks[b].name : "");
        }
        if (!seen_block[b]) {
            free(succ); free(succ_count); free(pred_count); free(pred_offset);
            free(last_instr); free(reachable); free(seen_block); free(queue);
            free(pred); free(pred_cursor);
            return anvil_format_failure(error, error_len, "reachable block %s is empty",
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
        return anvil_format_failure(error, error_len,
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
        return anvil_format_failure(error, error_len,
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
                return anvil_format_failure(error, error_len,
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
        return anvil_format_failure(error, error_len, "MachineIR function is null");
    }
    if (func->num_blocks == 0) {
        return anvil_format_failure(error, error_len, "MachineIR has no blocks");
    }

    for (size_t i = 0; i < func->num_vregs; i++) {
        const anvil_mir_vreg_info_t *info = &func->vregs[i];
        if (!valid_reg_class(info->reg_class) || info->size_bits == 0) {
            return anvil_format_failure(error, error_len,
                                   "vreg %zu has invalid metadata", i);
        }
        if (info->has_fixed_reg && info->fixed_phys_reg < 0) {
            return anvil_format_failure(error, error_len,
                                   "vreg %zu has invalid fixed register", i);
        }
        if (info->is_live_in && !info->has_fixed_reg) {
            return anvil_format_failure(error, error_len,
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
    return mir_verify_cfg_and_assignment(func, error, error_len) && anvil_mir_verify_clobbers(func, error, error_len);
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

/* Allocated values that sit in a scratch register (normally none: pools
 * keep scratch registers out of allocation). Ranges follow CFG liveness,
 * so a value carried around a loop occupies its register for the loop. */
typedef struct {
    anvil_mir_vreg_t vreg;
    anvil_mir_reg_class_t reg_class;
    int phys_reg;   /* -1: no longer there (spilled to make room) */
    size_t first;
    size_t last;
} scratch_occupant_t;

typedef struct {
    scratch_occupant_t *items;
    size_t count;
} scratch_occupancy_t;

static bool config_has_phys(const anvil_regalloc_class_config_t *config, int phys_reg)
{
    for (size_t index = 0; config && index < (size_t)(config->num_phys_regs > 0 ? config->num_phys_regs : 0); index++) {
        if (config_phys_reg_at(config, index) == phys_reg)
            return true;
    }
    return false;
}

static bool build_scratch_occupancy(const anvil_mir_func_t *func, const anvil_regalloc_class_config_t *configs,
                                    size_t num_configs, scratch_occupancy_t *out)
{
    out->items = NULL;
    out->count = 0;
    size_t *slot = malloc((func->num_vregs ? func->num_vregs : 1) * sizeof(*slot));
    if (!slot)
        return false;

    size_t count = 0;
    for (size_t value = 0; value < func->num_vregs; value++) {
        const anvil_regalloc_assignment_t *assignment = &func->assignments[value];
        slot[value] = SIZE_MAX;
        if (!assignment->spilled && assignment->phys_reg >= 0 &&
            config_has_phys(spill_config_for_class(configs, num_configs, assignment->reg_class), assignment->phys_reg))
            slot[value] = count++;
    }

    if (!count) {
        free(slot);
        return true;
    }

    out->items = calloc(count, sizeof(*out->items));
    if (!out->items) {
        free(slot);
        return false;
    }

    for (size_t value = 0; value < func->num_vregs; value++) {
        if (slot[value] == SIZE_MAX)
            continue;
        scratch_occupant_t *item = &out->items[slot[value]];
        item->vreg = (anvil_mir_vreg_t)value;
        item->reg_class = func->assignments[value].reg_class;
        item->phys_reg = func->assignments[value].phys_reg;
        item->first = SIZE_MAX;
        item->last = 0;
    }

#define NOTE_OCCUPANT(value, index)                                         \
    do {                                                                     \
        if (slot[value] != SIZE_MAX) {                                       \
            scratch_occupant_t *item = &out->items[slot[value]];             \
            if ((index) < item->first) item->first = (index);                \
            if ((index) > item->last) item->last = (index);                  \
        }                                                                    \
    } while (0)

    for (size_t index = 0; index < func->num_instrs; index++) {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        if (instr->def != ANVIL_MIR_NO_VREG)
            NOTE_OCCUPANT(instr->def, index);
        for (size_t u = 0; u < instr->num_uses; u++)
            NOTE_OCCUPANT(instr->uses[u], index);
    }

    anvil_mir_liveness_t liveness;
    bool ok = func->num_blocks == 0 || anvil_mir_compute_liveness(func, &liveness);
    for (size_t block = 0; ok && block < func->num_blocks; block++) {
        if (liveness.first_instr[block] == SIZE_MAX)
            continue;
        size_t row = block * liveness.words_per_block;
        for (size_t value = 0; value < func->num_vregs; value++) {
            uint64_t bit = UINT64_C(1) << (value % 64);
            if (slot[value] == SIZE_MAX)
                continue;
            if (liveness.live_in[row + value / 64] & bit)
                NOTE_OCCUPANT(value, liveness.first_instr[block]);
            if (liveness.live_out[row + value / 64] & bit)
                NOTE_OCCUPANT(value, liveness.last_instr[block]);
        }
    }
#undef NOTE_OCCUPANT

    if (ok && func->num_blocks)
        anvil_mir_liveness_destroy(&liveness);
    free(slot);
    if (!ok) {
        free(out->items);
        out->items = NULL;
        return false;
    }

    out->count = count;
    return true;
}

/* Whether an allocated value occupies `phys_reg` anywhere in instructions
 * first..last (a bundle: its reloads, loaded before it, must survive all of
 * it). */
static bool scratch_phys_live_in(const scratch_occupancy_t *occupancy, anvil_mir_reg_class_t reg_class, int phys_reg, size_t first,
                                 size_t last)
{
    for (size_t index = 0; index < occupancy->count; index++) {
        const scratch_occupant_t *item = &occupancy->items[index];
        if (item->reg_class == reg_class && item->phys_reg == phys_reg && item->first <= last && first <= item->last)
            return true;
    }
    return false;
}

static size_t first_matching_use(const anvil_mir_instr_t *instr, size_t index)
{
    for (size_t previous = 0; previous < index; previous++)
    {
        if (instr->uses[previous] == instr->uses[index])
            return previous;
    }

    return index;
}

static bool mir_bundle_continues(const anvil_mir_func_t *func, size_t index);

/* Whether a value of `size_bits` must sit in one of the class's narrow
 * registers (anvil_regalloc_class_config_t.narrow_bits), and whether
 * `phys` is one. */
static bool scratch_value_narrow(const anvil_regalloc_class_config_t *config, uint16_t size_bits)
{
    return config && config->narrow_bits && size_bits <= config->narrow_bits;
}

static bool scratch_phys_narrow(const anvil_regalloc_class_config_t *config, int phys)
{
    return config && config->narrow_bits && phys >= 0 && phys < 64 && (config->narrow_regs >> phys & 1);
}

/* Reload registers an instruction needs per class, and how many of them
 * must be narrow ones. */
static bool instruction_scratch_needs(const anvil_mir_func_t *func,
                                      const anvil_regalloc_class_config_t *configs,
                                      size_t num_configs,
                                      const anvil_mir_instr_t *instr,
                                      size_t needs[ANVIL_MIR_REG_CLASS_COUNT],
                                      size_t narrow_needs[ANVIL_MIR_REG_CLASS_COUNT])
{
    for (size_t i = 0; i < ANVIL_MIR_REG_CLASS_COUNT; i++) {
        needs[i] = 0;
        narrow_needs[i] = 0;
    }

    for (size_t u = 0; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = instr->uses[u];
        if (!assignment_is_spilled(func, use)) continue;
        if (first_matching_use(instr, u) != u)
            continue;

        anvil_mir_reg_class_t reg_class = func->vregs[use].reg_class;
        if (!valid_reg_class(reg_class)) return false;
        needs[reg_class]++;
        if (scratch_value_narrow(spill_config_for_class(configs, num_configs, reg_class), func->vregs[use].size_bits))
            narrow_needs[reg_class]++;
    }

    /* A spilled result reuses the register of a reload of its class (a
     * narrow reload for a narrow result). */
    if (assignment_is_spilled(func, instr->def)) {
        anvil_mir_reg_class_t reg_class = func->vregs[instr->def].reg_class;
        if (!valid_reg_class(reg_class)) return false;
        if (scratch_value_narrow(spill_config_for_class(configs, num_configs, reg_class), func->vregs[instr->def].size_bits)) {
            if (!narrow_needs[reg_class]) {
                needs[reg_class]++;
                narrow_needs[reg_class]++;
            }
        } else if (!needs[reg_class]) {
            needs[reg_class]++;
        }
    }

    return true;
}

/* The registers reloads and result temporaries of the bundle starting at
 * `index` must avoid: every member's printer scratch (reloads come before
 * the bundle) and, in a bundle of several, every member's allocated result
 * (written together, before the spill stores that follow the bundle). */
static void bundle_scratch(const anvil_mir_func_t *func, size_t index, uint64_t scratch[ANVIL_MIR_REG_CLASS_COUNT])
{
    memset(scratch, 0, ANVIL_MIR_REG_CLASS_COUNT * sizeof(*scratch));
    bool several = mir_bundle_continues(func, index);
    for (;; index++) {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++)
            scratch[c] |= instr->scratch[c];
        if (several && instr->def != ANVIL_MIR_NO_VREG && !func->assignments[instr->def].spilled &&
            func->assignments[instr->def].phys_reg >= 0 && func->assignments[instr->def].phys_reg < 64)
            scratch[func->vregs[instr->def].reg_class] |= UINT64_C(1) << func->assignments[instr->def].phys_reg;
        if (!mir_bundle_continues(func, index))
            break;
    }
}

/* Called for a bundle (instructions start..end) and class whose reloads
 * lack `missing` scratch registers, `narrow_missing` of them narrow. */
typedef void (*scratch_shortage_fn)(void *ctx, size_t start, size_t end, anvil_mir_reg_class_t reg_class, size_t missing,
                                    size_t narrow_missing);

/* Every bundle's reloads and spill stores must fit in the scratch registers
 * free at it; checked up front so materialization cannot fail halfway.
 * Wide values take narrow registers last (next_scratch_phys), so the counts
 * suffice in any order.  Reports each shortage to `short_of` when given
 * (else stops at the first); true when there is none. */
static bool scratch_configs_cover_spills(
    const anvil_mir_func_t *func,
    const anvil_regalloc_class_config_t *scratch_configs,
    size_t num_scratch_configs,
    const scratch_occupancy_t *occupancy,
    scratch_shortage_fn short_of,
    void *ctx)
{
    size_t needs[ANVIL_MIR_REG_CLASS_COUNT] = { 0 };
    size_t narrow_needs[ANVIL_MIR_REG_CLASS_COUNT] = { 0 };
    uint64_t excluded[ANVIL_MIR_REG_CLASS_COUNT] = { 0 };
    size_t start = 0;
    bool fits = true;
    for (size_t i = 0; i < func->num_instrs; i++) {
        size_t own[ANVIL_MIR_REG_CLASS_COUNT], own_narrow[ANVIL_MIR_REG_CLASS_COUNT];
        if (!instruction_scratch_needs(func, scratch_configs, num_scratch_configs, &func->instrs[i], own, own_narrow)) {
            return false;
        }
        if (i == start)
            bundle_scratch(func, start, excluded);
        for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++) {
            needs[c] += own[c];
            narrow_needs[c] += own_narrow[c];
        }
        if (mir_bundle_continues(func, i))
            continue;

        for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++) {
            if (needs[c] == 0) continue;

            const anvil_regalloc_class_config_t *config =
                spill_config_for_class(scratch_configs, num_scratch_configs,
                                       (anvil_mir_reg_class_t)c);
            if (!config || config->num_phys_regs <= 0)
                return false;

            size_t available = 0, narrow_available = 0;
            for (size_t r = 0; r < (size_t)config->num_phys_regs; r++) {
                int candidate = config_phys_reg_at(config, r);
                if (candidate >= 0 && candidate < 64 && !(excluded[c] >> candidate & 1) &&
                    !scratch_phys_live_in(occupancy, (anvil_mir_reg_class_t)c, candidate, start, i)) {
                    available++;
                    narrow_available += scratch_phys_narrow(config, candidate);
                }
            }
            if (needs[c] > available || narrow_needs[c] > narrow_available) {
                fits = false;
                if (!short_of)
                    return false;
                short_of(ctx, start, i, (anvil_mir_reg_class_t)c, needs[c] > available ? needs[c] - available : 0,
                         narrow_needs[c] > narrow_available ? narrow_needs[c] - narrow_available : 0);
            }
        }
        memset(needs, 0, sizeof(needs));
        memset(narrow_needs, 0, sizeof(narrow_needs));
        start = i + 1;
    }

    return fits;
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
                              int spill_slot,
                              const anvil_mir_instr_t *rematerialize)
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
    if (rematerialize)
    {
        /* Recompute the value: its definition again, into the temp. */
        if (!clone_instr_for_materialization(rematerialize, &instr))
            return false;
        instr.def = temp;
        instr.block = block;
    }

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

/* A free scratch register for a value of `size_bits`: a narrow value
 * takes a narrow register, any other value a narrow register only when no
 * other is left. */
static bool next_scratch_phys(const anvil_regalloc_class_config_t *configs,
                              size_t num_configs,
                              anvil_mir_reg_class_t reg_class,
                              uint16_t size_bits,
                              uint64_t taken[ANVIL_MIR_REG_CLASS_COUNT],
                              const scratch_occupancy_t *occupancy,
                              const uint64_t excluded[ANVIL_MIR_REG_CLASS_COUNT],
                              size_t bundle_start,
                              size_t instr_index,
                              int *out_phys)
{
    const anvil_regalloc_class_config_t *config =
        spill_config_for_class(configs, num_configs, reg_class);
    if (!config || config->num_phys_regs <= 0) return false;

    bool narrow = scratch_value_narrow(config, size_bits);
    for (int pass = narrow ? 1 : 0; pass < 2; pass++) {
        for (size_t index = 0; index < (size_t)config->num_phys_regs; index++) {
            int candidate = config_phys_reg_at(config, index);
            if (candidate < 0 || candidate >= 64 || ((taken[reg_class] | excluded[reg_class]) >> candidate & 1) ||
                (config->narrow_bits && scratch_phys_narrow(config, candidate) != (pass == 1)) ||
                scratch_phys_live_in(occupancy, reg_class, candidate, bundle_start, instr_index))
                continue;
            taken[reg_class] |= UINT64_C(1) << candidate;
            *out_phys = candidate;
            return true;
        }
        if (!config->narrow_bits)
            break;
    }
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

typedef struct {
    const anvil_mir_instr_t *instruction;
    unsigned definitions;
} rematerialization_t;

/* A definition that reads nothing and changes nothing (a constant, a
 * symbol's or a frame slot's address) can be recomputed wherever the value
 * would be reloaded. */
static bool rematerializable(const anvil_mir_func_t *func, const anvil_mir_instr_t *instr)
{
    if (instr->def == ANVIL_MIR_NO_VREG || instr->num_uses || instr_has_clobbers(instr))
        return false;
    switch (instr->op) {
    case ANVIL_MIR_OP_MOV:
        return instr->has_imm && !instr->symbol && func->vregs[instr->def].reg_class == ANVIL_MIR_REG_GPR;
    case ANVIL_MIR_OP_SYMBOL_ADDR:
    case ANVIL_MIR_OP_FRAME_ADDR:
        return true;
    default:
        return false;
    }
}

bool anvil_mir_instr_recomputable(const anvil_mir_func_t *func, size_t index)
{
    return func && index < func->num_instrs && rematerializable(func, &func->instrs[index]);
}

static rematerialization_t *find_rematerializable_values(const anvil_mir_func_t *func)
{
    rematerialization_t *values = calloc(func->num_vregs, sizeof(*values));
    if (!values)
        return NULL;

    for (size_t index = 0; index < func->num_instrs; index++)
    {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        if (instr->def == ANVIL_MIR_NO_VREG)
            continue;

        rematerialization_t *value = &values[instr->def];
        if (value->definitions < 2)
            value->definitions++;

        const anvil_mir_vreg_info_t *info = &func->vregs[instr->def];
        if (value->definitions == 1 && !info->is_live_in && rematerializable(func, instr))
            value->instruction = instr;
        else
            value->instruction = NULL;
    }

    return values;
}

typedef struct {
    anvil_mir_block_t block;
    size_t definitions;
    size_t definition;
    size_t first_use;
    size_t definition_epoch;
    size_t last_use_epoch;
    size_t current_epoch;
    anvil_mir_vreg_t current;
    int slot;
    bool crosses_blocks;
} split_value_t;

static void split_note_block(split_value_t *value, anvil_mir_block_t block)
{
    if (value->block == ANVIL_MIR_NO_BLOCK)
        value->block = block;
    else if (value->block != block)
        value->crosses_blocks = true;
}

/* Instructions that must stay adjacent: RET_VALUE_PART... RET and
 * CALL CALL_RESULT... Spill code goes before or after the whole bundle. */
static bool mir_bundle_continues(const anvil_mir_func_t *func, size_t index)
{
    const anvil_mir_instr_t *current = &func->instrs[index];
    const anvil_mir_instr_t *next = index + 1 < func->num_instrs ? &func->instrs[index + 1] : NULL;
    return next && next->block == current->block &&
           ((current->op == ANVIL_MIR_OP_RET_VALUE_PART &&
             (next->op == ANVIL_MIR_OP_RET_VALUE_PART || next->op == ANVIL_MIR_OP_RET)) ||
            ((current->op == ANVIL_MIR_OP_CALL || current->op == ANVIL_MIR_OP_CALL_RESULT) &&
             next->op == ANVIL_MIR_OP_CALL_RESULT));
}

static void split_advance_epochs(const anvil_mir_instr_t *instruction, size_t *epochs)
{
    for (size_t reg_class = 0; reg_class < ANVIL_MIR_REG_CLASS_COUNT; reg_class++)
    {
        if (instruction->clobbers[reg_class])
            epochs[(size_t)instruction->block * ANVIL_MIR_REG_CLASS_COUNT + reg_class]++;
    }
}

static bool mir_op_is_compare(anvil_mir_opcode_t op)
{
    switch (op) {
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
    case ANVIL_MIR_OP_CMP_UGE:
    case ANVIL_MIR_OP_FCMP:
        return true;
    default:
        return false;
    }
}

/* Whether a compare may move past `instr`: it neither redefines the
 * compare's operands or result nor is a call or an ABI bundle member. */
static bool compare_may_pass(const anvil_mir_instr_t *compare, const anvil_mir_instr_t *instr)
{
    if (instr->op == ANVIL_MIR_OP_CALL || instr->op == ANVIL_MIR_OP_CALL_RESULT || instr->op == ANVIL_MIR_OP_RET_VALUE_PART ||
        instr->op == ANVIL_MIR_OP_VA_START || instr->def == compare->def)
        return false;
    for (size_t reg_class = 0; reg_class < ANVIL_MIR_REG_CLASS_COUNT; reg_class++) {
        if (instr->clobbers[reg_class])
            return false;
    }
    for (size_t use = 0; use < compare->num_uses; use++) {
        if (instr->def == compare->uses[use])
            return false;
    }
    return true;
}

/* A compare whose only use is the condition of a later BR_COND or SELECT in
 * its block moves down to just before it (past what it may pass), so the
 * printer can branch or select on the flags instead of materializing the
 * condition. */
bool anvil_mir_sink_compares(anvil_mir_func_t *func)
{
    if (!func || !func->num_vregs || func->num_instrs < 2)
        return func != NULL;
    uint8_t *uses = calloc(func->num_vregs, 1), *defs = calloc(func->num_vregs, 1);
    if (!uses || !defs) {
        free(uses);
        free(defs);
        return false;
    }
    for (size_t index = 0; index < func->num_instrs; index++) {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        if (instr->def != ANVIL_MIR_NO_VREG && instr->def < func->num_vregs && defs[instr->def] < UINT8_MAX)
            defs[instr->def]++;
        for (size_t use = 0; use < instr->num_uses; use++) {
            if (instr->uses[use] < func->num_vregs && uses[instr->uses[use]] < UINT8_MAX)
                uses[instr->uses[use]]++;
        }
    }
    for (size_t index = 0; index + 1 < func->num_instrs; index++) {
        anvil_mir_instr_t compare = func->instrs[index];
        if (!mir_op_is_compare(compare.op) || compare.def == ANVIL_MIR_NO_VREG || compare.def >= func->num_vregs || defs[compare.def] != 1 ||
            uses[compare.def] != 1)
            continue;
        size_t consumer = index + 1;
        while (consumer < func->num_instrs && func->instrs[consumer].block == compare.block &&
               !(func->instrs[consumer].num_uses && func->instrs[consumer].uses[0] == compare.def &&
                 (func->instrs[consumer].op == ANVIL_MIR_OP_BR_COND || func->instrs[consumer].op == ANVIL_MIR_OP_SELECT)) &&
               compare_may_pass(&compare, &func->instrs[consumer]))
            consumer++;
        if (consumer == index + 1 || consumer >= func->num_instrs || func->instrs[consumer].block != compare.block ||
            !func->instrs[consumer].num_uses || func->instrs[consumer].uses[0] != compare.def ||
            (func->instrs[consumer].op != ANVIL_MIR_OP_BR_COND && func->instrs[consumer].op != ANVIL_MIR_OP_SELECT))
            continue;
        memmove(&func->instrs[index], &func->instrs[index + 1], (consumer - index - 1) * sizeof(*func->instrs));
        func->instrs[consumer - 1] = compare;
        index--;  /* the instruction now at `index` is unexamined */
    }
    free(uses);
    free(defs);
    return true;
}

/* `def = use` in `block`, for an instruction stream being rebuilt. */
static bool make_block_copy(anvil_mir_instr_t *instr, anvil_mir_block_t block, anvil_mir_vreg_t def, anvil_mir_vreg_t use)
{
    memset(instr, 0, sizeof(*instr));
    instr->uses = malloc(sizeof(*instr->uses));
    if (!instr->uses)
        return false;
    instr->op = ANVIL_MIR_OP_COPY;
    instr->def = def;
    instr->uses[0] = use;
    instr->num_uses = 1;
    instr->block = block;
    instr->true_block = ANVIL_MIR_NO_BLOCK;
    instr->false_block = ANVIL_MIR_NO_BLOCK;
    instr->spill_slot = -1;
    instr->frame_slot = -1;
    return true;
}

typedef struct {
    anvil_mir_block_t block;
    anvil_mir_vreg_t value;
    anvil_mir_vreg_t copy;
} cold_move_t;

/* A value live into a cold block, live after its last call and not
 * redefined there before it is copied into a fresh vreg at the block's
 * start and back after that call (and the results it defines).  Only the
 * copy crosses the calls, and it lives in the cold block alone, so where no
 * register survives a call the store and reload stay on the rare path while
 * the value keeps a caller-saved register everywhere else. */
bool anvil_mir_split_around_cold_calls(anvil_mir_func_t *func, const bool *split_class)
{
    bool any = false;
    for (size_t block = 0; block < func->num_blocks; block++)
        any |= func->blocks[block].cold;
    if (!any || !func->num_vregs || !func->num_instrs)
        return true;

    anvil_mir_liveness_t liveness;
    if (!anvil_mir_compute_liveness(func, &liveness))
        return false;
    size_t words = liveness.words_per_block, original_vregs = func->num_vregs;
    size_t *restore_after = malloc(func->num_blocks * sizeof(*restore_after));
    uint64_t *live = malloc(words * sizeof(*live));
    uint64_t *touched = malloc(words * sizeof(*touched));
    cold_move_t *moves = NULL;
    size_t num_moves = 0, cap_moves = 0;
    anvil_mir_instr_t *rebuilt = NULL;
    size_t count = 0;
    bool ok = restore_after && live && touched;
    for (size_t block = 0; ok && block < func->num_blocks; block++) {
        restore_after[block] = SIZE_MAX;
        size_t first = liveness.first_instr[block], last = liveness.last_instr[block], call = SIZE_MAX;
        if (!func->blocks[block].cold || first == SIZE_MAX)
            continue;
        for (size_t index = first; index <= last; index++) {
            if (func->instrs[index].block == block && func->instrs[index].op == ANVIL_MIR_OP_CALL)
                call = index;
        }
        if (call == SIZE_MAX)
            continue;
        size_t end = call;
        while (end + 1 <= last && func->instrs[end + 1].block == block && func->instrs[end + 1].op == ANVIL_MIR_OP_CALL_RESULT)
            end++;
        /* Live after `end`, and defined from the block's start through it
         * (uses there still read the value, which then dies before the
         * calls). */
        memcpy(live, &liveness.live_out[block * words], words * sizeof(*live));
        memset(touched, 0, words * sizeof(*touched));
        for (size_t index = last + 1; index-- > first;) {
            const anvil_mir_instr_t *instr = &func->instrs[index];
            if (instr->block != block)
                continue;
            if (index <= end) {
                if (instr->def != ANVIL_MIR_NO_VREG && instr->def < original_vregs)
                    touched[instr->def / 64] |= UINT64_C(1) << (instr->def % 64);
                continue;
            }
            if (instr->def != ANVIL_MIR_NO_VREG && instr->def < original_vregs)
                live[instr->def / 64] &= ~(UINT64_C(1) << (instr->def % 64));
            for (size_t use = 0; use < instr->num_uses; use++) {
                if (instr->uses[use] < original_vregs)
                    live[instr->uses[use] / 64] |= UINT64_C(1) << (instr->uses[use] % 64);
            }
        }
        for (size_t value = 0; ok && value < original_vregs; value++) {
            uint64_t bit = UINT64_C(1) << (value % 64);
            size_t cell = value / 64;
            if (!(liveness.live_in[block * words + cell] & bit) || !(live[cell] & bit) || (touched[cell] & bit) ||
                func->vregs[value].has_fixed_reg || !split_class[func->vregs[value].reg_class])
                continue;
            if (num_moves == cap_moves) {
                size_t grown_cap = cap_moves ? cap_moves * 2 : 8;
                cold_move_t *grown = realloc(moves, grown_cap * sizeof(*grown));
                if (!(ok = grown != NULL))
                    break;
                moves = grown;
                cap_moves = grown_cap;
            }
            const anvil_mir_vreg_info_t *info = &func->vregs[value];
            anvil_mir_vreg_t copy = anvil_mir_add_vreg_typed(func, info->reg_class, info->size_bits, info->is_signed);
            if (!(ok = copy != ANVIL_MIR_NO_VREG))
                break;
            moves[num_moves++] = (cold_move_t){.block = (anvil_mir_block_t)block, .value = (anvil_mir_vreg_t)value, .copy = copy};
            restore_after[block] = end;
        }
    }

    if (ok && num_moves) {
        rebuilt = calloc(func->num_instrs + 2 * num_moves, sizeof(*rebuilt));
        ok = rebuilt != NULL;
        for (size_t index = 0; ok && index < func->num_instrs; index++) {
            anvil_mir_block_t block = func->instrs[index].block;
            bool split = mir_valid_block(func, block) && restore_after[block] != SIZE_MAX;
            for (size_t move = 0; ok && split && index == liveness.first_instr[block] && move < num_moves; move++) {
                if (moves[move].block == block)
                    ok = make_block_copy(&rebuilt[count++], block, moves[move].copy, moves[move].value);
            }
            rebuilt[count++] = func->instrs[index];
            for (size_t move = 0; ok && split && index == restore_after[block] && move < num_moves; move++) {
                if (moves[move].block == block)
                    ok = make_block_copy(&rebuilt[count++], block, moves[move].value, moves[move].copy);
            }
        }
        if (ok) {
            free(func->instrs);
            func->instrs = rebuilt;
            func->num_instrs = count;
            func->cap_instrs = count;
            rebuilt = NULL;
            recompute_block_instr_ranges(func);
            anvil_mir_clear_allocations(func);
        } else if (rebuilt) {
            /* Only the copies are owned here; the originals stay in func. */
            for (size_t index = 0; index < count; index++) {
                if (rebuilt[index].op == ANVIL_MIR_OP_COPY && rebuilt[index].uses && (rebuilt[index].def >= original_vregs ||
                                                                                       rebuilt[index].uses[0] >= original_vregs))
                    free_instr_contents(&rebuilt[index]);
            }
        }
    }
    if (!ok)
        func->num_vregs = original_vregs;
    free(rebuilt);
    free(moves);
    free(live);
    free(touched);
    free(restore_after);
    anvil_mir_liveness_destroy(&liveness);
    return ok;
}

/* Split local, single-definition spilled values at clobbers. Each immutable
 * value is saved once, then reloaded only at the first use in a new segment.
 * Cross-block ranges and PHI copies retain the global allocator's policy.
 * Pinned slots keep these explicit transfers stable when allocation reruns. */
bool anvil_mir_split_spilled_intervals(anvil_mir_func_t *func, bool *changed)
{
    *changed = false;
    if (!func->num_spills || !func->num_vregs)
        return true;

    if (func->num_blocks > SIZE_MAX / (ANVIL_MIR_REG_CLASS_COUNT * sizeof(size_t)))
        return false;

    size_t original_vregs = func->num_vregs;
    split_value_t *values = calloc(original_vregs, sizeof(*values));
    size_t epoch_count = func->num_blocks * ANVIL_MIR_REG_CLASS_COUNT;
    size_t *epochs = calloc(epoch_count, sizeof(*epochs));
    if (!values || !epochs)
    {
        free(values);
        free(epochs);
        return false;
    }

    for (size_t index = 0; index < original_vregs; index++)
    {
        values[index].block = ANVIL_MIR_NO_BLOCK;
        values[index].first_use = SIZE_MAX;
        values[index].slot = -1;
        values[index].current = (anvil_mir_vreg_t)index;
    }

    for (size_t index = 0; index < func->num_instrs; index++)
    {
        const anvil_mir_instr_t *instruction = &func->instrs[index];
        for (size_t operand = 0; operand < instruction->num_uses; operand++)
        {
            anvil_mir_vreg_t use = instruction->uses[operand];
            split_value_t *value = &values[use];
            split_note_block(value, instruction->block);
            if (value->first_use == SIZE_MAX)
                value->first_use = index;

            value->last_use_epoch = epochs[(size_t)instruction->block * ANVIL_MIR_REG_CLASS_COUNT + func->vregs[use].reg_class];
        }

        split_advance_epochs(instruction, epochs);
        if (instruction->def != ANVIL_MIR_NO_VREG)
        {
            split_value_t *value = &values[instruction->def];
            split_note_block(value, instruction->block);
            value->definitions++;
            value->definition = index;
            value->definition_epoch = epochs[(size_t)instruction->block * ANVIL_MIR_REG_CLASS_COUNT + func->vregs[instruction->def].reg_class];
        }
    }

    size_t pinned = func->num_pinned_spills;
    for (size_t index = 0; index < original_vregs; index++)
    {
        split_value_t *value = &values[index];
        if (!assignment_is_spilled(func, (anvil_mir_vreg_t)index) || value->definitions != 1 || value->crosses_blocks ||
            value->first_use == SIZE_MAX || value->first_use <= value->definition || value->last_use_epoch <= value->definition_epoch ||
            func->vregs[index].has_fixed_reg || func->vregs[index].is_live_in)
            continue;

        const anvil_mir_instr_t *definition = &func->instrs[value->definition];
        if ((definition->op == ANVIL_MIR_OP_MOV && definition->has_imm) || definition->op == ANVIL_MIR_OP_CALL ||
            definition->op == ANVIL_MIR_OP_CALL_RESULT || definition->op == ANVIL_MIR_OP_SPILL_LOAD)
            continue;

        if (pinned == INT_MAX)
        {
            free(values);
            free(epochs);
            return false;
        }

        value->slot = (int)pinned++;
    }

    if (pinned == func->num_pinned_spills)
    {
        free(values);
        free(epochs);
        return true;
    }

    /* Bundle members are held so their reloads land before the bundle and
     * their spill stores after it (see mir_bundle_continues). */
    anvil_mir_instr_t *held = NULL;
    size_t num_held = 0, cap_held = 0;
    anvil_mir_instr_t *stores = NULL;
    size_t num_stores = 0, cap_stores = 0;
    anvil_mir_spill_slot_info_t *slots = calloc(pinned, sizeof(*slots));
    anvil_mir_instr_t *instructions = NULL;
    size_t count = 0;
    size_t capacity = 0;
    bool success = false;
    if (!slots)
        goto done;

    if (func->num_pinned_spills)
        memcpy(slots, func->spill_slots, func->num_pinned_spills * sizeof(*slots));

    for (size_t index = 0; index < original_vregs; index++)
    {
        if (values[index].slot >= 0)
        {
            slots[values[index].slot].reg_class = func->vregs[index].reg_class;
            slots[values[index].slot].size_bits = func->vregs[index].size_bits;
        }
    }

    memset(epochs, 0, epoch_count * sizeof(*epochs));
    for (size_t index = 0; index < func->num_instrs; index++)
    {
        const anvil_mir_instr_t *original = &func->instrs[index];
        anvil_mir_instr_t copy;
        if (!clone_instr_for_materialization(original, &copy))
            goto done;

        bool copied = true;
        for (size_t operand = 0; operand < copy.num_uses; operand++)
        {
            anvil_mir_vreg_t use = original->uses[operand];
            split_value_t *value = &values[use];
            if (value->slot < 0)
                continue;

            anvil_mir_vreg_info_t info = func->vregs[use];
            size_t epoch = epochs[(size_t)original->block * ANVIL_MIR_REG_CLASS_COUNT + info.reg_class];
            if (value->current_epoch != epoch)
            {
                if (func->num_vregs >= UINT32_MAX || !reserve_vregs(func, func->num_vregs + 1))
                {
                    copied = false;
                    break;
                }

                anvil_mir_vreg_t segment = (anvil_mir_vreg_t)func->num_vregs++;
                func->vregs[segment] = info;
                if (!append_spill_load(&instructions, &count, &capacity, original->block, segment, value->slot, NULL))
                {
                    copied = false;
                    break;
                }

                value->current = segment;
                value->current_epoch = epoch;
            }

            copy.uses[operand] = value->current;
        }

        if (!copied || !append_owned_materialized_instr(&held, &num_held, &cap_held, &copy))
        {
            free_instr_contents(&copy);
            goto done;
        }

        split_advance_epochs(original, epochs);
        if (original->def != ANVIL_MIR_NO_VREG && values[original->def].slot >= 0)
        {
            split_value_t *value = &values[original->def];
            value->current_epoch = epochs[(size_t)original->block * ANVIL_MIR_REG_CLASS_COUNT + func->vregs[original->def].reg_class];
            if (!append_spill_store(&stores, &num_stores, &cap_stores, original->block, original->def, value->slot))
                goto done;
        }

        if (mir_bundle_continues(func, index))
            continue;

        bool flushed = true;
        for (size_t member = 0; flushed && member < num_held; member++)
            flushed = append_owned_materialized_instr(&instructions, &count, &capacity, &held[member]);
        for (size_t member = 0; flushed && member < num_stores; member++)
            flushed = append_owned_materialized_instr(&instructions, &count, &capacity, &stores[member]);
        if (!flushed)
            goto done;
        num_held = 0;
        num_stores = 0;
    }

    free_instr_array(func->instrs, func->num_instrs);
    func->instrs = instructions;
    func->num_instrs = count;
    func->cap_instrs = capacity;
    instructions = NULL;
    free(func->assignments);
    func->assignments = NULL;
    free(func->spill_slots);
    func->spill_slots = slots;
    slots = NULL;
    func->num_spills = pinned;
    func->num_pinned_spills = pinned;
    func->cap_spills = pinned;
    recompute_block_instr_ranges(func);
    success = true;
    *changed = true;

done:
    if (!success)
        func->num_vregs = original_vregs;

    free(slots);
    free_instr_array(instructions, instructions ? count : 0);
    free_instr_array(held, num_held);
    free_instr_array(stores, num_stores);
    free(values);
    free(epochs);
    return success;
}

/* Within a block, the spilled value (and the temporary naming it) each
 * scratch register still holds after a reload or a spill store, for
 * classes whose printers declare every write to those registers. */
typedef struct {
    bool enabled[ANVIL_MIR_REG_CLASS_COUNT];
    anvil_mir_vreg_t value[ANVIL_MIR_REG_CLASS_COUNT][64];
    anvil_mir_vreg_t temp[ANVIL_MIR_REG_CLASS_COUNT][64];
} reload_cache_t;

static void reload_cache_clear(reload_cache_t *cache)
{
    for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++)
        for (size_t r = 0; r < 64; r++)
            cache->value[c][r] = ANVIL_MIR_NO_VREG;
}

static void reload_cache_forget_registers(reload_cache_t *cache, size_t reg_class, uint64_t registers)
{
    for (size_t r = 0; r < 64; r++)
        if (registers >> r & 1)
            cache->value[reg_class][r] = ANVIL_MIR_NO_VREG;
}

/* `value` now lives in `temp`'s register (or, temp NONE, changed). */
static void reload_cache_note(reload_cache_t *cache, const anvil_mir_func_t *func, anvil_mir_vreg_t value, anvil_mir_vreg_t temp)
{
    size_t reg_class = func->vregs[value].reg_class;
    for (size_t r = 0; r < 64; r++)
        if (cache->value[reg_class][r] == value)
            cache->value[reg_class][r] = ANVIL_MIR_NO_VREG;
    int phys = temp != ANVIL_MIR_NO_VREG ? func->assignments[temp].phys_reg : -1;
    if (cache->enabled[reg_class] && phys >= 0 && phys < 64) {
        cache->value[reg_class][phys] = value;
        cache->temp[reg_class][phys] = temp;
    }
}

/* What an instruction writes: its result's register, its clobbers and its
 * printer scratch. */
static void reload_cache_forget_writes(reload_cache_t *cache, const anvil_mir_func_t *func, const anvil_mir_instr_t *instr)
{
    for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++)
        reload_cache_forget_registers(cache, c, instr->clobbers[c] | instr->scratch[c]);
    if (instr->def != ANVIL_MIR_NO_VREG && !func->assignments[instr->def].spilled && func->assignments[instr->def].phys_reg >= 0 &&
        func->assignments[instr->def].phys_reg < 64)
        reload_cache_forget_registers(cache, func->vregs[instr->def].reg_class, UINT64_C(1) << func->assignments[instr->def].phys_reg);
}

bool anvil_mir_materialize_spills(
    anvil_mir_func_t *func,
    const anvil_regalloc_class_config_t *scratch_configs,
    size_t num_scratch_configs)
{
    if (!func) return false;
    if (func->num_spills == 0) return true;
    if (!func->assignments) return false;
    if (!scratch_configs || num_scratch_configs == 0) return false;

    size_t original_vregs = func->num_vregs;
    scratch_occupancy_t occupancy;
    if (!build_scratch_occupancy(func, scratch_configs, num_scratch_configs, &occupancy))
        return false;
    if (!scratch_configs_cover_spills(func, scratch_configs,
                                      num_scratch_configs, &occupancy, NULL, NULL)) {
        free(occupancy.items);
        return false;
    }
    anvil_mir_instr_t *new_instrs = NULL;
    size_t new_num_instrs = 0;
    size_t new_cap_instrs = 0;
    rematerialization_t *rematerialize = find_rematerializable_values(func);
    if (!rematerialize) {
        free(occupancy.items);
        return false;
    }

    /* Instructions that must stay adjacent (RET_VALUE_PART... RET, and
     * CALL CALL_RESULT...) form a bundle: its reloads all go before it, its
     * spill stores after it, and its members share scratch registers. */
    anvil_mir_instr_t *held = NULL;
    size_t num_held = 0, cap_held = 0;
    anvil_mir_instr_t *stores = NULL;
    size_t num_stores = 0, cap_stores = 0;
    uint64_t scratch_taken[ANVIL_MIR_REG_CLASS_COUNT] = { 0 };
    uint64_t excluded[ANVIL_MIR_REG_CLASS_COUNT] = { 0 };
    bool ok = true;
    reload_cache_t cache;
    reload_cache_clear(&cache);
    for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++) {
        const anvil_regalloc_class_config_t *config = spill_config_for_class(scratch_configs, num_scratch_configs, (anvil_mir_reg_class_t)c);
        cache.enabled[c] = config && config->printer_declares_writes;
    }
    anvil_mir_block_t cache_block = ANVIL_MIR_NO_BLOCK;
    size_t bundle_start = 0;

    for (size_t i = 0; ok && i < func->num_instrs; i++) {
        const anvil_mir_instr_t *old = &func->instrs[i];
        if (old->block != cache_block) {
            reload_cache_clear(&cache);
            cache_block = old->block;
        }
        if (old->def != ANVIL_MIR_NO_VREG && assignment_is_spilled(func, old->def) && rematerialize[old->def].instruction)
            continue;

        /* A copy between a register and a spill slot is the store or the
         * load itself (a copy between two spilled values still needs its
         * register in between). */
        if (num_held == 0 && old->op == ANVIL_MIR_OP_COPY && old->num_uses == 1 &&
            func->vregs[old->def].reg_class == func->vregs[old->uses[0]].reg_class &&
            func->vregs[old->def].size_bits == func->vregs[old->uses[0]].size_bits &&
            assignment_is_spilled(func, old->def) != assignment_is_spilled(func, old->uses[0])) {
            anvil_mir_vreg_t source = old->uses[0];
            if (assignment_is_spilled(func, old->def)) {
                ok = append_spill_store(&new_instrs, &new_num_instrs, &new_cap_instrs, old->block, source, func->assignments[old->def].spill_slot);
                reload_cache_note(&cache, func, old->def, ANVIL_MIR_NO_VREG);
            } else {
                ok = append_spill_load(&new_instrs, &new_num_instrs, &new_cap_instrs, old->block, old->def, func->assignments[source].spill_slot,
                                       rematerialize[source].instruction);
                reload_cache_forget_writes(&cache, func, old);
            }
            continue;
        }

        if (num_held == 0) {
            memset(scratch_taken, 0, sizeof(scratch_taken));
            bundle_scratch(func, i, excluded);
            bundle_start = i;
        }
        /* This instruction's first reload register of each class, and its
         * first narrow one. */
        int first_reload[ANVIL_MIR_REG_CLASS_COUNT], first_narrow_reload[ANVIL_MIR_REG_CLASS_COUNT];
        for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++)
            first_reload[c] = first_narrow_reload[c] = -1;

        anvil_mir_instr_t patched;
        if (!clone_instr_for_materialization(old, &patched)) {
            ok = false;
            break;
        }

        for (size_t u = 0; ok && u < patched.num_uses; u++) {
            anvil_mir_vreg_t use = patched.uses[u];
            if (!assignment_is_spilled(func, use)) continue;

            size_t previous = first_matching_use(old, u);
            if (previous != u)
            {
                patched.uses[u] = patched.uses[previous];
                continue;
            }

            const anvil_mir_vreg_info_t *info = &func->vregs[use];
            int phys_reg = -1;
            anvil_mir_vreg_t temp = ANVIL_MIR_NO_VREG;

            /* A register still holding the value since an earlier reload or
             * store in this block (outside bundles, whose reloads all come
             * first). */
            for (int r = 0; num_held == 0 && !mir_bundle_continues(func, i) && r < 64; r++) {
                if (cache.value[info->reg_class][r] == use && !((scratch_taken[info->reg_class] | excluded[info->reg_class]) >> r & 1)) {
                    phys_reg = r;
                    temp = cache.temp[info->reg_class][r];
                }
            }
            if (temp != ANVIL_MIR_NO_VREG) {
                scratch_taken[info->reg_class] |= UINT64_C(1) << phys_reg;
                patched.uses[u] = temp;
                if (first_reload[info->reg_class] < 0)
                    first_reload[info->reg_class] = phys_reg;
                if (first_narrow_reload[info->reg_class] < 0 &&
                    scratch_phys_narrow(spill_config_for_class(scratch_configs, num_scratch_configs, info->reg_class), phys_reg))
                    first_narrow_reload[info->reg_class] = phys_reg;
                continue;
            }

            ok = next_scratch_phys(scratch_configs, num_scratch_configs,
                                   info->reg_class, info->size_bits, scratch_taken, &occupancy,
                                   excluded, bundle_start, i, &phys_reg) &&
                 (temp = add_spill_temp_vreg(func, info->reg_class,
                                             info->size_bits, info->is_signed,
                                             phys_reg)) != ANVIL_MIR_NO_VREG &&
                 append_spill_load(&new_instrs, &new_num_instrs,
                                   &new_cap_instrs, old->block, temp,
                                   func->assignments[use].spill_slot,
                                   rematerialize[use].instruction);
            patched.uses[u] = temp;
            if (ok)
                reload_cache_note(&cache, func, use, temp);
            anvil_mir_reg_class_t reg_class = func->vregs[use].reg_class;
            if (ok && first_reload[reg_class] < 0)
                first_reload[reg_class] = phys_reg;
            if (ok && first_narrow_reload[reg_class] < 0 &&
                scratch_phys_narrow(spill_config_for_class(scratch_configs, num_scratch_configs, reg_class), phys_reg))
                first_narrow_reload[reg_class] = phys_reg;
        }

        if (ok && assignment_is_spilled(func, patched.def)) {
            anvil_mir_vreg_t original_def = patched.def;
            const anvil_mir_vreg_info_t *info = &func->vregs[original_def];
            bool narrow = scratch_value_narrow(spill_config_for_class(scratch_configs, num_scratch_configs, info->reg_class),
                                               info->size_bits);
            int phys_reg = narrow ? first_narrow_reload[info->reg_class] : first_reload[info->reg_class];
            anvil_mir_vreg_t def_temp = ANVIL_MIR_NO_VREG;
            ok = (phys_reg >= 0 || next_scratch_phys(scratch_configs, num_scratch_configs,
                                                     info->reg_class, info->size_bits, scratch_taken, &occupancy,
                                                     excluded, bundle_start, i, &phys_reg)) &&
                 (def_temp = add_spill_temp_vreg(func, info->reg_class,
                                                 info->size_bits, info->is_signed,
                                                 phys_reg)) != ANVIL_MIR_NO_VREG &&
                 append_spill_store(&stores, &num_stores, &cap_stores,
                                    old->block, def_temp,
                                    func->assignments[original_def].spill_slot);
            patched.def = def_temp;
            if (ok) {
                reload_cache_forget_writes(&cache, func, &patched);
                reload_cache_note(&cache, func, original_def, def_temp);
            }
        } else if (ok) {
            reload_cache_forget_writes(&cache, func, &patched);
        }

        if (!ok || !append_owned_materialized_instr(&held, &num_held, &cap_held, &patched)) {
            free_instr_contents(&patched);
            ok = false;
            break;
        }

        if (mir_bundle_continues(func, i))
            continue;

        for (size_t h = 0; ok && h < num_held; h++)
            ok = append_owned_materialized_instr(&new_instrs, &new_num_instrs, &new_cap_instrs, &held[h]);
        for (size_t h = 0; ok && h < num_stores; h++)
            ok = append_owned_materialized_instr(&new_instrs, &new_num_instrs, &new_cap_instrs, &stores[h]);
        if (ok) {
            num_held = 0;
            num_stores = 0;
        }
    }

    free_instr_array(held, num_held);
    free_instr_array(stores, num_stores);
    free(occupancy.items);
    if (!ok) {
        /* Leave the function as it was: drop the scratch temporaries. */
        func->num_vregs = original_vregs;
        free(rematerialize);
        free_instr_array(new_instrs, new_num_instrs);
        return false;
    }

    free(rematerialize);
    free_instr_array(func->instrs, func->num_instrs);
    func->instrs = new_instrs;
    func->num_instrs = new_num_instrs;
    func->cap_instrs = new_cap_instrs;
    recompute_block_instr_ranges(func);
    return true;
}

typedef struct {
    anvil_mir_func_t *func;
    const anvil_regalloc_class_config_t *configs;
    size_t num_configs;
    scratch_occupancy_t *occupancy;
    const rematerialization_t *rematerialize;
    bool spilled;
    bool failed;
} scratch_relief_t;

static bool bundle_references(const anvil_mir_func_t *func, size_t start, size_t end, anvil_mir_vreg_t vreg)
{
    for (size_t index = start; index <= end; index++) {
        const anvil_mir_instr_t *instr = &func->instrs[index];
        if (instr->def == vreg)
            return true;
        for (size_t u = 0; u < instr->num_uses; u++)
            if (instr->uses[u] == vreg)
                return true;
    }
    return false;
}

/* Spills values that sit in the bundle's free-able scratch registers, live
 * across it: recomputable ones first, then the longest-lived. */
static void relieve_scratch_shortage(void *ctx, size_t start, size_t end, anvil_mir_reg_class_t reg_class, size_t missing,
                                     size_t narrow_missing)
{
    scratch_relief_t *relief = ctx;
    anvil_mir_func_t *func = relief->func;
    const anvil_regalloc_class_config_t *config = spill_config_for_class(relief->configs, relief->num_configs, reg_class);
    uint64_t excluded[ANVIL_MIR_REG_CLASS_COUNT];
    bundle_scratch(func, start, excluded);
    while (!relief->failed && (missing || narrow_missing)) {
        scratch_occupant_t *best = NULL;
        for (size_t index = 0; index < relief->occupancy->count; index++) {
            scratch_occupant_t *item = &relief->occupancy->items[index];
            if (item->reg_class != reg_class || item->phys_reg < 0 || item->phys_reg >= 64 || (excluded[reg_class] >> item->phys_reg & 1) ||
                item->first > end || item->last < start || func->vregs[item->vreg].has_fixed_reg || func->vregs[item->vreg].is_live_in ||
                (narrow_missing && !scratch_phys_narrow(config, item->phys_reg)) || bundle_references(func, start, end, item->vreg))
                continue;
            bool cheap = relief->rematerialize[item->vreg].instruction != NULL;
            bool best_cheap = best && relief->rematerialize[best->vreg].instruction != NULL;
            if (!best || (cheap && !best_cheap) || (cheap == best_cheap && item->last - item->first > best->last - best->first))
                best = item;
        }
        if (!best)
            return;
        int slot = anvil_mir_allocate_spill_slot(func, reg_class, func->vregs[best->vreg].size_bits);
        if (slot < 0) {
            relief->failed = true;
            return;
        }
        anvil_regalloc_assignment_t *assignment = &func->assignments[best->vreg];
        assignment->spilled = true;
        assignment->phys_reg = -1;
        assignment->spill_slot = slot;
        if (narrow_missing && scratch_phys_narrow(config, best->phys_reg))
            narrow_missing--;
        if (missing)
            missing--;
        best->phys_reg = -1;
        relief->spilled = true;
    }
}

bool anvil_mir_spill_for_scratch(anvil_mir_func_t *func, const anvil_regalloc_class_config_t *scratch_configs,
                                 size_t num_scratch_configs, bool *changed)
{
    *changed = false;
    if (!func || !func->assignments || !scratch_configs)
        return false;
    scratch_occupancy_t occupancy;
    if (!build_scratch_occupancy(func, scratch_configs, num_scratch_configs, &occupancy))
        return false;
    rematerialization_t *rematerialize = find_rematerializable_values(func);
    scratch_relief_t relief = {func, scratch_configs, num_scratch_configs, &occupancy, rematerialize, false, !rematerialize};
    if (rematerialize)
        scratch_configs_cover_spills(func, scratch_configs, num_scratch_configs, &occupancy, relieve_scratch_shortage, &relief);
    free(rematerialize);
    free(occupancy.items);
    *changed = relief.spilled;
    return !relief.failed;
}

void anvil_mir_clear_allocations(anvil_mir_func_t *func)
{
    if (!func) return;
    free(func->assignments);
    func->assignments = NULL;
    if (func->num_pinned_spills)
    {
        func->num_spills = func->num_pinned_spills;
        return;
    }

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

/* ------------------------------------------------------------------
 * Address arithmetic folding.
 * ------------------------------------------------------------------ */

typedef struct {
    anvil_mir_vreg_t base;
    anvil_mir_vreg_t index;
    unsigned scale;
} mir_address_option_t;

typedef struct {
    size_t *def_at;     /* defining instruction of single-definition values */
    uint32_t *defs;
    size_t *uses;
} mir_address_facts_t;

static bool mir_address_single(const anvil_mir_func_t *func, const mir_address_facts_t *facts, anvil_mir_vreg_t value)
{
    return value < func->num_vregs && facts->defs[value] == 1 && !func->vregs[value].has_fixed_reg && !func->vregs[value].is_live_in;
}

static bool mir_address_constant(const anvil_mir_func_t *func, const mir_address_facts_t *facts, anvil_mir_vreg_t value, int64_t *out)
{
    if (!mir_address_single(func, facts, value))
        return false;
    const anvil_mir_instr_t *def = &func->instrs[facts->def_at[value]];
    if (def->op != ANVIL_MIR_OP_MOV || !def->has_imm || def->num_uses != 0)
        return false;
    *out = def->imm;
    return true;
}

/* index * scale when `value` is a single-use MUL or SHL by 1, 2, 4 or 8. */
static bool mir_address_scaled(const anvil_mir_func_t *func, const mir_address_facts_t *facts, anvil_mir_vreg_t value,
                               anvil_mir_block_t block, anvil_mir_vreg_t *index, unsigned *scale)
{
    if (!mir_address_single(func, facts, value) || facts->uses[value] != 1)
        return false;
    const anvil_mir_instr_t *def = &func->instrs[facts->def_at[value]];
    if (def->num_uses != 2 || def->has_imm || def->block != block)
        return false;
    int64_t amount;
    if (def->op == ANVIL_MIR_OP_SHL && mir_address_constant(func, facts, def->uses[1], &amount) && amount >= 0 && amount <= 3) {
        *index = def->uses[0];
        *scale = 1u << amount;
        return true;
    }
    for (int side = 0; def->op == ANVIL_MIR_OP_MUL && side < 2; side++) {
        if (mir_address_constant(func, facts, def->uses[1 - side], &amount) &&
            (amount == 1 || amount == 2 || amount == 4 || amount == 8)) {
            *index = def->uses[side];
            *scale = (unsigned)amount;
            return true;
        }
    }
    return false;
}

static bool mir_address_removable(const anvil_mir_func_t *func, const anvil_mir_instr_t *instr, const mir_address_facts_t *facts)
{
    if (instr->def == ANVIL_MIR_NO_VREG || facts->uses[instr->def] != 0 || func->vregs[instr->def].has_fixed_reg)
        return false;
    return instr->op == ANVIL_MIR_OP_ADD || instr->op == ANVIL_MIR_OP_MUL || instr->op == ANVIL_MIR_OP_SHL ||
           (instr->op == ANVIL_MIR_OP_MOV && instr->has_imm && instr->num_uses == 0);
}

bool anvil_mir_fold_address_arithmetic(anvil_mir_func_t *func, anvil_mir_address_mode_fn legal, void *user)
{
    if (!func)
        return false;
    if (!legal || !func->num_instrs || !func->num_vregs)
        return true;

    mir_address_facts_t facts = {
        malloc(func->num_vregs * sizeof(size_t)),
        calloc(func->num_vregs, sizeof(uint32_t)),
        calloc(func->num_vregs, sizeof(size_t)),
    };
    bool *dead = calloc(func->num_instrs, sizeof(bool));
    bool ok = facts.def_at && facts.defs && facts.uses && dead;
    for (size_t i = 0; ok && i < func->num_instrs; i++) {
        const anvil_mir_instr_t *instr = &func->instrs[i];
        if (instr->def != ANVIL_MIR_NO_VREG && instr->def < func->num_vregs) {
            facts.defs[instr->def]++;
            facts.def_at[instr->def] = i;
        }
        for (size_t u = 0; u < instr->num_uses; u++)
            if (instr->uses[u] < func->num_vregs)
                facts.uses[instr->uses[u]]++;
    }

    /* An address folds when every user is a load or store through it in
     * the address's own block (a load and a store of one element, say). */
    size_t *accesses = ok ? calloc(func->num_vregs, sizeof(size_t)) : NULL;
    ok = ok && accesses;
    for (size_t i = 0; ok && i < func->num_instrs; i++) {
        const anvil_mir_instr_t *access = &func->instrs[i];
        size_t pointer = access->op == ANVIL_MIR_OP_LOAD ? 0 : 1;
        if ((access->op != ANVIL_MIR_OP_LOAD && access->op != ANVIL_MIR_OP_STORE) || access->index_scale || access->num_uses != pointer + 1)
            continue;
        anvil_mir_vreg_t address = access->uses[pointer];
        if (mir_address_single(func, &facts, address) && func->instrs[facts.def_at[address]].block == access->block &&
            (access->op == ANVIL_MIR_OP_LOAD || access->uses[0] != address))
            accesses[address]++;
    }

    bool changed = false;
    for (size_t i = 0; ok && i < func->num_instrs; i++) {
        anvil_mir_instr_t *access = &func->instrs[i];
        if ((access->op != ANVIL_MIR_OP_LOAD && access->op != ANVIL_MIR_OP_STORE) || access->index_scale)
            continue;
        size_t pointer = access->op == ANVIL_MIR_OP_LOAD ? 0 : 1;
        if (access->num_uses != pointer + 1)
            continue;
        anvil_mir_vreg_t address = access->uses[pointer];
        if (!mir_address_single(func, &facts, address) || facts.uses[address] == 0 || facts.uses[address] != accesses[address])
            continue;
        const anvil_mir_instr_t *add = &func->instrs[facts.def_at[address]];
        if (add->op != ANVIL_MIR_OP_ADD || add->num_uses != 2 || add->has_imm || add->block != access->block)
            continue;

        /* Candidates: a scaled operand (either side), then base + index. */
        mir_address_option_t options[3];
        size_t num_options = 0;
        anvil_mir_vreg_t scaled_index;
        unsigned scaled_by;
        if (mir_address_scaled(func, &facts, add->uses[1], access->block, &scaled_index, &scaled_by))
            options[num_options++] = (mir_address_option_t){add->uses[0], scaled_index, scaled_by};
        if (mir_address_scaled(func, &facts, add->uses[0], access->block, &scaled_index, &scaled_by))
            options[num_options++] = (mir_address_option_t){add->uses[1], scaled_index, scaled_by};
        options[num_options++] = (mir_address_option_t){add->uses[0], add->uses[1], 1};

        anvil_mir_vreg_t accessed = access->op == ANVIL_MIR_OP_LOAD ? access->def : access->uses[0];
        uint16_t width = func->vregs[address].size_bits;
        anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG, index = ANVIL_MIR_NO_VREG;
        unsigned scale = 0;
        for (size_t option = 0; option < num_options && !scale; option++) {
            anvil_mir_vreg_t b_reg = options[option].base, i_reg = options[option].index;
            if (func->vregs[b_reg].size_bits != width || func->vregs[i_reg].size_bits != width ||
                func->vregs[b_reg].reg_class != ANVIL_MIR_REG_GPR || func->vregs[i_reg].reg_class != ANVIL_MIR_REG_GPR ||
                !legal(user, options[option].scale, access->has_imm ? access->imm : 0, func->vregs[accessed].size_bits))
                continue;
            /* base and index are read at the access now: neither may be
             * redefined after the arithmetic that read them. */
            size_t from = facts.def_at[address];
            if (i_reg != add->uses[0] && i_reg != add->uses[1])
                from = facts.def_at[add->uses[b_reg == add->uses[0] ? 1 : 0]];
            bool redefined = from > i;
            for (size_t j = from + 1; j < i && !redefined; j++)
                redefined = func->instrs[j].block == access->block && (func->instrs[j].def == b_reg || func->instrs[j].def == i_reg);
            if (redefined)
                continue;
            base = b_reg;
            index = i_reg;
            scale = options[option].scale;
        }
        if (!scale)
            continue;

        anvil_mir_vreg_t *uses = realloc(access->uses, (access->num_uses + 1) * sizeof(*uses));
        if (!uses) {
            ok = false;
            break;
        }
        access->uses = uses;
        access->uses[pointer] = base;
        access->uses[pointer + 1] = index;
        access->num_uses++;
        access->index_scale = (uint8_t)scale;
        facts.uses[address]--;
        accesses[address]--;
        facts.uses[base]++;
        facts.uses[index]++;
        changed = true;
    }
    free(accesses);

    /* Address arithmetic left without users. */
    for (bool removed = changed; ok && removed;) {
        removed = false;
        for (size_t i = func->num_instrs; i-- > 0;) {
            anvil_mir_instr_t *instr = &func->instrs[i];
            if (dead[i] || !mir_address_removable(func, instr, &facts))
                continue;
            dead[i] = true;
            removed = true;
            for (size_t u = 0; u < instr->num_uses; u++)
                facts.uses[instr->uses[u]]--;
        }
    }

    if (ok && changed) {
        size_t kept = 0;
        for (size_t i = 0; i < func->num_instrs; i++) {
            if (dead[i]) {
                free_instr_contents(&func->instrs[i]);
                continue;
            }
            func->instrs[kept++] = func->instrs[i];
        }
        func->num_instrs = kept;
        recompute_block_instr_ranges(func);
        anvil_mir_clear_allocations(func);
    }

    free(facts.def_at);
    free(facts.defs);
    free(facts.uses);
    free(dead);
    return ok;
}

/* ------------------------------------------------------------------
 * Local value numbering over pure MachineIR operations.
 * ------------------------------------------------------------------ */

static bool mir_cse_candidate(const anvil_mir_func_t *func, const anvil_mir_instr_t *instr)
{
    switch (instr->op) {
    case ANVIL_MIR_OP_SEXT:
    case ANVIL_MIR_OP_ZEXT:
    case ANVIL_MIR_OP_TRUNC:
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_MUL:
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR:
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
        break;
    default:
        return false;
    }
    if (instr->def == ANVIL_MIR_NO_VREG || instr->num_uses == 0 || instr->num_uses > 2 || func->vregs[instr->def].has_fixed_reg ||
        func->vregs[instr->def].reg_class != ANVIL_MIR_REG_GPR)
        return false;
    for (size_t u = 0; u < instr->num_uses; u++)
        if (func->vregs[instr->uses[u]].has_fixed_reg)
            return false;
    return true;
}

static bool mir_cse_same(const anvil_mir_func_t *func, const anvil_mir_instr_t *a, const anvil_mir_instr_t *b)
{
    if (a->op != b->op || a->num_uses != b->num_uses || a->has_imm != b->has_imm || (a->has_imm && a->imm != b->imm))
        return false;
    const anvil_mir_vreg_info_t *da = &func->vregs[a->def], *db = &func->vregs[b->def];
    if (da->size_bits != db->size_bits || da->is_signed != db->is_signed)
        return false;
    for (size_t u = 0; u < a->num_uses; u++)
        if (a->uses[u] != b->uses[u])
            return false;
    return true;
}

/* A repeated pure computation in a block becomes a copy of the first
 * (coalescing removes it) while neither its operands nor its result have
 * been redefined in between. */
bool anvil_mir_local_cse(anvil_mir_func_t *func)
{
    if (!func)
        return false;
    size_t *available = malloc((func->num_instrs ? func->num_instrs : 1) * sizeof(size_t));
    if (!available)
        return false;

    bool changed = false;
    for (size_t block = 0; block < func->num_blocks; block++) {
        size_t count = 0;
        for (size_t i = 0; i < func->num_instrs; i++) {
            anvil_mir_instr_t *instr = &func->instrs[i];
            if (instr->block != (anvil_mir_block_t)block)
                continue;

            size_t found = SIZE_MAX;
            if (mir_cse_candidate(func, instr))
                for (size_t k = 0; k < count && found == SIZE_MAX; k++)
                    if (mir_cse_same(func, &func->instrs[available[k]], instr))
                        found = available[k];

            if (found != SIZE_MAX && func->instrs[found].def != instr->def) {
                instr->op = ANVIL_MIR_OP_COPY;
                instr->uses[0] = func->instrs[found].def;
                instr->num_uses = 1;
                instr->has_imm = false;
                instr->imm = 0;
                changed = true;
            }

            /* This definition invalidates what read or produced it. */
            if (instr->def != ANVIL_MIR_NO_VREG) {
                size_t kept = 0;
                for (size_t k = 0; k < count; k++) {
                    const anvil_mir_instr_t *entry = &func->instrs[available[k]];
                    bool stale = entry->def == instr->def;
                    for (size_t u = 0; u < entry->num_uses && !stale; u++)
                        stale = entry->uses[u] == instr->def;
                    if (!stale)
                        available[kept++] = available[k];
                }
                count = kept;
            }
            if (found == SIZE_MAX && instr->op != ANVIL_MIR_OP_COPY && mir_cse_candidate(func, instr)) {
                bool self_reading = false;
                for (size_t u = 0; u < instr->num_uses; u++)
                    self_reading |= instr->uses[u] == instr->def;
                if (!self_reading)
                    available[count++] = i;
            }
        }
    }

    free(available);
    if (changed)
        anvil_mir_clear_allocations(func);
    return true;
}

/* ------------------------------------------------------------------
 * Branch-target duplication (rotates top-tested loops).
 * ------------------------------------------------------------------ */

static bool mir_duplicable(const anvil_mir_func_t *func, const anvil_mir_instr_t *instr)
{
    switch (instr->op) {
    case ANVIL_MIR_OP_MOV:
    case ANVIL_MIR_OP_COPY:
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR:
    case ANVIL_MIR_OP_SEXT:
    case ANVIL_MIR_OP_ZEXT:
    case ANVIL_MIR_OP_TRUNC:
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
    case ANVIL_MIR_OP_CMP_UGE:
        break;
    default:
        return false;
    }
    if (instr->def == ANVIL_MIR_NO_VREG || func->vregs[instr->def].has_fixed_reg || instr->symbol)
        return false;
    for (size_t u = 0; u < instr->num_uses; u++)
        if (func->vregs[instr->uses[u]].has_fixed_reg)
            return false;
    for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++)
        if (instr->clobbers[c])
            return false;
    return true;
}

/* Renumbers blocks in the order their instructions appear.  Lowering emits
 * blocks in layout order (an edge block right after its source), and the
 * printers emit by block number with a branch to block + 1 falling
 * through, so code follows that layout. */
bool anvil_mir_order_blocks(anvil_mir_func_t *func)
{
    if (!func || func->num_blocks < 2)
        return func != NULL;
    size_t count = func->num_blocks;
    anvil_mir_block_t *renumber = malloc(count * sizeof(*renumber));
    anvil_mir_block_data_t *blocks = malloc(count * sizeof(*blocks));
    if (!renumber || !blocks) {
        free(renumber);
        free(blocks);
        return false;
    }

    /* Blocks with instructions in order of their first one, then empty
     * blocks in their old order; the entry stays first. */
    for (size_t b = 0; b < count; b++)
        renumber[b] = ANVIL_MIR_NO_BLOCK;
    size_t next = 0;
    renumber[0] = (anvil_mir_block_t)next++;
    for (size_t i = 0; i < func->num_instrs; i++) {
        anvil_mir_block_t block = func->instrs[i].block;
        if (mir_valid_block(func, block) && renumber[block] == ANVIL_MIR_NO_BLOCK)
            renumber[block] = (anvil_mir_block_t)next++;
    }
    bool identity = true;
    for (size_t b = 0; b < count; b++) {
        if (renumber[b] == ANVIL_MIR_NO_BLOCK)
            renumber[b] = (anvil_mir_block_t)next++;
        identity &= renumber[b] == b;
    }

    if (!identity) {
        for (size_t b = 0; b < count; b++)
            blocks[renumber[b]] = func->blocks[b];
        memcpy(func->blocks, blocks, count * sizeof(*blocks));
        for (size_t i = 0; i < func->num_instrs; i++) {
            anvil_mir_instr_t *instr = &func->instrs[i];
            if (mir_valid_block(func, instr->block))
                instr->block = renumber[instr->block];
            if (instr->true_block != ANVIL_MIR_NO_BLOCK)
                instr->true_block = renumber[instr->true_block];
            if (instr->false_block != ANVIL_MIR_NO_BLOCK)
                instr->false_block = renumber[instr->false_block];
        }
        if (mir_valid_block(func, func->current_block))
            func->current_block = renumber[func->current_block];
    }
    free(renumber);
    free(blocks);
    return true;
}

/* A block ending in an unconditional branch to a small test block (a few
 * pure instructions and a conditional branch) gets a copy of that block
 * instead of the jump, as long as the test block keeps another incoming
 * branch: the latch of a top-tested loop then branches back by itself. Values the test block defines and uses only internally get
 * fresh registers in the copy, so each compare keeps a single use. */
bool anvil_mir_duplicate_branch_targets(anvil_mir_func_t *func, size_t max_instrs)
{
    if (!func)
        return false;
    if (func->num_blocks < 2 || !func->num_instrs || max_instrs == 0)
        return true;

    /* Per block: its instructions (array order), when it qualifies. */
    size_t *members = malloc(func->num_blocks * (max_instrs + 1) * sizeof(size_t));
    size_t *member_count = calloc(func->num_blocks, sizeof(size_t));
    bool *candidate = calloc(func->num_blocks, sizeof(bool));
    size_t *uses = calloc(func->num_vregs ? func->num_vregs : 1, sizeof(size_t));
    size_t *latches = malloc(func->num_blocks * sizeof(size_t));
    bool ok = members && member_count && candidate && uses && latches;
    for (size_t b = 0; ok && b < func->num_blocks; b++)
        candidate[b] = true;
    for (size_t i = 0; ok && i < func->num_instrs; i++) {
        const anvil_mir_instr_t *instr = &func->instrs[i];
        size_t b = instr->block;
        for (size_t u = 0; u < instr->num_uses; u++)
            uses[instr->uses[u]]++;
        if (!candidate[b])
            continue;
        if (member_count[b] > max_instrs) {
            candidate[b] = false;
            continue;
        }
        members[b * (max_instrs + 1) + member_count[b]++] = i;
    }

    size_t num_latches = 0;
    for (size_t b = 1; ok && b < func->num_blocks; b++) {
        size_t n = member_count[b];
        if (!candidate[b] || n == 0) {
            candidate[b] = false;
            continue;
        }
        const anvil_mir_instr_t *last = &func->instrs[members[b * (max_instrs + 1) + n - 1]];
        bool shape = last->op == ANVIL_MIR_OP_BR_COND && last->true_block != (anvil_mir_block_t)b && last->false_block != (anvil_mir_block_t)b;
        for (size_t k = 0; shape && k + 1 < n; k++)
            shape = mir_duplicable(func, &func->instrs[members[b * (max_instrs + 1) + k]]);
        candidate[b] = shape;
    }
    candidate[0] = false;   /* the entry block keeps its single copy */
    for (size_t i = 0; ok && i < func->num_instrs; i++) {
        const anvil_mir_instr_t *instr = &func->instrs[i];
        if (instr->op == ANVIL_MIR_OP_BR && instr->true_block < func->num_blocks && candidate[instr->true_block] &&
            instr->true_block != instr->block)
            latches[num_latches++] = i;
    }

    /* A test block keeps at least one incoming branch (else it would
     * become unreachable). */
    size_t *edges = ok && num_latches ? calloc(func->num_blocks, sizeof(size_t)) : NULL;
    size_t *replaced = ok && num_latches ? calloc(func->num_blocks, sizeof(size_t)) : NULL;
    ok = ok && (!num_latches || (edges && replaced));
    for (size_t i = 0; ok && num_latches && i < func->num_instrs; i++) {
        const anvil_mir_instr_t *instr = &func->instrs[i];
        if ((instr->op == ANVIL_MIR_OP_BR || instr->op == ANVIL_MIR_OP_BR_COND) && instr->true_block < func->num_blocks)
            edges[instr->true_block]++;
        if (instr->op == ANVIL_MIR_OP_BR_COND && instr->false_block < func->num_blocks)
            edges[instr->false_block]++;
    }
    for (size_t l = 0; ok && l < num_latches; l++)
        replaced[func->instrs[latches[l]].true_block]++;
    /* When every edge would be replaced, the earliest jump (the loop
     * entry, laid out first) keeps branching to the test. */
    size_t kept_latches = 0;
    for (size_t l = 0; ok && l < num_latches; l++) {
        size_t target = func->instrs[latches[l]].true_block;
        if (replaced[target] == edges[target]) {
            edges[target]++;   /* this edge now stays */
            continue;
        }
        latches[kept_latches++] = latches[l];
    }
    num_latches = ok ? kept_latches : 0;
    free(edges);
    free(replaced);

    size_t added = 0;
    for (size_t l = 0; ok && l < num_latches; l++)
        added += member_count[func->instrs[latches[l]].true_block] - 1;
    anvil_mir_instr_t *rebuilt = ok && num_latches ? calloc(func->num_instrs + added, sizeof(*rebuilt)) : NULL;
    bool *is_copy = ok && num_latches ? calloc(func->num_instrs + added, sizeof(bool)) : NULL;
    ok = ok && (!num_latches || (rebuilt && is_copy));

    size_t count = 0;
    bool changed = false;
    if (ok && num_latches) {
        /* Instructions keep their order; each latch's jump becomes the copy. */
        size_t next_latch = 0;
        for (size_t i = 0; ok && i < func->num_instrs; i++) {
            if (next_latch < num_latches && latches[next_latch] == i) {
                next_latch++;
                const anvil_mir_instr_t *jump = &func->instrs[i];
                size_t target = jump->true_block, n = member_count[target];
                anvil_mir_vreg_t renamed_from[16], renamed_to[16];
                size_t num_renamed = 0;
                for (size_t k = 0; ok && k < n; k++) {
                    const anvil_mir_instr_t *src = &func->instrs[members[target * (max_instrs + 1) + k]];
                    anvil_mir_instr_t copy;
                    if (!clone_instr_for_materialization(src, &copy)) {
                        ok = false;
                        break;
                    }
                    copy.block = jump->block;
                    for (size_t u = 0; u < copy.num_uses; u++)
                        for (size_t r = 0; r < num_renamed; r++)
                            if (copy.uses[u] == renamed_from[r])
                                copy.uses[u] = renamed_to[r];
                    /* A temporary whose every use is inside the test block. */
                    if (copy.def != ANVIL_MIR_NO_VREG && num_renamed < 16) {
                        size_t inside = 0;
                        for (size_t q = k + 1; q < n; q++) {
                            const anvil_mir_instr_t *later = &func->instrs[members[target * (max_instrs + 1) + q]];
                            for (size_t u = 0; u < later->num_uses; u++)
                                inside += later->uses[u] == src->def;
                        }
                        if (inside && inside == uses[src->def]) {
                            anvil_mir_vreg_info_t info = func->vregs[src->def];
                            anvil_mir_vreg_t fresh = anvil_mir_add_vreg_typed(func, info.reg_class, info.size_bits, info.is_signed);
                            if (fresh == ANVIL_MIR_NO_VREG) {
                                free_instr_contents(&copy);
                                ok = false;
                                break;
                            }
                            renamed_from[num_renamed] = src->def;
                            renamed_to[num_renamed++] = fresh;
                            copy.def = fresh;
                        }
                    }
                    is_copy[count] = true;
                    rebuilt[count++] = copy;
                }
                changed = true;
                continue;
            }
            rebuilt[count++] = func->instrs[i];
        }
    }

    if (ok && changed) {
        /* The replaced jumps are the only originals not carried over. */
        for (size_t l = 0; l < num_latches; l++)
            free_instr_contents(&func->instrs[latches[l]]);
        free(func->instrs);
        func->instrs = rebuilt;
        func->num_instrs = count;
        func->cap_instrs = func->num_instrs + added;
        recompute_block_instr_ranges(func);
        anvil_mir_clear_allocations(func);
    } else {
        for (size_t k = 0; rebuilt && k < count; k++)
            if (is_copy[k])
                free_instr_contents(&rebuilt[k]);
        free(rebuilt);
    }
    free(is_copy);
    free(members);
    free(member_count);
    free(candidate);
    free(uses);
    free(latches);
    return ok;
}

bool anvil_mir_set_instr_fcmp_pred(anvil_mir_func_t *func, size_t index, anvil_fcmp_pred_t predicate)
{
    if (!func || index >= func->num_instrs || func->instrs[index].op != ANVIL_MIR_OP_VECTOR || func->instrs[index].ir_op != ANVIL_OP_FCMP ||
        (unsigned)predicate > (unsigned)ANVIL_FCMP_TRUE)
        return false;
    func->instrs[index].fcmp_pred = predicate;
    return true;
}

bool anvil_mir_set_instr_widening(anvil_mir_func_t *func, size_t index, unsigned ratio)
{
    if (!func || index >= func->num_instrs || func->instrs[index].op != ANVIL_MIR_OP_VECTOR || (ratio != 2 && ratio != 4 && ratio != 8))
        return false;
    func->instrs[index].widening = (uint8_t)ratio;
    return true;
}

bool anvil_mir_set_instr_symbol_got(anvil_mir_func_t *func, size_t index, bool got)
{
    if (!func || index >= func->num_instrs || !func->instrs[index].symbol)
        return false;
    func->instrs[index].symbol_got = got;
    return true;
}
