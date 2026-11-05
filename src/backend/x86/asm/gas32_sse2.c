/* i386 floating point in SSE/SSE2 registers (Pentium 4 and later). */
#include "gas32_internal.h"

#include <stdlib.h>
#include <string.h>

static const char *xmm(x86_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    int phys = x86_phys_of(emit, vreg);
    return phys >= 0 && phys < 8 ? x86_xmm_names[phys] : "?";
}

/* A scalar, or a whole vector (unaligned: element-aligned streams). */
static const char *mov_op(int size)
{
    return size == 16 ? "movups" : size <= 4 ? "movss" : "movsd";
}

/* Register copies move the whole register: movss/movsd between registers
 * merge into the destination's old upper bits (a false dependency). */
static const char *const copy_op = "movaps";

static int frame_bytes(const x86_mir_emit_t *emit)
{
    (void)emit;
    return 0;
}

static void copy(x86_mir_emit_t *emit, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = x86_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = x86_vreg_info_checked(emit, src);
    int dst_phys = x86_phys_of(emit, dst);
    int src_phys = x86_phys_of(emit, src);
    if (!dst_info || !src_info || emit->failed)
        return;

    if (dst_info->reg_class == ANVIL_MIR_REG_FPR && src_info->reg_class == ANVIL_MIR_REG_FPR) {
        if (dst_phys != src_phys)
            anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", copy_op, x86_xmm_names[src_phys], x86_xmm_names[dst_phys]);
    } else if (dst_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tmovd %%%s, %%%s\n", x86_gpr_name(src_phys, 4), x86_xmm_names[dst_phys]);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmovd %%%s, %%%s\n", x86_xmm_names[src_phys], x86_gpr_name(dst_phys, 4));
    }
}

/* The literal-pool entry holding `bits` (added when new); -1 without
 * memory. */
static long pool_entry(x86_mir_emit_t *emit, uint64_t bits, int size)
{
    for (size_t i = 0; i < emit->fp_pool_count; i++)
        if (emit->fp_pool[i].bits == bits && emit->fp_pool[i].size == size)
            return (long)i;
    if (emit->fp_pool_count == emit->fp_pool_cap) {
        size_t cap = emit->fp_pool_cap ? emit->fp_pool_cap * 2 : 8;
        void *grown = realloc(emit->fp_pool, cap * sizeof(*emit->fp_pool));
        if (!grown)
            return -1;
        emit->fp_pool = grown;
        emit->fp_pool_cap = cap;
    }
    emit->fp_pool[emit->fp_pool_count].bits = bits;
    emit->fp_pool[emit->fp_pool_count].size = size;
    return (long)emit->fp_pool_count++;
}

/* +0.0 by xorps; position-dependent code loads others from the literal
 * pool, position-independent code (no register for the GOT pointer) builds
 * them in the frame. */
static void constant(x86_mir_emit_t *emit, anvil_mir_vreg_t dst, uint64_t bits)
{
    int size = x86_fp_size(emit, dst);
    const char *reg = xmm(emit, dst);
    if (emit->failed)
        return;
    if (size <= 4)
        bits &= UINT32_MAX;
    if (bits == 0) {
        anvil_strbuf_appendf(&emit->code, "\txorps %%%s, %%%s\n", reg, reg);
        return;
    }
    if (emit->reloc == ANVIL_RELOC_STATIC) {
        long entry = pool_entry(emit, bits, size);
        if (entry < 0) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\t%s %s%s_fpc%ld, %%%s\n", mov_op(size), emit->plat->format->local_label_prefix,
                             anvil_mir_func_name(emit->mir), entry, reg);
        return;
    }
    if (size <= 4) {
        anvil_strbuf_appendf(&emit->code, "\tmovl $%u, %s\n", (uint32_t)bits, x86_frame(emit, -(emit->scratch_a_off)));
        anvil_strbuf_appendf(&emit->code, "\tmovss %s, %%%s\n", x86_frame(emit, -(emit->scratch_a_off)), reg);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmovl $%u, %s\n", (uint32_t)(bits & 0xffffffffu), x86_frame(emit, -(emit->scratch_b_off)));
        anvil_strbuf_appendf(&emit->code, "\tmovl $%u, %s\n", (uint32_t)(bits >> 32), x86_frame(emit, -(emit->scratch_b_off - 4)));
        anvil_strbuf_appendf(&emit->code, "\tmovsd %s, %%%s\n", x86_frame(emit, -(emit->scratch_b_off)), reg);
    }
}

