/*
 * Regression tests for core target setup and ARM64 code generation.
 */

#include <anvil/anvil.h>
#include <anvil/anvil_internal.h>

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

static char *codegen_or_fail(anvil_module_t *mod)
{
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    (void)len;
    if (err != ANVIL_OK || !output) {
        const char *detail = (mod && mod->ctx) ? anvil_ctx_get_error(mod->ctx) : NULL;
        fprintf(stderr, "[FAIL] codegen failed%s%s\n",
                detail && detail[0] ? ": " : "",
                detail && detail[0] ? detail : "");
        failures++;
        return NULL;
    }
    return output;
}

static bool contains_after(const char *text, const char *first, const char *second)
{
    const char *pos = text ? strstr(text, first) : NULL;
    if (!pos) return false;
    return strstr(pos + strlen(first), second) != NULL;
}

static anvil_ctx_t *new_arm64_ctx(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx != NULL, "context should be created");
    if (!ctx) return NULL;

    CHECK(anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64) == ANVIL_OK,
          "ARM64 target should be available");
    CHECK(anvil_ctx_set_abi(ctx, ANVIL_ABI_DARWIN) == ANVIL_OK,
          "ARM64 Darwin ABI should be selectable");
    return ctx;
}

static anvil_ctx_t *new_arm64_linux_ctx(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx != NULL, "context should be created");
    if (!ctx) return NULL;

    CHECK(anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64) == ANVIL_OK,
          "ARM64 target should be available");
    CHECK(anvil_ctx_set_abi(ctx, ANVIL_ABI_SYSV) == ANVIL_OK,
          "ARM64 SysV ABI should be selectable");
    return ctx;
}

static void test_pointer_cache_tracks_target_changes(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx != NULL, "context should be created for pointer cache test");
    if (!ctx) return;

    anvil_type_t *i8_ptr = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *void_ptr = anvil_type_ptr(ctx, anvil_type_void(ctx));
    CHECK(anvil_type_size(i8_ptr) == 8, "default i8* should be 8 bytes");
    CHECK(anvil_type_size(void_ptr) == 8, "default void* should be 8 bytes");

    CHECK(anvil_ctx_set_target(ctx, ANVIL_ARCH_X86) == ANVIL_OK,
          "x86 target should be available");
    i8_ptr = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    void_ptr = anvil_type_ptr(ctx, anvil_type_void(ctx));
    CHECK(anvil_type_size(i8_ptr) == 4, "i8* should update to 4 bytes on x86");
    CHECK(anvil_type_align(i8_ptr) == 4, "i8* align should update to 4 bytes on x86");
    CHECK(anvil_type_size(void_ptr) == 4, "void* should update to 4 bytes on x86");
    CHECK(anvil_type_align(void_ptr) == 4, "void* align should update to 4 bytes on x86");

    CHECK(anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64) == ANVIL_OK,
          "ARM64 target should be available after x86");
    i8_ptr = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    void_ptr = anvil_type_ptr(ctx, anvil_type_void(ctx));
    CHECK(anvil_type_size(i8_ptr) == 8, "i8* should update back to 8 bytes on ARM64");
    CHECK(anvil_type_size(void_ptr) == 8, "void* should update back to 8 bytes on ARM64");

    anvil_ctx_destroy(ctx);
}

