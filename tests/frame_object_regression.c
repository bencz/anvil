/* Stack objects and globals take the IR type's own layout on every MachineIR
 * back end: an ALLOCA's frame slot has the type's size and alignment, capped
 * at what the stack keeps (a packed struct is byte aligned, a vector keeps
 * its alignment), a dynamic ALLOCA counts in the address width (a 64-bit
 * count on a 32-bit target by its low word), and a global is aligned as its
 * type (a packed struct by the byte). */
#include "anvil/anvil_internal.h"
#include "anvil/anvil_x86_64_mir.h"
#include "anvil/anvil_x86_mir.h"
#include "anvil/anvil_arm64_mir.h"
#include "anvil/anvil_ppc_mir.h"
#include "anvil/anvil_mainframe_mir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    size_t stack_alignment;
} target_t;

static const target_t targets[] = {
    {ANVIL_ARCH_X86_64, 16}, {ANVIL_ARCH_X86, 16},  {ANVIL_ARCH_ARM64, 16}, {ANVIL_ARCH_PPC32, 16}, {ANVIL_ARCH_PPC64, 16},
    {ANVIL_ARCH_PPC64LE, 16}, {ANVIL_ARCH_S370, 8}, {ANVIL_ARCH_S390, 8},   {ANVIL_ARCH_ZARCH, 8},
};

static anvil_mir_func_t *lower(anvil_arch_t arch, anvil_func_t *func)
{
    switch (arch) {
    case ANVIL_ARCH_X86_64:
        return anvil_x86_64_lower_func_to_mir(func);
    case ANVIL_ARCH_X86:
        return anvil_x86_lower_func_to_mir(func);
    case ANVIL_ARCH_ARM64:
        return anvil_arm64_lower_func_to_mir(func);
    case ANVIL_ARCH_PPC32:
        return anvil_ppc_lower_func_to_mir(func, ANVIL_PPC_VARIANT_PPC32);
    case ANVIL_ARCH_PPC64:
        return anvil_ppc_lower_func_to_mir(func, ANVIL_PPC_VARIANT_PPC64);
    case ANVIL_ARCH_PPC64LE:
        return anvil_ppc_lower_func_to_mir(func, ANVIL_PPC_VARIANT_PPC64LE);
    case ANVIL_ARCH_S370:
        return anvil_mainframe_lower_func_to_mir(func, ANVIL_MAINFRAME_VARIANT_S370);
    case ANVIL_ARCH_S390:
        return anvil_mainframe_lower_func_to_mir(func, ANVIL_MAINFRAME_VARIANT_S390);
    case ANVIL_ARCH_ZARCH:
        return anvil_mainframe_lower_func_to_mir(func, ANVIL_MAINFRAME_VARIANT_ZARCH);
    default:
        return NULL;
    }
}

static bool has_op(const anvil_mir_func_t *mir, anvil_mir_opcode_t op)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (anvil_mir_get_instr_info(mir, i, &info) && info.op == op)
            return true;
    }
    return false;
}

/* f(i64 n): a packed { i8, i32 }, an i64, a <4 x i32> and n bytes. */
static void test_frame_objects(const target_t *target)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(target->arch);
    anvil_module_t *module = anvil_module_create(ctx, "frame");
    anvil_type_t *i8 = anvil_type_i8(ctx), *i32 = anvil_type_i32(ctx), *i64 = anvil_type_i64(ctx);
    anvil_type_t *fields[] = {i8, i32};
    anvil_type_t *objects[] = {anvil_type_literal_struct(ctx, fields, 2, true), i64, anvil_type_vector(ctx, i32, 4)};
    anvil_type_t *params[] = {i64};
    anvil_func_t *func = anvil_func_create(module, "frame", anvil_type_func(ctx, anvil_type_void(ctx), params, 1, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, func->entry);
    for (size_t o = 0; o < 3; o++)
        anvil_build_alloca(ctx, objects[o], "object");
    anvil_build_alloca_dyn(ctx, i8, anvil_func_get_param(func, 0), "bytes");
    anvil_build_ret_void(ctx);

    char message[160];
    anvil_mir_func_t *mir = lower(target->arch, func);
    snprintf(message, sizeof(message), "arch %d: the function lowers", (int)target->arch);
    CHECK(mir != NULL, message);
    if (mir) {
        snprintf(message, sizeof(message), "arch %d: one frame slot per static ALLOCA", (int)target->arch);
        CHECK(anvil_mir_num_frame_slots(mir) == 3, message);
        for (size_t o = 0; o < 3 && o < anvil_mir_num_frame_slots(mir); o++) {
            anvil_mir_frame_slot_info_t slot;
            size_t align = anvil_type_align(objects[o]) < target->stack_alignment ? anvil_type_align(objects[o]) : target->stack_alignment;
            bool found = anvil_mir_get_frame_slot_info(mir, (int)o, &slot);
            snprintf(message, sizeof(message), "arch %d object %zu: slot of %zu bytes aligned to %zu (got %zu, %u)", (int)target->arch, o,
                     anvil_type_size(objects[o]), align, found ? slot.size_bytes : 0, found ? slot.align_bytes : 0);
            CHECK(found && slot.size_bytes == anvil_type_size(objects[o]) && slot.align_bytes == align, message);
        }
        snprintf(message, sizeof(message), "arch %d: the counted ALLOCA is a dynamic allocation", (int)target->arch);
        CHECK(has_op(mir, ANVIL_MIR_OP_DYN_ALLOCA), message);
        anvil_mir_func_destroy(mir);
    }

    char *assembly = NULL;
    size_t length = 0;
    snprintf(message, sizeof(message), "arch %d: the function compiles: %s", (int)target->arch, "");
    bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
    if (!built)
        snprintf(message, sizeof(message), "arch %d: the function compiles: %s", (int)target->arch, anvil_ctx_get_error(ctx));
    CHECK(built, message);
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

/* A packed struct global takes no alignment directive. */
static void test_packed_global(anvil_arch_t arch)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "globals");
    anvil_type_t *fields[] = {anvil_type_i8(ctx), anvil_type_i32(ctx)};
    anvil_module_add_global(module, "wide", anvil_type_i64(ctx), ANVIL_LINK_EXTERNAL);
    anvil_module_add_global(module, "packed", anvil_type_literal_struct(ctx, fields, 2, true), ANVIL_LINK_EXTERNAL);
    char *assembly = NULL;
    size_t length = 0;
    char message[128];
    bool built = anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && assembly;
    const char *start = built ? strstr(assembly, "\t.globl packed\n") : NULL;
    const char *label = start ? strstr(start, "packed:") : NULL;
    const char *align = start ? strstr(start, ".p2align") : NULL;
    snprintf(message, sizeof(message), "arch %d: a packed struct global is byte aligned", (int)arch);
    CHECK(label && (!align || align > label), message);
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++)
        test_frame_objects(&targets[t]);
    test_packed_global(ANVIL_ARCH_X86_64);
    test_packed_global(ANVIL_ARCH_X86);
    test_packed_global(ANVIL_ARCH_ARM64);
    test_packed_global(ANVIL_ARCH_PPC64LE);
    if (failures) {
        fprintf(stderr, "%d frame object regression check(s) failed\n", failures);
        return 1;
    }
    printf("frame object regression tests passed\n");
    return 0;
}