static void binary(x86_mir_emit_t *emit, anvil_mir_opcode_t op, anvil_mir_vreg_t dst, anvil_mir_vreg_t a, anvil_mir_vreg_t b)
{
    int size = x86_fp_size(emit, dst);
    bool commutative = op == ANVIL_MIR_OP_ADD || op == ANVIL_MIR_OP_MUL;
    const char *d = xmm(emit, dst), *ra = xmm(emit, a), *rb = xmm(emit, b);
    const char *name = op == ANVIL_MIR_OP_ADD ? "add" : op == ANVIL_MIR_OP_SUB ? "sub" : op == ANVIL_MIR_OP_MUL ? "mul"
                     : op == ANVIL_MIR_OP_DIV || op == ANVIL_MIR_OP_FDIV ? "div" : NULL;
    if (!name)
        emit->failed = true;
    if (emit->failed)
        return;
    /* dst may hold b: a commutative operation applies a to it, any other
     * moves b aside to XMM4, never allocated. */
    if (strcmp(d, rb) == 0 && strcmp(d, ra) != 0) {
        if (commutative) {
            anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n", name, size <= 4 ? "ss" : "sd", ra, d);
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%xmm4\n", copy_op, rb);
        rb = "xmm4";
    }
    if (strcmp(d, ra) != 0)
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", copy_op, ra, d);
    anvil_strbuf_appendf(&emit->code, "\t%s%s %%%s, %%%s\n", name, size <= 4 ? "ss" : "sd", rb, d);
}

static void unary(x86_mir_emit_t *emit, anvil_mir_opcode_t op, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    int size = x86_fp_size(emit, dst);
    const char *d = xmm(emit, dst), *s = xmm(emit, src);
    if (emit->failed)
        return;
    if (strcmp(d, s) != 0)
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", copy_op, s, d);
    if (emit->reloc != ANVIL_RELOC_STATIC && (op == ANVIL_MIR_OP_NEG || op == ANVIL_MIR_OP_FABS)) {
        /* No absolute mask address: flip the sign bit in a stack copy. */
        int high = size <= 4 ? 0 : 4;
        anvil_strbuf_appendf(&emit->code, "\tsubl $8, %%esp\n\t%s %%%s, (%%esp)\n", mov_op(size), d);
        anvil_strbuf_appendf(&emit->code, op == ANVIL_MIR_OP_NEG ? "\txorl $0x80000000, %d(%%esp)\n" : "\tandl $0x7fffffff, %d(%%esp)\n", high);
        anvil_strbuf_appendf(&emit->code, "\t%s (%%esp), %%%s\n\taddl $8, %%esp\n", mov_op(size), d);
        return;
    }
    if (op == ANVIL_MIR_OP_NEG) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s%s_fneg_mask%s, %%%s\n", size <= 4 ? "xorps" : "xorpd", emit->plat->format->local_label_prefix, anvil_mir_func_name(emit->mir), size <= 4 ? "32" : "64", d);
        emit->emitted_fneg_mask = true;
    } else if (op == ANVIL_MIR_OP_FABS) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s%s_fabs_mask%s, %%%s\n", size <= 4 ? "andps" : "andpd", emit->plat->format->local_label_prefix, anvil_mir_func_name(emit->mir), size <= 4 ? "32" : "64", d);
        emit->emitted_fabs_mask = true;
    } else {
        emit->failed = true;
    }
}

static void apply(x86_mir_emit_t *emit, const char *mnemonic, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    const char *d = xmm(emit, dst), *s = xmm(emit, src);
    if (emit->failed)
        return;
    /* Only the low lane is written: zeroing the rest first breaks the
     * dependency on the old value. */
    if (strcmp(d, s) != 0)
        anvil_strbuf_appendf(&emit->code, "\txorps %%%s, %%%s\n", d, d);
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", mnemonic, s, d);
}

static void compare(x86_mir_emit_t *emit, anvil_mir_vreg_t a, anvil_mir_vreg_t b)
{
    int size = x86_fp_size(emit, a);
    const char *ra = xmm(emit, a), *rb = xmm(emit, b);
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", size <= 4 ? "ucomiss" : "ucomisd", rb, ra);
}