static void test_arm64_routes_fp_instruction_results_through_mir(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "fp_spill_regression");
    CHECK(mod != NULL, "module should be created for FP spill test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *params[] = { f64, f64, f64, f64 };
    anvil_type_t *fn_type = anvil_type_func(ctx, f64, params, 4, false);
    anvil_func_t *fn = anvil_func_create(mod, "fp_two_live", fn_type, ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "function should be created for FP spill test");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *a = anvil_func_get_param(fn, 0);
        anvil_value_t *b = anvil_func_get_param(fn, 1);
        anvil_value_t *c = anvil_func_get_param(fn, 2);
        anvil_value_t *d = anvil_func_get_param(fn, 3);
        anvil_value_t *left = anvil_build_fadd(ctx, a, b, "left");
        anvil_value_t *right = anvil_build_fadd(ctx, c, d, "right");
        anvil_value_t *product = anvil_build_fmul(ctx, left, right, "product");
        anvil_build_ret(ctx, product);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lfp_two_live_entry:\n") != NULL,
              "supported ARM64 FP function should be emitted through MIR");
        CHECK(strstr(asm_text, "\tfadd ") != NULL,
              "ARM64 MIR FP function should emit fadd");
        CHECK(strstr(asm_text, "\tfmul ") != NULL,
              "ARM64 MIR FP function should emit fmul");
        CHECK(strstr(asm_text, "\tstr d") != NULL &&
              strstr(asm_text, "\tldr d") != NULL,
              "ARM64 MIR FP function should preserve used callee-saved FPRs");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_dynamic_alloca_restores_sp_from_frame_pointer(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "dyn_alloca_regression");
    CHECK(mod != NULL, "module should be created for dynamic alloca test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *params[] = { i64 };
    anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 1, false);
    anvil_func_t *fn = anvil_func_create(mod, "dyn_alloca_probe", fn_type, ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "function should be created for dynamic alloca test");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *count = anvil_func_get_param(fn, 0);
        anvil_value_t *ptr = anvil_build_alloca_dyn(ctx, i64, count, "items");
        anvil_build_store(ctx, anvil_const_i64(ctx, 42), ptr);
        anvil_value_t *loaded = anvil_build_load(ctx, i64, ptr, "loaded");
        anvil_build_ret(ctx, loaded);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Ldyn_alloca_probe_entry:\n") != NULL,
              "dynamic alloca should be emitted through MIR");
        CHECK(strstr(asm_text, "\tsub sp, sp, x") != NULL,
              "dynamic alloca should subtract runtime size from sp");
        CHECK(strstr(asm_text, "\tmov sp, x29\n") != NULL,
              "ARM64 epilogue should restore sp from x29 after dynamic alloca");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_emits_stack_arguments_after_x7(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "stack_args_regression");
    CHECK(mod != NULL, "module should be created for stack args test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *callee_params[10];
    for (size_t i = 0; i < 10; i++) {
        callee_params[i] = i64;
    }
    anvil_type_t *callee_type = anvil_type_func(ctx, i64, callee_params, 10, false);
    anvil_value_t *callee = anvil_module_add_extern(mod, "callee10", callee_type);
    CHECK(callee != NULL, "external callee should be created");

    anvil_type_t *caller_type = anvil_type_func(ctx, i64, NULL, 0, false);
    anvil_func_t *caller = anvil_func_create(mod, "call10", caller_type, ANVIL_LINK_EXTERNAL);
    CHECK(caller != NULL, "caller should be created for stack args test");
    if (caller && callee) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
        anvil_value_t *args[10];
        for (size_t i = 0; i < 10; i++) {
            args[i] = anvil_const_i64(ctx, (int64_t)i + 1);
        }
        anvil_value_t *result = anvil_build_call(ctx, i64, callee, args, 10, "result");
        anvil_build_ret(ctx, result);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lcall10_entry:\n") != NULL,
              "ARM64 stack-argument call should be emitted through MIR");
        CHECK(strstr(asm_text, "[sp, #0]\n") != NULL,
              "ARM64 should store argument 8 at the outgoing stack area");
        CHECK(strstr(asm_text, "[sp, #8]\n") != NULL,
              "ARM64 should store argument 9 at the outgoing stack area");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_fp_call_uses_fp_registers_and_spills_d0_result(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "fp_call_regression");
    CHECK(mod != NULL, "module should be created for FP call test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *callee_params[] = { f64 };
    anvil_type_t *callee_type = anvil_type_func(ctx, f64, callee_params, 1, false);
    anvil_value_t *callee = anvil_module_add_extern(mod, "fp_ext", callee_type);
    CHECK(callee != NULL, "FP external callee should be created");

    anvil_type_t *caller_params[] = { f64 };
    anvil_type_t *caller_type = anvil_type_func(ctx, f64, caller_params, 1, false);
    anvil_func_t *caller = anvil_func_create(mod, "fp_call", caller_type, ANVIL_LINK_EXTERNAL);
    CHECK(caller != NULL, "FP caller should be created");
    if (caller && callee) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
        anvil_value_t *arg = anvil_func_get_param(caller, 0);
        anvil_value_t *args[] = { arg };
        anvil_value_t *called = anvil_build_call(ctx, f64, callee, args, 1, "called");
        anvil_value_t *sum = anvil_build_fadd(ctx, called, arg, "sum");
        anvil_build_ret(ctx, sum);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lfp_call_entry:\n") != NULL,
              "ARM64 FP call should be emitted through MIR");
        CHECK(strstr(asm_text, "\tfmov d0,") != NULL,
              "ARM64 MIR should copy f64 call arguments into d0");
        CHECK(strstr(asm_text, "\tbl _fp_ext\n\tfmov d") != NULL,
              "ARM64 MIR should copy f64 call results out of d0 immediately after the call");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_module_codegen_routes_supported_leaf_function_through_mir(void)
{
    anvil_ctx_t *ctx = new_arm64_linux_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_codegen_regression");
    CHECK(mod != NULL, "module should be created for MIR codegen routing test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *params[] = { i64, i64 };
    anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 2, false);
    anvil_func_t *fn = anvil_func_create(mod, "mir_leaf_add", fn_type,
                                         ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "MIR-routable leaf function should be created");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *a = anvil_func_get_param(fn, 0);
        anvil_value_t *b = anvil_func_get_param(fn, 1);
        anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
        anvil_build_ret(ctx, sum);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lmir_leaf_add_entry:\n") != NULL,
              "supported ARM64 function should be emitted through the MIR path");
        CHECK(strstr(asm_text, "\tmov x19, x0\n") != NULL ||
              strstr(asm_text, "\tmov x20, x0\n") != NULL ||
              strstr(asm_text, "\tmov x21, x0\n") != NULL,
              "MIR codegen should copy incoming x0 into an allocatable callee-saved register");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_module_codegen_routes_register_call_function_through_mir(void)
{
    anvil_ctx_t *ctx = new_arm64_linux_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_call_codegen_regression");
    CHECK(mod != NULL, "module should be created for MIR call routing test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *callee_params[] = { i64 };
    anvil_type_t *callee_type = anvil_type_func(ctx, i64, callee_params, 1, false);
    anvil_value_t *callee = anvil_module_add_extern(mod, "mir_callee1", callee_type);
    CHECK(callee != NULL, "external MIR callee should be created");

    anvil_type_t *caller_type = anvil_type_func(ctx, i64, callee_params, 1, false);
    anvil_func_t *caller = anvil_func_create(mod, "mir_call_preserves_arg",
                                             caller_type, ANVIL_LINK_EXTERNAL);
    CHECK(caller != NULL, "MIR call caller should be created");
    if (caller && callee) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
        anvil_value_t *x = anvil_func_get_param(caller, 0);
        anvil_value_t *args[] = { x };
        anvil_value_t *called = anvil_build_call(ctx, i64, callee, args, 1, "called");
        anvil_value_t *sum = anvil_build_add(ctx, called, x, "sum");
        anvil_build_ret(ctx, sum);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lmir_call_preserves_arg_entry:\n") != NULL,
              "register-only ARM64 call function should be emitted through MIR");
        CHECK(strstr(asm_text, "\tbl mir_callee1\n\tmov x") != NULL,
              "MIR call result should be copied out of x0 immediately after bl");
        CHECK(contains_after(asm_text, "\tbl mir_callee1\n", "\tadd "),
              "MIR call caller should still use the preserved original argument after call");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_module_codegen_routes_cfg_phi_function_through_mir(void)
{
    anvil_ctx_t *ctx = new_arm64_linux_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_phi_codegen_regression");
    CHECK(mod != NULL, "module should be created for MIR CFG/PHI routing test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *params[] = { i64 };
    anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 1, false);
    anvil_func_t *fn = anvil_func_create(mod, "mir_phi_codegen", fn_type,
                                         ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "MIR CFG/PHI function should be created");
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *then_block = anvil_block_create(fn, "then");
        anvil_block_t *else_block = anvil_block_create(fn, "else");
        anvil_block_t *merge_block = anvil_block_create(fn, "merge");
        CHECK(entry && then_block && else_block && merge_block,
              "MIR CFG/PHI blocks should be created");

        anvil_set_insert_point(ctx, entry);
        anvil_value_t *x = anvil_func_get_param(fn, 0);
        anvil_value_t *cond = anvil_build_cmp_gt(ctx, x, anvil_const_i64(ctx, 0),
                                                 "is_pos");
        anvil_build_br_cond(ctx, cond, then_block, else_block);

        anvil_set_insert_point(ctx, then_block);
        anvil_build_br(ctx, merge_block);

        anvil_set_insert_point(ctx, else_block);
        anvil_build_br(ctx, merge_block);

        anvil_set_insert_point(ctx, merge_block);
        anvil_value_t *phi = anvil_build_phi(ctx, i64, "selected");
        anvil_phi_add_incoming(phi, anvil_const_i64(ctx, 11), then_block);
        anvil_phi_add_incoming(phi, anvil_const_i64(ctx, 22), else_block);
        anvil_build_ret(ctx, phi);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lmir_phi_codegen_entry:\n") != NULL,
              "CFG/PHI ARM64 function should be emitted through MIR");
        CHECK(strstr(asm_text, ".Lmir_phi_codegen_then:\n") != NULL,
              "MIR CFG codegen should emit then label");
        CHECK(strstr(asm_text, ".Lmir_phi_codegen_else:\n") != NULL,
              "MIR CFG codegen should emit else label");
        CHECK(strstr(asm_text, ".Lmir_phi_codegen_merge:\n") != NULL,
              "MIR CFG codegen should emit merge label");
        CHECK(strstr(asm_text, "\tcbnz ") != NULL,
              "MIR CFG codegen should emit conditional branch");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_module_codegen_routes_stack_argument_calls_through_mir(void)
{
    anvil_ctx_t *ctx = new_arm64_linux_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_stack_arg_codegen");
    CHECK(mod != NULL, "module should be created for stack-arg MIR codegen test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *callee_params[10];
    for (size_t i = 0; i < 10; i++) {
        callee_params[i] = i64;
    }
    anvil_type_t *callee_type = anvil_type_func(ctx, i64, callee_params, 10, false);
    anvil_value_t *callee = anvil_module_add_extern(mod, "mir_callee10", callee_type);
    CHECK(callee != NULL, "external stack-arg MIR callee should be created");

    anvil_type_t *caller_type = anvil_type_func(ctx, i64, NULL, 0, false);
    anvil_func_t *caller = anvil_func_create(mod, "mir_stack_arg_call",
                                             caller_type, ANVIL_LINK_EXTERNAL);
    CHECK(caller != NULL, "stack-arg MIR caller should be created");
    if (caller && callee) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
        anvil_value_t *args[10];
        for (size_t i = 0; i < 10; i++) {
            args[i] = anvil_const_i64(ctx, (int64_t)i + 1);
        }
        anvil_value_t *result = anvil_build_call(ctx, i64, callee, args, 10, "result");
        anvil_build_ret(ctx, result);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lmir_stack_arg_call_entry:\n") != NULL,
              "stack-argument ARM64 call should be emitted through MIR");
        CHECK(strstr(asm_text, "\tstr x") != NULL &&
              strstr(asm_text, "[sp, #0]\n") != NULL &&
              strstr(asm_text, "[sp, #8]\n") != NULL,
              "MIR stack-argument call should store outgoing args in the call frame");
        CHECK(strstr(asm_text, "\tbl mir_callee10\n") != NULL,
              "MIR stack-argument call should emit the direct call");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_module_codegen_loads_incoming_stack_arguments_through_mir(void)
{
    anvil_ctx_t *ctx = new_arm64_linux_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_incoming_stack_args");
    CHECK(mod != NULL, "module should be created for incoming stack-arg MIR test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *i64_params[10];
    for (size_t i = 0; i < 10; i++) {
        i64_params[i] = i64;
    }
    anvil_type_t *sum_type = anvil_type_func(ctx, i64, i64_params, 10, false);
    anvil_func_t *sum_fn = anvil_func_create(mod, "mir_incoming_stack_sum",
                                             sum_type, ANVIL_LINK_EXTERNAL);
    CHECK(sum_fn != NULL, "incoming stack integer function should be created");
    if (sum_fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(sum_fn));
        anvil_value_t *a8 = anvil_func_get_param(sum_fn, 8);
        anvil_value_t *a9 = anvil_func_get_param(sum_fn, 9);
        anvil_value_t *sum = anvil_build_add(ctx, a8, a9, "sum");
        anvil_build_ret(ctx, sum);
    }

    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *f64_params[9];
    for (size_t i = 0; i < 9; i++) {
        f64_params[i] = f64;
    }
    anvil_type_t *fp_type = anvil_type_func(ctx, f64, f64_params, 9, false);
    anvil_func_t *fp_fn = anvil_func_create(mod, "mir_incoming_stack_fp",
                                            fp_type, ANVIL_LINK_EXTERNAL);
    CHECK(fp_fn != NULL, "incoming stack FP function should be created");
    if (fp_fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fp_fn));
        anvil_value_t *a0 = anvil_func_get_param(fp_fn, 0);
        anvil_value_t *a8 = anvil_func_get_param(fp_fn, 8);
        anvil_value_t *sum = anvil_build_fadd(ctx, a0, a8, "sum");
        anvil_build_ret(ctx, sum);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lmir_incoming_stack_sum_entry:\n") != NULL,
              "incoming stack integer function should be emitted through MIR");
        CHECK(strstr(asm_text, "\tldr x") != NULL &&
              strstr(asm_text, "[x29, #16]\n") != NULL &&
              strstr(asm_text, "[x29, #24]\n") != NULL,
              "incoming integer stack args should load from x29-positive ABI slots");
        CHECK(strstr(asm_text, ".Lmir_incoming_stack_fp_entry:\n") != NULL,
              "incoming stack FP function should be emitted through MIR");
        CHECK(strstr(asm_text, "\tldr d") != NULL &&
              strstr(asm_text, "[x29, #16]\n") != NULL,
              "incoming FP stack args should load from x29-positive ABI slots");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_module_codegen_routes_memory_cast_select_function_through_mir(void)
{
    anvil_ctx_t *ctx = new_arm64_linux_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_memory_codegen");
    CHECK(mod != NULL, "module should be created for memory/cast/select MIR test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i8 = anvil_type_i8(ctx);
    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *array_i64_4 = anvil_type_array(ctx, i64, 4);
    anvil_type_t *params[] = { i64, i64, i64, i8, f64 };
    anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 5, false);
    anvil_func_t *fn = anvil_func_create(mod, "mir_memory_cast_select",
                                         fn_type, ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "memory/cast/select function should be created");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *idx = anvil_func_get_param(fn, 0);
        anvil_value_t *a = anvil_func_get_param(fn, 1);
        anvil_value_t *b = anvil_func_get_param(fn, 2);
        anvil_value_t *tiny = anvil_func_get_param(fn, 3);
        anvil_value_t *fp = anvil_func_get_param(fn, 4);
        anvil_value_t *items = anvil_build_alloca(ctx, array_i64_4, "items");
        anvil_value_t *indices[] = { idx };
        anvil_value_t *slot = anvil_build_gep(ctx, i64, items, indices, 1, "slot");
        anvil_build_store(ctx, a, slot);
        anvil_value_t *loaded = anvil_build_load(ctx, i64, slot, "loaded");
        anvil_value_t *cond = anvil_build_cmp_gt(ctx, loaded, b, "gt");
        anvil_value_t *picked = anvil_build_select(ctx, cond, loaded, b, "picked");
        anvil_value_t *signed_wide = anvil_build_sext(ctx, tiny, i64, "signed_wide");
        anvil_value_t *unsigned_wide = anvil_build_zext(ctx, tiny, i64, "unsigned_wide");
        anvil_value_t *abs_fp = anvil_build_fabs(ctx, fp, "abs_fp");
        anvil_value_t *fp_int = anvil_build_fptosi(ctx, abs_fp, i64, "fp_int");
        anvil_value_t *acc0 = anvil_build_add(ctx, picked, signed_wide, "acc0");
        anvil_value_t *acc1 = anvil_build_add(ctx, acc0, unsigned_wide, "acc1");
        anvil_value_t *acc2 = anvil_build_add(ctx, acc1, fp_int, "acc2");
        anvil_build_ret(ctx, acc2);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lmir_memory_cast_select_entry:\n") != NULL,
              "memory/cast/select ARM64 function should be emitted through MIR");
        CHECK(strstr(asm_text, "\tmul ") != NULL,
              "GEP lowering should scale the runtime index before pointer addition");
        CHECK(strstr(asm_text, "\tstr x") != NULL &&
              strstr(asm_text, "\tldr x") != NULL,
              "MIR memory lowering should emit stack-backed stores and loads");
        CHECK(strstr(asm_text, "\tcsel ") != NULL,
              "MIR select lowering should emit conditional select");
        CHECK(strstr(asm_text, "\tsxtb ") != NULL &&
              strstr(asm_text, "\tuxtb ") != NULL,
              "MIR cast lowering should emit byte sign and zero extensions");
        CHECK(strstr(asm_text, "\tfabs ") != NULL &&
              strstr(asm_text, "\tfcvtzs ") != NULL,
              "MIR FP cast lowering should emit fabs and fptosi conversion");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_module_codegen_folds_constant_gep_into_load_store_offsets(void)
{
    anvil_ctx_t *ctx = new_arm64_linux_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_offset_memory_codegen");
    CHECK(mod != NULL, "module should be created for offset memory MIR test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *array_i64_4 = anvil_type_array(ctx, i64, 4);
    anvil_type_t *fn_type = anvil_type_func(ctx, i64, NULL, 0, false);
    anvil_func_t *fn = anvil_func_create(mod, "mir_const_gep_offsets",
                                         fn_type, ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "constant GEP offset function should be created");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *items = anvil_build_alloca(ctx, array_i64_4, "items");
        anvil_value_t *indices[] = { anvil_const_i64(ctx, 2) };
        anvil_value_t *slot = anvil_build_gep(ctx, i64, items, indices, 1, "slot");
        anvil_build_store(ctx, anvil_const_i64(ctx, 88), slot);
        anvil_value_t *loaded = anvil_build_load(ctx, i64, slot, "loaded");
        anvil_build_ret(ctx, loaded);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lmir_const_gep_offsets_entry:\n") != NULL,
              "constant GEP offset function should be emitted through MIR");
        CHECK(strstr(asm_text, "\tmul ") == NULL,
              "constant GEP offset should not emit runtime scaling");
        CHECK(strstr(asm_text, "\tstr x") != NULL &&
              strstr(asm_text, ", #16]\n") != NULL,
              "constant GEP store should use an immediate memory offset");
        CHECK(strstr(asm_text, "\tldr x") != NULL &&
              strstr(asm_text, ", #16]\n") != NULL,
              "constant GEP load should use an immediate memory offset");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_module_codegen_materializes_global_and_string_addresses(void)
{
    anvil_ctx_t *ctx = new_arm64_linux_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_global_string_codegen");
    CHECK(mod != NULL, "module should be created for global/string MIR test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_value_t *global = anvil_module_add_global(mod, "mir_global_seed",
                                                    i64, ANVIL_LINK_EXTERNAL);
    CHECK(global != NULL, "global variable should be created");
    if (global) {
        anvil_global_set_initializer(global, anvil_const_i64(ctx, 41));
    }

    anvil_type_t *global_params[] = { i64 };
    anvil_type_t *global_fn_type = anvil_type_func(ctx, i64, global_params, 1, false);
    anvil_func_t *global_fn = anvil_func_create(mod, "mir_global_plus",
                                                global_fn_type,
                                                ANVIL_LINK_EXTERNAL);
    CHECK(global_fn != NULL, "global load function should be created");
    if (global_fn && global) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(global_fn));
        anvil_value_t *x = anvil_func_get_param(global_fn, 0);
        anvil_value_t *loaded = anvil_build_load(ctx, i64, global, "loaded");
        anvil_value_t *sum = anvil_build_add(ctx, loaded, x, "sum");
        anvil_build_ret(ctx, sum);
    }

    anvil_type_t *i8_ptr = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *string_fn_type = anvil_type_func(ctx, i8_ptr, NULL, 0, false);
    anvil_func_t *string_fn = anvil_func_create(mod, "mir_const_string",
                                                string_fn_type,
                                                ANVIL_LINK_EXTERNAL);
    CHECK(string_fn != NULL, "string literal function should be created");
    if (string_fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(string_fn));
        anvil_build_ret(ctx, anvil_const_string(ctx, "anvil-mir"));
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, "\tadrp ") != NULL &&
              strstr(asm_text, "mir_global_seed") != NULL &&
              strstr(asm_text, ":lo12:mir_global_seed") != NULL,
              "ARM64 MIR should materialize global addresses with PC-relative adrp/add");
        CHECK(strstr(asm_text, "mir_global_seed:\n") != NULL &&
              strstr(asm_text, "\t.quad 41\n") != NULL,
              "ARM64 module codegen should still emit global data");
        CHECK(strstr(asm_text, ".Lstr_mir_const_string_0:\n") != NULL &&
              strstr(asm_text, "\t.asciz \"anvil-mir\"\n") != NULL,
              "ARM64 MIR should emit string literal data for const string operands");
        CHECK(strstr(asm_text, ":lo12:.Lstr_mir_const_string_0") != NULL,
              "ARM64 MIR should materialize string literal addresses");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_module_codegen_uses_signed_and_unsigned_byte_loads(void)
{
    anvil_ctx_t *ctx = new_arm64_linux_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_signed_load_codegen");
    CHECK(mod != NULL, "module should be created for signed load MIR test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i8 = anvil_type_i8(ctx);
    anvil_type_t *u8 = anvil_type_u8(ctx);
    anvil_value_t *signed_global = anvil_module_add_global(mod, "mir_s8",
                                                           i8, ANVIL_LINK_EXTERNAL);
    anvil_value_t *unsigned_global = anvil_module_add_global(mod, "mir_u8",
                                                             u8, ANVIL_LINK_EXTERNAL);
    CHECK(signed_global != NULL && unsigned_global != NULL,
          "signed and unsigned byte globals should be created");
    if (signed_global) {
        anvil_global_set_initializer(signed_global, anvil_const_i8(ctx, -5));
    }
    if (unsigned_global) {
        anvil_global_set_initializer(unsigned_global, anvil_const_u8(ctx, 250));
    }

    anvil_type_t *signed_fn_type = anvil_type_func(ctx, i8, NULL, 0, false);
    anvil_func_t *signed_fn = anvil_func_create(mod, "mir_load_s8",
                                                signed_fn_type,
                                                ANVIL_LINK_EXTERNAL);
    CHECK(signed_fn != NULL, "signed byte load function should be created");
    if (signed_fn && signed_global) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(signed_fn));
        anvil_value_t *loaded = anvil_build_load(ctx, i8, signed_global, "loaded");
        anvil_build_ret(ctx, loaded);
    }

    anvil_type_t *unsigned_fn_type = anvil_type_func(ctx, u8, NULL, 0, false);
    anvil_func_t *unsigned_fn = anvil_func_create(mod, "mir_load_u8",
                                                  unsigned_fn_type,
                                                  ANVIL_LINK_EXTERNAL);
    CHECK(unsigned_fn != NULL, "unsigned byte load function should be created");
    if (unsigned_fn && unsigned_global) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(unsigned_fn));
        anvil_value_t *loaded = anvil_build_load(ctx, u8, unsigned_global, "loaded");
        anvil_build_ret(ctx, loaded);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, "\tldrsb ") != NULL,
              "signed i8 loads should emit ldrsb");
        CHECK(strstr(asm_text, "\tldrb ") != NULL,
              "unsigned u8 loads should emit ldrb");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_darwin_module_codegen_routes_supported_leaf_function_through_mir(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_darwin_codegen");
    CHECK(mod != NULL, "module should be created for Darwin MIR routing test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *params[] = { i64, i64 };
    anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 2, false);
    anvil_func_t *fn = anvil_func_create(mod, "mir_darwin_leaf", fn_type,
                                         ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "Darwin MIR-routable leaf function should be created");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *a = anvil_func_get_param(fn, 0);
        anvil_value_t *b = anvil_func_get_param(fn, 1);
        anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
        anvil_build_ret(ctx, sum);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, "\t.globl _mir_darwin_leaf\n") != NULL,
              "Darwin MIR codegen should use underscore-prefixed global symbol");
        CHECK(strstr(asm_text, "_mir_darwin_leaf:\n") != NULL,
              "Darwin MIR codegen should emit underscore-prefixed function label");
        CHECK(strstr(asm_text, "\t.type mir_darwin_leaf") == NULL,
              "Darwin MIR codegen should not emit ELF type directive");
        CHECK(strstr(asm_text, ".Lmir_darwin_leaf_entry:\n") != NULL,
              "supported Darwin ARM64 function should be emitted through MIR");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_darwin_variadic_calls_route_through_mir(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_darwin_variadic");
    CHECK(mod != NULL, "module should be created for Darwin variadic MIR test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *callee_params[] = { i64 };
    anvil_type_t *callee_type = anvil_type_func(ctx, i64, callee_params, 1, true);
    anvil_value_t *callee = anvil_module_add_extern(mod, "darwin_vararg", callee_type);
    CHECK(callee != NULL, "Darwin variadic callee should be created");

    anvil_type_t *caller_type = anvil_type_func(ctx, i64, NULL, 0, false);
    anvil_func_t *caller = anvil_func_create(mod, "darwin_variadic_call",
                                             caller_type, ANVIL_LINK_EXTERNAL);
    CHECK(caller != NULL, "Darwin variadic caller should be created");
    if (caller && callee) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
        anvil_value_t *args[] = {
            anvil_const_i64(ctx, 1),
            anvil_const_i64(ctx, 2),
            anvil_const_i64(ctx, 3)
        };
        anvil_value_t *result = anvil_build_call(ctx, i64, callee, args, 3, "result");
        anvil_build_ret(ctx, result);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Ldarwin_variadic_call_entry:\n") != NULL,
              "Darwin variadic call should be emitted through MIR");
        CHECK(strstr(asm_text, "[sp, #0]\n") != NULL &&
              strstr(asm_text, "[sp, #8]\n") != NULL,
              "Darwin variadic MIR call should place variadic args on stack");
        CHECK(strstr(asm_text, "\tbl _darwin_vararg\n") != NULL,
              "Darwin variadic MIR call should still call underscored callee");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_module_codegen_lowers_switch_through_mir(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "switch_mir_regression");
    CHECK(mod != NULL, "module should be created for switch MIR test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *params[] = { i64 };
    anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 1, false);
    anvil_func_t *fn = anvil_func_create(mod, "switch_pick",
                                         fn_type, ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "switch function should be created");
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *case0 = anvil_block_create(fn, "case0");
        anvil_block_t *case1 = anvil_block_create(fn, "case1");
        anvil_block_t *def = anvil_block_create(fn, "default");
        CHECK(entry != NULL && case0 != NULL && case1 != NULL && def != NULL,
              "switch MIR blocks should be created");

        anvil_set_insert_point(ctx, entry);
        anvil_instr_t *sw =
            anvil_build_switch(ctx, anvil_func_get_param(fn, 0), def);
        CHECK(sw != NULL, "switch MIR terminator should be created");
        CHECK(anvil_switch_add_case(sw, anvil_const_i64(ctx, 0), case0),
              "switch MIR case 0 should be added");
        CHECK(anvil_switch_add_case(sw, anvil_const_i64(ctx, 7), case1),
              "switch MIR case 7 should be added");

        anvil_set_insert_point(ctx, case0);
        anvil_build_ret(ctx, anvil_const_i64(ctx, 100));

        anvil_set_insert_point(ctx, case1);
        anvil_build_ret(ctx, anvil_const_i64(ctx, 700));

        anvil_set_insert_point(ctx, def);
        anvil_build_ret(ctx, anvil_const_i64(ctx, -1));
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lswitch_pick_entry:\n") != NULL,
              "ARM64 switch function should be emitted through MIR");
        CHECK(strstr(asm_text, "\tcmp ") != NULL,
              "ARM64 switch lowering should compare the selector against cases");
        CHECK(strstr(asm_text, "\tcbnz ") != NULL,
              "ARM64 switch lowering should branch conditionally per case");
        CHECK(strstr(asm_text, ".Lswitch_pick_case0:\n") != NULL &&
              strstr(asm_text, ".Lswitch_pick_case1:\n") != NULL &&
              strstr(asm_text, ".Lswitch_pick_default:\n") != NULL,
              "ARM64 switch lowering should preserve case/default labels");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_module_codegen_lowers_function_pointer_indirect_call(void)
{
    anvil_ctx_t *ctx = new_arm64_linux_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "mir_func_ptr_call");
    CHECK(mod != NULL, "module should be created for function pointer MIR test");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32 };
    anvil_type_t *callee_type = anvil_type_func(ctx, i32, params, 2, false);
    anvil_type_t *callee_ptr_type = anvil_type_ptr(ctx, callee_type);

    anvil_func_t *callee = anvil_func_create(mod, "mir_fp_add_i32",
                                             callee_type,
                                             ANVIL_LINK_EXTERNAL);
    CHECK(callee != NULL, "function pointer MIR callee should be created");
    if (callee) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(callee));
        anvil_value_t *a = anvil_func_get_param(callee, 0);
        anvil_value_t *b = anvil_func_get_param(callee, 1);
        anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
        anvil_build_ret(ctx, sum);
    }

    anvil_type_t *caller_type = anvil_type_func(ctx, i32, NULL, 0, false);
    anvil_func_t *caller = anvil_func_create(mod, "mir_func_ptr_call",
                                             caller_type,
                                             ANVIL_LINK_EXTERNAL);
    CHECK(caller != NULL, "function pointer MIR caller should be created");
    if (caller && callee) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
        anvil_value_t *slot = anvil_build_alloca(ctx, callee_ptr_type, "slot");
        anvil_build_store(ctx, anvil_func_get_value(callee), slot);
        anvil_value_t *loaded =
            anvil_build_load(ctx, callee_ptr_type, slot, "loaded_fn");
        anvil_value_t *args[] = {
            anvil_const_i32(ctx, 3),
            anvil_const_i32(ctx, 4)
        };
        anvil_value_t *called =
            anvil_build_call(ctx, callee_type, loaded, args, 2, "called");
        anvil_build_ret(ctx, called);
    }

    char *asm_text = codegen_or_fail(mod);
    if (asm_text) {
        CHECK(strstr(asm_text, ".Lmir_func_ptr_call_entry:\n") != NULL,
              "function pointer caller should be emitted through MIR");
        CHECK(strstr(asm_text, "mir_fp_add_i32") != NULL,
              "function value should be materialized as a symbol address");
        CHECK(strstr(asm_text, "\tblr x16\n") != NULL,
              "indirect function pointer calls should branch through x16");
        free(asm_text);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    test_pointer_cache_tracks_target_changes();
    test_arm64_routes_fp_instruction_results_through_mir();
    test_arm64_dynamic_alloca_restores_sp_from_frame_pointer();
    test_arm64_emits_stack_arguments_after_x7();
    test_arm64_fp_call_uses_fp_registers_and_spills_d0_result();
    test_arm64_module_codegen_routes_supported_leaf_function_through_mir();
    test_arm64_module_codegen_routes_register_call_function_through_mir();
    test_arm64_module_codegen_routes_cfg_phi_function_through_mir();
    test_arm64_module_codegen_routes_stack_argument_calls_through_mir();
    test_arm64_module_codegen_loads_incoming_stack_arguments_through_mir();
    test_arm64_module_codegen_routes_memory_cast_select_function_through_mir();
    test_arm64_module_codegen_folds_constant_gep_into_load_store_offsets();
    test_arm64_module_codegen_materializes_global_and_string_addresses();
    test_arm64_module_codegen_uses_signed_and_unsigned_byte_loads();
    test_arm64_darwin_module_codegen_routes_supported_leaf_function_through_mir();
    test_arm64_darwin_variadic_calls_route_through_mir();
    test_arm64_module_codegen_lowers_switch_through_mir();
    test_arm64_module_codegen_lowers_function_pointer_indirect_call();

    if (failures) {
        fprintf(stderr, "%d regression test(s) failed\n", failures);
        return 1;
    }

    printf("core/arm64 regression tests passed\n");
    return 0;
}
