/*
 * ARM64 Linux (AAPCS64/ELF) and Darwin (Apple AAPCS64/Mach-O) argument
 * passing, checked on the lowered MachineIR against what clang produces for
 * each platform, plus cross-assembly of complete modules.
 */

#include <anvil/anvil.h>
#include <anvil/anvil_arm64_mir.h>
#include <anvil/anvil_machine.h>

#include "platform/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s\n", msg); \
        failures++; \
    } \
} while (0)

typedef struct {
    anvil_abi_t abi;
    const char *name;
    const char *assembler_target;
    int64_t spill_offsets[5];   /* char, short, int, char, long after x0-x7 */
    bool extends_small_ints;
    bool unnamed_on_stack;
    bool va_list_is_pointer;
} abi_case_t;

static const abi_case_t cases[] = {
    { ANVIL_ABI_SYSV, "linux", "aarch64-linux-gnu", { 0, 8, 16, 24, 32 }, false, false, false },
    { ANVIL_ABI_DARWIN, "darwin", "arm64-apple-macos13", { 0, 2, 4, 8, 16 }, true, true, true },
};

static anvil_ctx_t *new_ctx(anvil_abi_t abi)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx && anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64) == ANVIL_OK &&
          anvil_ctx_set_abi(ctx, abi) == ANVIL_OK, "ARM64 context with the ABI should be created");
    return ctx;
}

/* (long x8, char, short, int, char, long) as clang's callee(...) */
static anvil_type_t *spill_type(anvil_ctx_t *ctx, bool variadic)
{
    anvil_type_t *params[13];
    for (int i = 0; i < 8; i++) params[i] = anvil_type_i64(ctx);
    params[8] = anvil_type_i8(ctx);
    params[9] = anvil_type_i16(ctx);
    params[10] = anvil_type_i32(ctx);
    params[11] = anvil_type_i8(ctx);
    params[12] = anvil_type_i64(ctx);
    return anvil_type_func(ctx, anvil_type_i64(ctx), params, 13, variadic);
}

static size_t collect_imms(const anvil_mir_func_t *mir, anvil_mir_opcode_t op, int64_t *out, size_t max)
{
    size_t count = 0;
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (anvil_mir_get_instr_info(mir, i, &info) && info.op == op && count < max)
            out[count++] = info.imm;
    }
    return count;
}

static bool find_call(const anvil_mir_func_t *mir, anvil_mir_instr_info_t *call, size_t *index)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++)
        if (anvil_mir_get_instr_info(mir, i, call) && call->op == ANVIL_MIR_OP_CALL) {
            *index = i;
            return true;
        }
    return false;
}