static void convert(x86_mir_emit_t *emit, anvil_mir_opcode_t op, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    switch (op) {
    case ANVIL_MIR_OP_SITOFP: {
        int size = x86_fp_size(emit, dst);
        const char *d = xmm(emit, dst), *s = x86_gpr_name(x86_phys_of(emit, src), 4);
        /* Conversions into an XMM register merge into its upper bits:
         * zeroing it first breaks the dependency on its old value. */
        if (!emit->failed)
            anvil_strbuf_appendf(&emit->code, "\txorps %%%s, %%%s\n\tcvtsi2s%sl %%%s, %%%s\n", d, d, size <= 4 ? "s" : "d", s, d);
        break;
    }
    case ANVIL_MIR_OP_UITOFP: {
        /* As the 64-bit value (0, src): exact, then one rounding. */
        int size = x86_fp_size(emit, dst);
        const char *d = xmm(emit, dst), *s = x86_gpr_name(x86_phys_of(emit, src), 4);
        if (emit->failed)
            break;
        anvil_strbuf_appendf(&emit->code, "\tmovl %%%s, %s\n\tmovl $0, %s\n", s, x86_frame(emit, -(emit->scratch_b_off)), x86_frame(emit, -(emit->scratch_b_off - 4)));
        anvil_strbuf_appendf(&emit->code, "\tfildll %s\n\tfstp%s %s\n", x86_frame(emit, -(emit->scratch_b_off)), size <= 4 ? "s" : "l", x86_frame(emit, -(emit->scratch_b_off)));
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %%%s\n", mov_op(size), x86_frame(emit, -(emit->scratch_b_off)), d);
        break;
    }
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI: {
        int size = x86_fp_size(emit, src);
        const char *s = xmm(emit, src), *d = x86_gpr_name(x86_phys_of(emit, dst), 4);
        const char *sd = size <= 4 ? "s" : "d";
        if (emit->failed)
            break;
        anvil_strbuf_appendf(&emit->code, "\tcvtts%s2si %%%s, %%%s\n", sd, s, d);
        if (op == ANVIL_MIR_OP_FPTOUI) {
            /* From 2^31 up the signed conversion overflows: convert
             * x - 2^31 instead (exact there) and set bit 31.  XMM4 is never
             * allocated; -2^31 comes from the integer conversion. */
            anvil_strbuf_appendf(&emit->code, "\ttestl %%%s, %%%s\n\tjns 1f\n", d, d);
            anvil_strbuf_appendf(&emit->code, "\tmovl $-2147483648, %s\n\tcvtsi2s%sl %s, %%xmm4\n", x86_frame(emit, -(emit->scratch_a_off)), sd, x86_frame(emit, -(emit->scratch_a_off)));
            anvil_strbuf_appendf(&emit->code, "\tadds%s %%%s, %%xmm4\n\tcvtts%s2si %%xmm4, %%%s\n", sd, s, sd, d);
            anvil_strbuf_appendf(&emit->code, "\txorl $0x80000000, %%%s\n1:\n", d);
        }
        break;
    }
    case ANVIL_MIR_OP_FPEXT:
    case ANVIL_MIR_OP_FPTRUNC: {
        const char *d = xmm(emit, dst), *s = xmm(emit, src);
        if (emit->failed)
            break;
        if (strcmp(d, s) != 0)
            anvil_strbuf_appendf(&emit->code, "\txorps %%%s, %%%s\n", d, d);
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %%%s\n", op == ANVIL_MIR_OP_FPEXT ? "cvtss2sd" : "cvtsd2ss", s, d);
        break;
    }
    default:
        emit->failed = true;
        break;
    }
}

static void load(x86_mir_emit_t *emit, anvil_mir_vreg_t dst, const char *mem)
{
    int size = x86_fp_size(emit, dst);
    const char *d = xmm(emit, dst);
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %%%s\n", mov_op(size), mem, d);
}

static void store(x86_mir_emit_t *emit, anvil_mir_vreg_t src, const char *mem)
{
    int size = x86_fp_size(emit, src);
    const char *s = xmm(emit, src);
    if (!emit->failed)
        anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %s\n", mov_op(size), s, mem);
}

static void push_result(x86_mir_emit_t *emit, anvil_mir_vreg_t src)
{
    int size = x86_fp_size(emit, src);
    const char *s = xmm(emit, src);
    if (emit->failed)
        return;
    anvil_strbuf_appendf(&emit->code, "\t%s %%%s, %s\n", mov_op(size), s, x86_frame(emit, -(emit->scratch_b_off)));
    anvil_strbuf_appendf(&emit->code, "\t%s %s\n", size <= 4 ? "flds" : "fldl", x86_frame(emit, -(emit->scratch_b_off)));
}

static void pop_result(x86_mir_emit_t *emit, anvil_mir_vreg_t dst)
{
    int size = x86_fp_size(emit, dst);
    const char *d = xmm(emit, dst);
    if (emit->failed)
        return;
    anvil_strbuf_appendf(&emit->code, "\t%s %s\n", size <= 4 ? "fstps" : "fstpl", x86_frame(emit, -(emit->scratch_b_off)));
    anvil_strbuf_appendf(&emit->code, "\t%s %s, %%%s\n", mov_op(size), x86_frame(emit, -(emit->scratch_b_off)), d);
}

const x86_fp_unit_t x86_fp_sse2_unit = {
    .name = "sse2",
    .frame_bytes = frame_bytes,
    .copy = copy,
    .constant = constant,
    .binary = binary,
    .unary = unary,
    .apply = apply,
    .compare = compare,
    .convert = convert,
    .load = load,
    .store = store,
    .push_result = push_result,
    .pop_result = pop_result,
};
