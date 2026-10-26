#include "aggregate_mir.h"

#include <stdint.h>

anvil_mir_vreg_t anvil_agg_frame_object(anvil_mir_func_t *mir, size_t size, size_t align,
                                        anvil_agg_target_t target)
{
    if (size == 0 || size > UINT16_MAX / 8)
        return ANVIL_MIR_NO_VREG;
    if (align < 8)
        align = 8;
    int slot = anvil_mir_add_frame_slot(mir, (uint16_t)(size * 8), (uint16_t)align);
    anvil_mir_vreg_t address = anvil_mir_add_vreg_typed(mir, ANVIL_MIR_REG_GPR, (uint16_t)(target.word * 8), false);
    if (slot < 0 || address == ANVIL_MIR_NO_VREG || !anvil_mir_add_frame_addr(mir, address, slot))
        return ANVIL_MIR_NO_VREG;
    return address;
}

uint16_t anvil_agg_part_bits(const anvil_agg_part_t *part, anvil_agg_target_t target)
{
    return part->fp ? (part->size <= 4 ? 32 : 64) : (uint16_t)(target.word * 8);
}

static anvil_mir_vreg_t load(anvil_mir_func_t *mir, anvil_mir_reg_class_t cls, uint16_t bits,
                             anvil_mir_vreg_t base, int64_t offset)
{
    anvil_mir_vreg_t value = anvil_mir_add_vreg_typed(mir, cls, bits, false);
    anvil_mir_vreg_t uses[] = {base};
    if (value == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;
    bool ok = offset == 0 ? anvil_mir_add_instr(mir, ANVIL_MIR_OP_LOAD, value, uses, 1)
                          : anvil_mir_add_instr_imm_uses(mir, ANVIL_MIR_OP_LOAD, value, uses, 1, offset);
    return ok ? value : ANVIL_MIR_NO_VREG;
}

static bool store(anvil_mir_func_t *mir, anvil_mir_vreg_t value, anvil_mir_vreg_t base, int64_t offset)
{
    anvil_mir_vreg_t uses[] = {value, base};
    return offset == 0 ? anvil_mir_add_instr(mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, uses, 2)
                       : anvil_mir_add_instr_imm_uses(mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, uses, 2, offset);
}

static anvil_mir_vreg_t unary(anvil_mir_func_t *mir, anvil_mir_opcode_t op, uint16_t bits, anvil_mir_vreg_t src)
{
    anvil_mir_vreg_t def = anvil_mir_add_vreg_typed(mir, ANVIL_MIR_REG_GPR, bits, false);
    anvil_mir_vreg_t uses[] = {src};
    return def != ANVIL_MIR_NO_VREG && anvil_mir_add_instr(mir, op, def, uses, 1) ? def : ANVIL_MIR_NO_VREG;
}

static bool byte_copy(anvil_mir_func_t *mir, anvil_mir_vreg_t from, int64_t from_offset,
                      anvil_mir_vreg_t to, int64_t to_offset, unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        anvil_mir_vreg_t byte = load(mir, ANVIL_MIR_REG_GPR, 8, from, from_offset + i);
        if (byte == ANVIL_MIR_NO_VREG || !store(mir, byte, to, to_offset + i))
            return false;
    }
    return true;
}

/* True when a zero-extending load of the part alone yields its register
 * image: the part is naturally sized and sits at the low-order end. */
static bool direct(const anvil_agg_part_t *part, anvil_agg_target_t target)
{
    unsigned size = part->size;
    bool natural = size == 1 || size == 2 || size == 4 || size == target.word;
    return natural && part->pad == (target.big_endian ? target.word - size : 0);
}

anvil_mir_vreg_t anvil_agg_load_part(anvil_mir_func_t *mir, const anvil_agg_part_t *part,
                                     anvil_mir_vreg_t base, anvil_agg_target_t target)
{
    if (part->fp)
        return load(mir, ANVIL_MIR_REG_FPR, part->size <= 4 ? 32 : 64, base, part->offset);

    uint16_t bits = (uint16_t)(target.word * 8);
    if (direct(part, target)) {
        anvil_mir_vreg_t value = load(mir, ANVIL_MIR_REG_GPR, (uint16_t)(part->size * 8), base, part->offset);
        return value != ANVIL_MIR_NO_VREG && part->size < target.word ? unary(mir, ANVIL_MIR_OP_ZEXT, bits, value) : value;
    }

    /* Assemble the image in a zeroed word-sized bounce slot. */
    anvil_mir_vreg_t bounce = anvil_agg_frame_object(mir, target.word, target.word, target);
    anvil_mir_vreg_t zero = anvil_mir_add_vreg_typed(mir, ANVIL_MIR_REG_GPR, bits, false);
    if (bounce == ANVIL_MIR_NO_VREG || zero == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, zero, 0) ||
        !store(mir, zero, bounce, 0) || !byte_copy(mir, base, part->offset, bounce, part->pad, part->size))
        return ANVIL_MIR_NO_VREG;
    return load(mir, ANVIL_MIR_REG_GPR, bits, bounce, 0);
}

bool anvil_agg_store_part(anvil_mir_func_t *mir, const anvil_agg_part_t *part,
                          anvil_mir_vreg_t value, anvil_mir_vreg_t base, anvil_agg_target_t target)
{
    if (part->fp)
        return store(mir, value, base, part->offset);

    if (direct(part, target)) {
        anvil_mir_vreg_t narrow = part->size < target.word ? unary(mir, ANVIL_MIR_OP_TRUNC, (uint16_t)(part->size * 8), value) : value;
        return narrow != ANVIL_MIR_NO_VREG && store(mir, narrow, base, part->offset);
    }

    anvil_mir_vreg_t bounce = anvil_agg_frame_object(mir, target.word, target.word, target);
    return bounce != ANVIL_MIR_NO_VREG && store(mir, value, bounce, 0) &&
           byte_copy(mir, bounce, part->pad, base, part->offset, part->size);
}

anvil_agg_part_t anvil_agg_chunk(size_t size, size_t offset, anvil_agg_target_t target)
{
    size_t left = size - offset;
    return (anvil_agg_part_t){false, (unsigned)(left < target.word ? left : target.word), (unsigned)offset, 0};
}

bool anvil_agg_copy(anvil_mir_func_t *mir, anvil_mir_vreg_t from, anvil_mir_vreg_t to,
                    size_t size, anvil_agg_target_t target)
{
    for (size_t offset = 0; offset < size;) {
        unsigned piece = target.word;
        while (piece > size - offset)
            piece /= 2;
        anvil_mir_vreg_t value = load(mir, ANVIL_MIR_REG_GPR, (uint16_t)(piece * 8), from, (int64_t)offset);
        if (value == ANVIL_MIR_NO_VREG || !store(mir, value, to, (int64_t)offset))
            return false;
        offset += piece;
    }
    return true;
}