static void test_named_stack_arguments(const abi_case_t *c)
{
    anvil_ctx_t *ctx = new_ctx(c->abi);
    anvil_module_t *mod = anvil_module_create(ctx, "spill");
    anvil_func_t *callee = anvil_func_create(mod, "callee", spill_type(ctx, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(callee));
    anvil_build_ret(ctx, anvil_func_get_param(callee, 12));

    anvil_func_t *ext = anvil_func_declare(mod, "ext", spill_type(ctx, false));
    anvil_type_t *caller_type = anvil_type_func(ctx, anvil_type_i64(ctx), NULL, 0, false);
    anvil_func_t *caller = anvil_func_create(mod, "caller", caller_type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
    anvil_value_t *args[13];
    for (int i = 0; i < 8; i++) args[i] = anvil_const_i64(ctx, i + 1);
    args[8] = anvil_const_i8(ctx, 'x');
    args[9] = anvil_const_i16(ctx, 300);
    args[10] = anvil_const_i32(ctx, 7);
    args[11] = anvil_const_i8(ctx, 'y');
    args[12] = anvil_const_i64(ctx, 9);
    anvil_value_t *result = NULL;
    CHECK(anvil_build_call_checked(ctx, anvil_func_get_value(ext), args, 13, "r", &result) &&
          anvil_build_ret(ctx, result), "stack-argument call should build");

    int64_t imms[8];
    anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(callee);
    size_t count = mir ? collect_imms(mir, ANVIL_MIR_OP_INCOMING_STACK_ARG, imms, 8) : 0;
    CHECK(count == 5, "callee should read five stack arguments");
    for (size_t i = 0; i < count && i < 5; i++)
        CHECK(imms[i] == c->spill_offsets[i], "incoming stack argument offsets should follow the platform ABI");
    anvil_mir_func_destroy(mir);

    mir = anvil_arm64_lower_func_to_mir(caller);
    count = mir ? collect_imms(mir, ANVIL_MIR_OP_CALL_STACK_ARG, imms, 8) : 0;
    CHECK(count == 5, "caller should store five stack arguments");
    for (size_t i = 0; i < count && i < 5; i++)
        CHECK(imms[i] == c->spill_offsets[i], "outgoing stack argument offsets should follow the platform ABI");
    CHECK(mir && anvil_arm64_regalloc_mir(mir), "packed stack arguments should be legal and allocate");
    anvil_mir_func_destroy(mir);

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_small_integer_extension(const abi_case_t *c)
{
    anvil_ctx_t *ctx = new_ctx(c->abi);
    anvil_module_t *mod = anvil_module_create(ctx, "small");
    anvil_type_t *params[] = { anvil_type_i8(ctx), anvil_type_u16(ctx) };
    anvil_func_t *take = anvil_func_declare(mod, "take", anvil_type_func(ctx, anvil_type_i32(ctx), params, 2, false));
    anvil_func_t *caller = anvil_func_create(mod, "use_small", anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 0, false),
                                             ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
    anvil_value_t *args[] = { anvil_const_i8(ctx, -1), anvil_const_u16(ctx, 65535) };
    anvil_value_t *result = NULL;
    CHECK(anvil_build_call_checked(ctx, anvil_func_get_value(take), args, 2, "r", &result) &&
          anvil_build_ret(ctx, result), "small-integer call should build");

    anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(caller);
    anvil_mir_instr_info_t call;
    size_t index = 0;
    CHECK(mir && find_call(mir, &call, &index) && call.num_uses == 2, "small-integer call should pass x0 and x1");
    for (size_t u = 0; mir && u < call.num_uses; u++) {
        const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, u));
        CHECK(info && (info->size_bits == 32) == c->extends_small_ints,
              "8/16-bit arguments are extended to 32 bits exactly when the ABI requires it");
    }
    anvil_mir_func_destroy(mir);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_variadic_call_and_va_start(const abi_case_t *c)
{
    anvil_ctx_t *ctx = new_ctx(c->abi);
    anvil_module_t *mod = anvil_module_create(ctx, "variadic");
    anvil_type_t *named[] = { anvil_type_i32(ctx) };
    anvil_type_t *vf_type = anvil_type_func(ctx, anvil_type_i32(ctx), named, 1, true);
    anvil_func_t *vf = anvil_func_declare(mod, "vf", vf_type);
    anvil_func_t *caller = anvil_func_create(mod, "call_v", anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 0, false),
                                             ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
    anvil_value_t *args[] = { anvil_const_i32(ctx, 1), anvil_const_i64(ctx, 2), anvil_const_f64(ctx, 3.0),
                              anvil_const_i64(ctx, 99) };
    anvil_value_t *result = NULL;
    CHECK(anvil_build_call_checked(ctx, anvil_func_get_value(vf), args, 4, "r", &result) &&
          anvil_build_ret(ctx, result), "variadic call should build");

    anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(caller);
    int64_t imms[8];
    size_t count = mir ? collect_imms(mir, ANVIL_MIR_OP_CALL_STACK_ARG, imms, 8) : 0;
    anvil_mir_instr_info_t call;
    size_t index = 0;
    CHECK(mir && find_call(mir, &call, &index), "variadic call should lower");
    if (c->unnamed_on_stack) {
        CHECK(count == 3 && imms[0] == 0 && imms[1] == 8 && imms[2] == 16 && call.num_uses == 1,
              "Darwin passes unnamed arguments in doubleword stack slots only");
    } else {
        CHECK(count == 0 && call.num_uses == 4, "AAPCS64 passes unnamed arguments like named ones");
    }
    anvil_mir_func_destroy(mir);

    /* int g(int n, ...) { va_list ap; va_start(ap, n); ... } */
    anvil_func_t *g = anvil_func_create(mod, "g", vf_type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(g));
    anvil_value_t *cursor = anvil_build_va_start(ctx, "ap");
    anvil_value_t *storage = anvil_build_alloca(ctx, anvil_type_ptr(ctx, anvil_type_i8(ctx)), "ap.storage");
    anvil_build_store(ctx, cursor, storage);
    anvil_value_t *slot = anvil_build_va_arg(ctx, storage, anvil_type_i64(ctx), "arg");
    anvil_value_t *value = slot ? anvil_build_load(ctx, anvil_type_i64(ctx), slot, "value") : NULL;
    CHECK(value && anvil_build_ret(ctx, anvil_build_trunc(ctx, value, anvil_type_i32(ctx), "narrow")),
          "va_start / va_arg should build through the ABI");

    mir = anvil_arm64_lower_func_to_mir(g);
    bool saw = false;
    for (size_t i = 0; mir && i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info) || info.op != ANVIL_MIR_OP_VA_START)
            continue;
        saw = true;
        if (c->va_list_is_pointer)
            CHECK(info.frame_slot < 0 && info.has_imm && info.imm == 0, "Darwin va_start points at the first unnamed slot");
        else
            CHECK(info.frame_slot >= 0 && info.named_gpr == 1 && info.named_fpr == 0,
                  "AAPCS64 va_start builds the register save cursor");
    }
    CHECK(saw, "va_start should reach MachineIR");
    anvil_mir_func_destroy(mir);

    /* The whole module assembles for the platform. */
    char *text = NULL;
    size_t len = 0;
    CHECK(anvil_module_codegen(mod, &text, &len) == ANVIL_OK && text, "variadic module should generate code");
    if (text) {
        CHECK(anvil_test_host.cross_assemble(text, c->assembler_target) <= 0, "ARM64 module should assemble for its platform");
        if (c->va_list_is_pointer)
            CHECK(strstr(text, "_g:") && strstr(text, "@PAGE") == NULL, "Darwin functions use Mach-O symbols");
        else
            CHECK(strstr(text, ".type g, %function"), "Linux functions use ELF symbol types");
    }
    free(text);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        test_named_stack_arguments(&cases[i]);
        test_small_integer_extension(&cases[i]);
        test_variadic_call_and_va_start(&cases[i]);
    }
    if (failures) {
        fprintf(stderr, "%d ARM64 ABI regression check(s) failed\n", failures);
        return 1;
    }
    printf("ARM64 ABI regression checks passed (Linux, Darwin)\n");
    return 0;
}
