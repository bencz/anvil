/* 64-bit values on targets with 32-bit general registers travel as word
 * pairs, lowered by one shared implementation (backend/common/ir_lower_pair.c)
 * on i386, 32-bit PowerPC and S/390:
 *   - a truncation to i1 keeps only the low bit (an AND with 1);
 *   - a load at a constant offset takes the offset as both words'
 *     displacements, SystemZ included;
 *   - a select picks each word with the target's SELECT where the CPU
 *     selects in a register (CMOVcc), and branches where it does not (i486);
 *   - a 32-bit value shifted by a 64-bit count shifts by the count's low
 *     word, the count a variable or a constant. */
#include "anvil/anvil_internal.h"
#include "anvil/anvil_x86_mir.h"
#include "anvil/anvil_ppc_mir.h"
#include "anvil/anvil_mainframe_mir.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond, msg)                         \
    do {                                         \
        if (!(cond)) {                           \
            fprintf(stderr, "[FAIL] %s\n", msg); \
            failures++;                          \
        }                                        \
    } while (0)

typedef struct {
    anvil_arch_t arch;
    anvil_cpu_model_t cpu;
    const char *name;
    bool selects_in_register;
} target_t;

static const target_t targets[] = {
    {ANVIL_ARCH_X86, ANVIL_CPU_X86_PENTIUM4, "pentium4", true},
    {ANVIL_ARCH_X86, ANVIL_CPU_X86_I486, "i486", false},
    {ANVIL_ARCH_PPC32, ANVIL_CPU_PPC_G4, "ppc32", true},
    {ANVIL_ARCH_S390, ANVIL_CPU_S390_G5, "s390", true},
};

static anvil_mir_func_t *lower(anvil_arch_t arch, anvil_func_t *func)
{
    switch (arch) {
    case ANVIL_ARCH_X86:
        return anvil_x86_lower_func_to_mir(func);
    case ANVIL_ARCH_PPC32:
        return anvil_ppc_lower_func_to_mir(func, ANVIL_PPC_VARIANT_PPC32);
    case ANVIL_ARCH_S390:
        return anvil_mainframe_lower_func_to_mir(func, ANVIL_MAINFRAME_VARIANT_S390);
    default:
        return NULL;
    }
}

static size_t count_op(const anvil_mir_func_t *mir, anvil_mir_opcode_t op, bool with_imm, int64_t imm)
{
    size_t count = 0;
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (anvil_mir_get_instr_info(mir, i, &info) && info.op == op && (!with_imm || (info.has_imm && info.imm == imm)))
            count++;
    }
    return count;
}

static anvil_func_t *new_func(anvil_module_t *module, const char *name, anvil_type_t *ret, anvil_type_t **params, size_t num_params)
{
    anvil_ctx_t *ctx = module->ctx;
    anvil_func_t *func = anvil_func_create(module, name, anvil_type_func(ctx, ret, params, num_params, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, func->entry);
    return func;
}

static void test_target(const target_t *target)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(target->arch);
    anvil_ctx_set_cpu(ctx, target->cpu);
    anvil_module_t *module = anvil_module_create(ctx, "pairs");
    anvil_type_t *i1 = anvil_type_i1(ctx), *i32 = anvil_type_i32(ctx), *i64 = anvil_type_i64(ctx);
    anvil_type_t *ptr = anvil_type_ptr(ctx, i64);
    char message[160];

    /* low(a) = zext(trunc a to i1) */
    anvil_type_t *one_i64[] = {i64};
    anvil_func_t *low = new_func(module, "low", i32, one_i64, 1);
    anvil_build_ret(ctx, anvil_build_zext(ctx, anvil_build_trunc(ctx, anvil_func_get_param(low, 0), i1, "bit"), i32, "wide"));
    anvil_mir_func_t *mir = lower(target->arch, low);
    snprintf(message, sizeof(message), "%s: a truncation of a pair to i1 masks the low bit", target->name);
    CHECK(mir && count_op(mir, ANVIL_MIR_OP_AND, false, 0) >= 1, message);
    anvil_mir_func_destroy(mir);

    /* second(p) = p[1] */
    anvil_type_t *one_ptr[] = {ptr};
    anvil_func_t *second = new_func(module, "second", i64, one_ptr, 1);
    anvil_value_t *index[] = {anvil_const_i32(ctx, 1)};
    anvil_value_t *address = anvil_build_gep(ctx, i64, anvil_func_get_param(second, 0), index, 1, "address");
    anvil_build_ret(ctx, anvil_build_load(ctx, i64, address, "value"));
    mir = lower(target->arch, second);
    snprintf(message, sizeof(message), "%s: a pair load at offset 8 reads displacements 8 and 12 with no address add", target->name);
    CHECK(mir && count_op(mir, ANVIL_MIR_OP_LOAD, true, 8) == 1 && count_op(mir, ANVIL_MIR_OP_LOAD, true, 12) == 1 && count_op(mir, ANVIL_MIR_OP_ADD, false, 0) == 0,
          message);
    anvil_mir_func_destroy(mir);

    /* pick(c, a, b) = c ? a : b */
    anvil_type_t *pick_params[] = {i1, i64, i64};
    anvil_func_t *pick = new_func(module, "pick", i64, pick_params, 3);
    anvil_build_ret(ctx, anvil_build_select(ctx, anvil_func_get_param(pick, 0), anvil_func_get_param(pick, 1), anvil_func_get_param(pick, 2), "picked"));
    mir = lower(target->arch, pick);
    size_t selects = mir ? count_op(mir, ANVIL_MIR_OP_SELECT, false, 0) : 0;
    size_t branches = mir ? count_op(mir, ANVIL_MIR_OP_BR_COND, false, 0) : 0;
    snprintf(message, sizeof(message), "%s: a pair select %s (%zu selects, %zu branches)", target->name,
             target->selects_in_register ? "selects each word" : "branches", selects, branches);
    CHECK(mir && (target->selects_in_register ? selects == 2 && branches == 0 : selects == 0 && branches == 1), message);
    anvil_mir_func_destroy(mir);

    /* shift(x, n) = x << n, then x << 3, the counts 64-bit */
    anvil_type_t *shift_params[] = {i32, i64};
    anvil_func_t *shift = new_func(module, "shift", i32, shift_params, 2);
    anvil_value_t *shifted = anvil_build_shl(ctx, anvil_func_get_param(shift, 0), anvil_func_get_param(shift, 1), "by_count");
    anvil_build_ret(ctx, anvil_build_shl(ctx, shifted, anvil_const_i64(ctx, 3), "by_three"));
    mir = lower(target->arch, shift);
    snprintf(message, sizeof(message), "%s: a 32-bit value shifts by a 64-bit count", target->name);
    CHECK(mir && count_op(mir, ANVIL_MIR_OP_SHL, false, 0) == 2, message);
    anvil_mir_func_destroy(mir);

    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++)
        test_target(&targets[t]);
    if (failures) {
        fprintf(stderr, "%d pair lowering regression check(s) failed\n", failures);
        return 1;
    }
    printf("pair lowering regression tests passed\n");
    return 0;
}
