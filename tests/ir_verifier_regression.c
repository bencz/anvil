/*
 * Regression tests for source-level IR verification.
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

static anvil_ctx_t *new_ctx(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx != NULL, "context should be created");
    return ctx;
}

static anvil_func_t *new_i32_func(anvil_ctx_t *ctx, anvil_module_t **out_mod,
                                  const char *module_name,
                                  const char *func_name)
{
    anvil_module_t *mod = anvil_module_create(ctx, module_name);
    CHECK(mod != NULL, "module should be created");
    if (!mod) return NULL;

    anvil_type_t *fn_type =
        anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 0, false);
    anvil_func_t *fn = anvil_func_create(mod, func_name, fn_type,
                                         ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "function should be created");
    if (!fn) {
        anvil_module_destroy(mod);
        return NULL;
    }

    if (out_mod) *out_mod = mod;
    return fn;
}

static void test_ir_verifier_accepts_valid_function(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_valid", "valid");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *sum = anvil_build_add(ctx, anvil_const_i32(ctx, 2),
                                             anvil_const_i32(ctx, 3), "sum");
        anvil_build_ret(ctx, sum);

        char error[256] = { 0 };
        CHECK(anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should accept a valid module");
        CHECK(error[0] == '\0',
              "valid verification should not leave an error message");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_accepts_external_declaration_with_params(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "verify_decl");
    CHECK(mod != NULL, "module should be created for declaration test");
    if (mod) {
        anvil_type_t *params[] = {
            anvil_type_ptr(ctx, anvil_type_i8(ctx)),
            anvil_type_i32(ctx),
        };
        anvil_type_t *decl_type =
            anvil_type_func(ctx, anvil_type_i32(ctx), params, 2, true);
        anvil_func_t *decl =
            anvil_func_declare(mod, "printf_like", decl_type);
        CHECK(decl != NULL, "external declaration should be created");

        char error[256] = { 0 };
        CHECK(anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should accept external declarations with parameters");
        CHECK(error[0] == '\0',
              "valid external declaration should not leave an error message");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_binary_type_mismatch(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_bad_binop", "bad_binop");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *sum = anvil_build_add(ctx, anvil_const_i32(ctx, 2),
                                             anvil_const_i64(ctx, 3), "sum");
        anvil_build_ret(ctx, sum);

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject binary type mismatches");
        CHECK(strstr(error, "binary") != NULL &&
              strstr(error, "type") != NULL,
              "binary mismatch error should explain the type problem");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_phi_incoming_from_non_predecessor(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_bad_phi", "bad_phi");
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *merge = anvil_block_create(fn, "merge");
        anvil_block_t *other = anvil_block_create(fn, "other");
        CHECK(entry != NULL && merge != NULL && other != NULL,
              "PHI verifier test blocks should be created");

        anvil_set_insert_point(ctx, entry);
        anvil_build_br(ctx, merge);

        anvil_set_insert_point(ctx, other);
        anvil_build_ret(ctx, anvil_const_i32(ctx, 9));

        anvil_set_insert_point(ctx, merge);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 7), other);
        anvil_build_ret(ctx, phi);

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject PHI incoming from non-predecessor");
        CHECK(strstr(error, "PHI") != NULL &&
              strstr(error, "predecessor") != NULL,
              "PHI verifier error should mention predecessor mismatch");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_unterminated_block(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_unterminated",
                                    "unterminated");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_build_add(ctx, anvil_const_i32(ctx, 1),
                        anvil_const_i32(ctx, 2), "sum");

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject unterminated blocks");
        CHECK(strstr(error, "terminator") != NULL,
              "unterminated block error should mention terminator");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_call_signature_mismatch(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_bad_call", "bad_call");
    if (fn) {
        anvil_type_t *callee_params[] = { anvil_type_i32(ctx) };
        anvil_type_t *callee_type =
            anvil_type_func(ctx, anvil_type_i32(ctx),
                            callee_params, 1, false);
        anvil_value_t *callee =
            anvil_module_add_extern(mod, "takes_i32", callee_type);
        CHECK(callee != NULL, "external callee should be created");

        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *args[] = { anvil_const_i64(ctx, 42) };
        anvil_value_t *call =
            anvil_build_call(ctx, anvil_type_i32(ctx), callee,
                             args, 1, "call");
        anvil_build_ret(ctx, call);

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject call argument type mismatches");
        CHECK(strstr(error, "call") != NULL &&
              strstr(error, "argument") != NULL,
              "call verifier error should mention argument mismatch");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_return_value_from_other_function(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "verify_foreign_value");
    CHECK(mod != NULL, "module should be created for foreign value test");
    if (mod) {
        anvil_type_t *fn_type =
            anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 0, false);
        anvil_func_t *producer =
            anvil_func_create(mod, "producer", fn_type, ANVIL_LINK_EXTERNAL);
        anvil_func_t *consumer =
            anvil_func_create(mod, "consumer", fn_type, ANVIL_LINK_EXTERNAL);
        CHECK(producer != NULL && consumer != NULL,
              "producer and consumer functions should be created");

        anvil_value_t *foreign = NULL;
        if (producer) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(producer));
            foreign = anvil_build_add(ctx, anvil_const_i32(ctx, 1),
                                      anvil_const_i32(ctx, 2), "foreign");
            anvil_build_ret(ctx, foreign);
        }

        if (consumer && foreign) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(consumer));
            anvil_build_ret(ctx, foreign);
        }

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject return values from another function");
        CHECK(strstr(error, "outside the function") != NULL,
              "foreign return value error should mention function ownership");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_branch_condition_from_other_function(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "verify_foreign_branch");
    CHECK(mod != NULL, "module should be created for foreign branch test");
    if (mod) {
        anvil_type_t *fn_type =
            anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 0, false);
        anvil_func_t *producer =
            anvil_func_create(mod, "producer_cond", fn_type, ANVIL_LINK_EXTERNAL);
        anvil_func_t *consumer =
            anvil_func_create(mod, "consumer_branch", fn_type, ANVIL_LINK_EXTERNAL);
        CHECK(producer != NULL && consumer != NULL,
              "branch producer and consumer functions should be created");

        anvil_value_t *foreign_cond = NULL;
        if (producer) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(producer));
            foreign_cond = anvil_build_cmp_eq(ctx, anvil_const_i32(ctx, 1),
                                              anvil_const_i32(ctx, 1), "cond");
            anvil_build_ret(ctx, anvil_const_i32(ctx, 1));
        }

        if (consumer && foreign_cond) {
            anvil_block_t *then_block = anvil_block_create(consumer, "then");
            anvil_block_t *else_block = anvil_block_create(consumer, "else");
            CHECK(then_block != NULL && else_block != NULL,
                  "consumer branch blocks should be created");

            anvil_set_insert_point(ctx, anvil_func_get_entry(consumer));
            anvil_build_br_cond(ctx, foreign_cond, then_block, else_block);

            anvil_set_insert_point(ctx, then_block);
            anvil_build_ret(ctx, anvil_const_i32(ctx, 1));

            anvil_set_insert_point(ctx, else_block);
            anvil_build_ret(ctx, anvil_const_i32(ctx, 0));
        }

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject branch conditions from another function");
        CHECK(strstr(error, "outside the function") != NULL,
              "foreign branch condition error should mention function ownership");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_dynamic_alloca_count_from_other_function(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "verify_foreign_alloca");
    CHECK(mod != NULL, "module should be created for foreign alloca test");
    if (mod) {
        anvil_type_t *fn_type =
            anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 0, false);
        anvil_func_t *producer =
            anvil_func_create(mod, "producer_count", fn_type, ANVIL_LINK_EXTERNAL);
        anvil_func_t *consumer =
            anvil_func_create(mod, "consumer_alloca", fn_type, ANVIL_LINK_EXTERNAL);
        CHECK(producer != NULL && consumer != NULL,
              "alloca producer and consumer functions should be created");

        anvil_value_t *foreign_count = NULL;
        if (producer) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(producer));
            foreign_count = anvil_build_add(ctx, anvil_const_i32(ctx, 2),
                                            anvil_const_i32(ctx, 3), "count");
            anvil_build_ret(ctx, foreign_count);
        }

        if (consumer && foreign_count) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(consumer));
            anvil_build_alloca_dyn(ctx, anvil_type_i32(ctx),
                                   foreign_count, "items");
            anvil_build_ret(ctx, anvil_const_i32(ctx, 0));
        }

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject dynamic alloca counts from another function");
        CHECK(strstr(error, "outside the function") != NULL,
              "foreign dynamic alloca count error should mention function ownership");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_accepts_switch_and_dump_cases(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "verify_switch");
    CHECK(mod != NULL, "module should be created for switch test");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { i32 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 1, false);
        anvil_func_t *fn = anvil_func_create(mod, "switch_pick",
                                             fn_type, ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "switch function should be created");
        if (fn) {
            anvil_block_t *entry = anvil_func_get_entry(fn);
            anvil_block_t *case0 = anvil_block_create(fn, "case0");
            anvil_block_t *case1 = anvil_block_create(fn, "case1");
            anvil_block_t *def = anvil_block_create(fn, "default");
            CHECK(entry != NULL && case0 != NULL && case1 != NULL && def != NULL,
                  "switch blocks should be created");

            anvil_set_insert_point(ctx, entry);
            anvil_instr_t *sw =
                anvil_build_switch(ctx, anvil_func_get_param(fn, 0), def);
            CHECK(sw != NULL, "switch terminator should be created");
            CHECK(anvil_switch_add_case(sw, anvil_const_i32(ctx, 0), case0),
                  "first switch case should be added");
            CHECK(anvil_switch_add_case(sw, anvil_const_i32(ctx, 1), case1),
                  "second switch case should be added");

            anvil_set_insert_point(ctx, case0);
            anvil_build_ret(ctx, anvil_const_i32(ctx, 10));

            anvil_set_insert_point(ctx, case1);
            anvil_build_ret(ctx, anvil_const_i32(ctx, 20));

            anvil_set_insert_point(ctx, def);
            anvil_build_ret(ctx, anvil_const_i32(ctx, 30));
        }

        char error[256] = { 0 };
        CHECK(anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should accept a valid switch");
        CHECK(error[0] == '\0',
              "valid switch should not leave an error message");

        char *dump = anvil_module_to_string(mod);
        CHECK(dump != NULL, "switch module should dump to string");
        if (dump) {
            CHECK(strstr(dump, "switch") != NULL &&
                  strstr(dump, "default label %default") != NULL &&
                  strstr(dump, "case 0 label %case0") != NULL &&
                  strstr(dump, "case 1 label %case1") != NULL,
                  "IR dump should include switch default and cases");
            free(dump);
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_switch_case_type_mismatch(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "verify_bad_switch");
    CHECK(mod != NULL, "module should be created for bad switch test");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { i32 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 1, false);
        anvil_func_t *fn = anvil_func_create(mod, "bad_switch",
                                             fn_type, ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "bad switch function should be created");
        if (fn) {
            anvil_block_t *entry = anvil_func_get_entry(fn);
            anvil_block_t *case_block = anvil_block_create(fn, "case_i64");
            anvil_block_t *def = anvil_block_create(fn, "default");
            CHECK(entry != NULL && case_block != NULL && def != NULL,
                  "bad switch blocks should be created");

            anvil_set_insert_point(ctx, entry);
            anvil_instr_t *sw =
                anvil_build_switch(ctx, anvil_func_get_param(fn, 0), def);
            CHECK(sw != NULL, "bad switch terminator should be created");
            CHECK(anvil_switch_add_case(sw, anvil_const_i64(ctx, 1), case_block),
                  "mismatched switch case should still be representable");

            anvil_set_insert_point(ctx, case_block);
            anvil_build_ret(ctx, anvil_const_i32(ctx, 1));

            anvil_set_insert_point(ctx, def);
            anvil_build_ret(ctx, anvil_const_i32(ctx, 0));
        }

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject switch case type mismatch");
        CHECK(strstr(error, "switch") != NULL &&
              strstr(error, "case") != NULL &&
              strstr(error, "type") != NULL,
              "switch case mismatch error should explain the type problem");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_accepts_function_pointer_indirect_call(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "verify_func_ptr_call");
    CHECK(mod != NULL, "module should be created for function pointer test");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { i32, i32 };
        anvil_type_t *callee_type = anvil_type_func(ctx, i32, params, 2, false);
        anvil_type_t *callee_ptr_type = anvil_type_ptr(ctx, callee_type);

        anvil_func_t *callee = anvil_func_create(mod, "fp_add",
                                                 callee_type,
                                                 ANVIL_LINK_EXTERNAL);
        CHECK(callee != NULL, "function pointer callee should be created");
        if (callee) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(callee));
            anvil_value_t *a = anvil_func_get_param(callee, 0);
            anvil_value_t *b = anvil_func_get_param(callee, 1);
            anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
            anvil_build_ret(ctx, sum);

            anvil_value_t *callee_value = anvil_func_get_value(callee);
            CHECK(callee_value != NULL &&
                  callee_value->type &&
                  callee_value->type->kind == ANVIL_TYPE_PTR &&
                  callee_value->type->data.pointee == callee_type,
                  "function values should have pointer-to-function type");
        }

        anvil_type_t *caller_type = anvil_type_func(ctx, i32, NULL, 0, false);
        anvil_func_t *caller = anvil_func_create(mod, "fp_call",
                                                 caller_type,
                                                 ANVIL_LINK_EXTERNAL);
        CHECK(caller != NULL, "function pointer caller should be created");
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

        char error[256] = { 0 };
        CHECK(anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should accept function pointer indirect calls");
        CHECK(error[0] == '\0',
              "valid function pointer call should not leave an error message");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_accepts_global_pointer_variable_storage(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_global_ptr", "global_ptr");
    if (fn && mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *ptr_i32 = anvil_type_ptr(ctx, i32);
        anvil_value_t *global_ptr =
            anvil_module_add_global(mod, "gp", ptr_i32, ANVIL_LINK_EXTERNAL);
        CHECK(global_ptr != NULL, "pointer global should be created");

        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *local = anvil_build_alloca(ctx, i32, "local");
        anvil_build_store(ctx, local, global_ptr);
        anvil_value_t *loaded = anvil_build_load(ctx, ptr_i32, global_ptr, "loaded");
        anvil_build_store(ctx, anvil_const_i32(ctx, 7), loaded);
        anvil_build_ret(ctx, anvil_const_i32(ctx, 0));

        char error[256] = { 0 };
        CHECK(anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should treat pointer-typed globals as global variables");
        CHECK(error[0] == '\0',
              "valid global pointer storage should not leave an error message");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_codegen_runs_source_ir_verifier_before_backend(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    CHECK(anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64) == ANVIL_OK,
          "ARM64 target should be available for verifier codegen test");

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_codegen", "bad_codegen");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *sum = anvil_build_add(ctx, anvil_const_i32(ctx, 2),
                                             anvil_const_i64(ctx, 3), "sum");
        anvil_build_ret(ctx, sum);

        char *output = NULL;
        size_t len = 0;
        anvil_error_t err = anvil_module_codegen(mod, &output, &len);
        CHECK(err == ANVIL_ERR_INVALID_OP,
              "codegen should reject invalid source IR before backend lowering");
        CHECK(output == NULL && len == 0,
              "failed source IR verification should not produce output");
        CHECK(strstr(anvil_ctx_get_error(ctx), "IR verification failed") != NULL,
              "context error should report source IR verification failure");
        free(output);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    test_ir_verifier_accepts_valid_function();
    test_ir_verifier_accepts_external_declaration_with_params();
    test_ir_verifier_rejects_binary_type_mismatch();
    test_ir_verifier_rejects_phi_incoming_from_non_predecessor();
    test_ir_verifier_rejects_unterminated_block();
    test_ir_verifier_rejects_call_signature_mismatch();
    test_ir_verifier_rejects_return_value_from_other_function();
    test_ir_verifier_rejects_branch_condition_from_other_function();
    test_ir_verifier_rejects_dynamic_alloca_count_from_other_function();
    test_ir_verifier_accepts_switch_and_dump_cases();
    test_ir_verifier_rejects_switch_case_type_mismatch();
    test_ir_verifier_accepts_function_pointer_indirect_call();
    test_ir_verifier_accepts_global_pointer_variable_storage();
    test_codegen_runs_source_ir_verifier_before_backend();

    if (failures) {
        fprintf(stderr, "IR verifier regression tests failed: %d\n", failures);
        return 1;
    }

    printf("IR verifier regression tests passed\n");
    return 0;
}
