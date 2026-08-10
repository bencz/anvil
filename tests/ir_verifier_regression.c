/*
 * Regression tests for source-level IR verification.
 */

#include <anvil/anvil.h>
#include <anvil/anvil_internal.h>
#include <anvil/anvil_debug.h>
#include <anvil/anvil_opt.h>

#include <limits.h>
#include <stdint.h>
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
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
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
                                             anvil_const_i32(ctx, 3), "sum");
        if (sum) sum->data.instr->operands[1] = anvil_const_i64(ctx, 3);
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
        anvil_value_t *args[] = { anvil_const_i32(ctx, 42) };
        anvil_value_t *call =
            anvil_build_call(ctx, anvil_type_i32(ctx), callee,
                             args, 1, "call");
        if (call) call->data.instr->operands[1] = anvil_const_i64(ctx, 42);
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
            anvil_build_ret(ctx, anvil_const_i32(ctx, 0));
            consumer->entry->last->operands[0] = foreign;
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
            anvil_build_br_cond(ctx, anvil_const_i1(ctx, true),
                                then_block, else_block);
            consumer->entry->last->operands[0] = foreign_cond;

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
            anvil_value_t *items = anvil_build_alloca_dyn(
                ctx, anvil_type_i32(ctx), anvil_const_i32(ctx, 1), "items");
            if (items) items->data.instr->operands[0] = foreign_count;
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
            CHECK(anvil_switch_add_case(sw, anvil_const_i32(ctx, 1), case_block),
                  "valid switch case setup should succeed");
            if (sw) sw->operands[1] = anvil_const_i64(ctx, 1);

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

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_codegen", "bad_codegen");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *sum = anvil_build_add(ctx, anvil_const_i32(ctx, 2),
                                             anvil_const_i32(ctx, 3), "sum");
        if (sum) sum->data.instr->operands[1] = anvil_const_i64(ctx, 3);
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

static void test_ir_verifier_rejects_sibling_branch_use(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_sibling_use",
                                    "sibling_use");
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *left = anvil_block_create(fn, "left");
        anvil_block_t *right = anvil_block_create(fn, "right");
        anvil_block_t *merge = anvil_block_create(fn, "merge");

        anvil_set_insert_point(ctx, entry);
        anvil_build_br_cond(ctx, anvil_const_i1(ctx, true), left, right);

        anvil_set_insert_point(ctx, left);
        anvil_value_t *left_value =
            anvil_build_add(ctx, anvil_const_i32(ctx, 1),
                            anvil_const_i32(ctx, 2), "left_value");
        anvil_build_br(ctx, merge);

        anvil_set_insert_point(ctx, right);
        anvil_build_ret(ctx, left_value);

        anvil_set_insert_point(ctx, merge);
        anvil_build_ret(ctx, anvil_const_i32(ctx, 0));

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject a sibling-branch value use");
        CHECK(strstr(error, "does not dominate") != NULL,
              "sibling-branch error should mention dominance");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_same_block_use_before_definition(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_local_order",
                                    "local_order");
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *late =
            anvil_build_add(ctx, anvil_const_i32(ctx, 1),
                            anvil_const_i32(ctx, 2), "late");
        anvil_value_t *use =
            anvil_build_add(ctx, late, anvil_const_i32(ctx, 3), "use");
        anvil_build_ret(ctx, use);

        /* The public builder appends instructions, so construct this malformed
         * ordering explicitly to exercise verifier behavior after an IR pass. */
        anvil_instr_t *late_instr = late->data.instr;
        anvil_instr_t *use_instr = use->data.instr;
        anvil_instr_t *ret_instr = use_instr->next;
        entry->first = use_instr;
        use_instr->prev = NULL;
        use_instr->next = late_instr;
        late_instr->prev = use_instr;
        late_instr->next = ret_instr;
        ret_instr->prev = late_instr;

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject same-block use before definition");
        CHECK(strstr(error, "used before its definition") != NULL,
              "same-block order error should explain use before definition");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_duplicate_phi_predecessor(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_duplicate_phi",
                                    "duplicate_phi");
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *merge = anvil_block_create(fn, "merge");
        anvil_set_insert_point(ctx, entry);
        anvil_build_br(ctx, merge);

        anvil_set_insert_point(ctx, merge);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 1), entry);
        anvil_instr_t *phi_instr = phi->data.instr;
        /* Bypass the checked mutation API to prove the verifier remains an
         * independent corruption gate. The first append reserves capacity. */
        phi_instr->operands[1] = anvil_const_i32(ctx, 2);
        phi_instr->phi_blocks[1] = entry;
        phi_instr->num_operands = 2;
        phi_instr->num_phi_incoming = 2;
        anvil_build_ret(ctx, phi);

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject duplicate PHI predecessors");
        CHECK(strstr(error, "duplicate incoming predecessor") != NULL,
              "duplicate PHI error should identify the duplicate predecessor");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_missing_phi_predecessor(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_missing_phi",
                                    "missing_phi");
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *left = anvil_block_create(fn, "left");
        anvil_block_t *right = anvil_block_create(fn, "right");
        anvil_block_t *merge = anvil_block_create(fn, "merge");

        anvil_set_insert_point(ctx, entry);
        anvil_build_br_cond(ctx, anvil_const_i1(ctx, true), left, right);
        anvil_set_insert_point(ctx, left);
        anvil_build_br(ctx, merge);
        anvil_set_insert_point(ctx, right);
        anvil_build_br(ctx, merge);
        anvil_set_insert_point(ctx, merge);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 1), left);
        anvil_build_ret(ctx, phi);

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject a PHI missing a predecessor");
        CHECK(strstr(error, "missing an incoming predecessor") != NULL,
              "missing PHI error should identify the missing predecessor");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_phi_value_not_dominating_edge(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_phi_edge_dom",
                                    "phi_edge_dom");
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *left = anvil_block_create(fn, "left");
        anvil_block_t *right = anvil_block_create(fn, "right");
        anvil_block_t *merge = anvil_block_create(fn, "merge");

        anvil_set_insert_point(ctx, entry);
        anvil_build_br_cond(ctx, anvil_const_i1(ctx, true), left, right);
        anvil_set_insert_point(ctx, left);
        anvil_build_br(ctx, merge);
        anvil_set_insert_point(ctx, right);
        anvil_value_t *right_value =
            anvil_build_add(ctx, anvil_const_i32(ctx, 4),
                            anvil_const_i32(ctx, 5), "right_value");
        anvil_build_br(ctx, merge);
        anvil_set_insert_point(ctx, merge);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, right_value, left);
        anvil_phi_add_incoming(phi, right_value, right);
        anvil_build_ret(ctx, phi);

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject PHI value not dominating its edge");
        CHECK(strstr(error, "does not dominate incoming edge") != NULL,
              "PHI edge error should mention edge dominance");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_rejects_unreachable_block(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_unreachable",
                                    "unreachable");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_build_ret(ctx, anvil_const_i32(ctx, 0));

        anvil_block_t *dead = anvil_block_create(fn, "dead");
        anvil_set_insert_point(ctx, dead);
        anvil_build_ret(ctx, anvil_const_i32(ctx, 1));

        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should reject unreachable blocks");
        CHECK(strstr(error, "unreachable from the entry block") != NULL,
              "unreachable-block error should explain reachability");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ir_verifier_accepts_dominated_uses_and_phi_edges(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "verify_dominance_valid",
                                    "dominance_valid");
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *left = anvil_block_create(fn, "left");
        anvil_block_t *right = anvil_block_create(fn, "right");
        anvil_block_t *merge = anvil_block_create(fn, "merge");

        anvil_set_insert_point(ctx, entry);
        anvil_value_t *common =
            anvil_build_add(ctx, anvil_const_i32(ctx, 10),
                            anvil_const_i32(ctx, 20), "common");
        anvil_build_br_cond(ctx, anvil_const_i1(ctx, true), left, right);

        anvil_set_insert_point(ctx, left);
        anvil_value_t *left_value =
            anvil_build_add(ctx, common, anvil_const_i32(ctx, 1), "left_value");
        anvil_build_br(ctx, merge);

        anvil_set_insert_point(ctx, right);
        anvil_value_t *right_value =
            anvil_build_add(ctx, common, anvil_const_i32(ctx, 2), "right_value");
        anvil_build_br(ctx, merge);

        anvil_set_insert_point(ctx, merge);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, left_value, left);
        anvil_phi_add_incoming(phi, right_value, right);
        anvil_build_ret(ctx, phi);

        char error[256] = { 0 };
        CHECK(anvil_module_verify(mod, error, sizeof(error)),
              "source IR verifier should accept dominated uses and PHI edges");
        CHECK(error[0] == '\0',
              "valid dominance verification should not leave an error message");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_context_owns_constants_shared_across_modules(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *first = anvil_module_create(ctx, "constant_owner_first");
    anvil_module_t *second = anvil_module_create(ctx, "constant_owner_second");
    anvil_value_t *shared = anvil_const_i32(ctx, 73);
    anvil_value_t *first_global = anvil_module_add_global(
        first, "first_value", anvil_type_i32(ctx), ANVIL_LINK_INTERNAL);
    anvil_value_t *second_global = anvil_module_add_global(
        second, "second_value", anvil_type_i32(ctx), ANVIL_LINK_INTERNAL);
    CHECK(first && second && shared && first_global && second_global,
          "shared-constant modules and globals should be created");

    if (first_global && second_global && shared) {
        anvil_global_set_initializer(first_global, shared);
        anvil_global_set_initializer(second_global, shared);
        CHECK(first_global->data.global.init == shared &&
              second_global->data.global.init == shared,
              "one context-owned constant should initialize multiple modules");
    }

    anvil_module_destroy(first);
    first = NULL;
    if (second) {
        char error[256] = { 0 };
        CHECK(second_global && second_global->data.global.init == shared &&
              anvil_const_int_signed_value(shared) == 73,
              "destroying one module must not invalidate its shared constant");
        CHECK(anvil_module_verify(second, error, sizeof(error)),
              "remaining module should verify after other shared owner is destroyed");
    }

    anvil_module_destroy(second);

    anvil_module_t *third = anvil_module_create(ctx, "constant_owner_third");
    anvil_module_t *fourth = anvil_module_create(ctx, "constant_owner_fourth");
    anvil_value_t *third_global = anvil_module_add_global(
        third, "third_value", anvil_type_i32(ctx), ANVIL_LINK_INTERNAL);
    anvil_value_t *fourth_global = anvil_module_add_global(
        fourth, "fourth_value", anvil_type_i32(ctx), ANVIL_LINK_INTERNAL);
    anvil_global_set_initializer(third_global, shared);
    anvil_global_set_initializer(fourth_global, shared);
    anvil_module_destroy(fourth);
    if (third) {
        char error[256] = { 0 };
        CHECK(third_global && third_global->data.global.init == shared &&
              anvil_module_verify(third, error, sizeof(error)),
              "shared constant must survive the reverse module destruction order");
    }
    anvil_module_destroy(third);

    /* These deliberately remain unattached to any module.  The context must
     * still reclaim the entire nested constant DAG exactly once. */
    anvil_value_t *orphan_values[] = {
        anvil_const_i32(ctx, 1), anvil_const_i32(ctx, 2)
    };
    anvil_value_t *orphan_array = anvil_const_array(
        ctx, anvil_type_i32(ctx), orphan_values, 2);
    CHECK(orphan_array != NULL,
          "orphan array constant should be context-owned");

    anvil_ctx_destroy(ctx);
}

static void test_cross_context_types_and_values_are_rejected(void)
{
    anvil_ctx_t *left = new_ctx();
    anvil_ctx_t *right = new_ctx();
    if (!left || !right) {
        anvil_ctx_destroy(left);
        anvil_ctx_destroy(right);
        return;
    }

    anvil_type_t *left_i32 = anvil_type_i32(left);
    anvil_type_t *right_i32 = anvil_type_i32(right);
    anvil_type_t *bad_fields[] = { left_i32 };
    anvil_type_t *bad_params[] = { left_i32 };
    CHECK(anvil_type_ptr(right, left_i32) == NULL,
          "pointer type must reject a pointee from another context");
    CHECK(anvil_type_array(right, left_i32, 2) == NULL,
          "array type must reject an element from another context");
    CHECK(anvil_type_struct(right, "bad", bad_fields, 1) == NULL,
          "struct type must reject a field from another context");
    CHECK(anvil_type_func(right, right_i32, bad_params, 1, false) == NULL,
          "function type must reject a parameter from another context");
    CHECK(anvil_const_null(right, anvil_type_ptr(left, left_i32)) == NULL,
          "null constant must reject a pointer type from another context");

    anvil_module_t *mod = anvil_module_create(right, "cross_context");
    anvil_value_t *right_global = anvil_module_add_global(
        mod, "right_global", right_i32, ANVIL_LINK_INTERNAL);
    anvil_global_set_initializer(right_global, anvil_const_i32(left, 11));
    CHECK(right_global && right_global->data.global.init == NULL,
          "global initializer must reject a constant from another context");
    CHECK(anvil_module_add_global(mod, "bad_global", left_i32,
                                  ANVIL_LINK_INTERNAL) == NULL,
          "module must reject a global type from another context");
    anvil_type_t *left_fn_type =
        anvil_type_func(left, left_i32, NULL, 0, false);
    CHECK(anvil_func_create(mod, "bad_func", left_fn_type,
                            ANVIL_LINK_INTERNAL) == NULL,
          "module must reject a function type from another context");

    anvil_type_t *right_fn_type =
        anvil_type_func(right, right_i32, NULL, 0, false);
    anvil_func_t *fn = anvil_func_create(mod, "cross_operand", right_fn_type,
                                         ANVIL_LINK_INTERNAL);
    if (fn) {
        anvil_set_insert_point(right, anvil_func_get_entry(fn));
        anvil_value_t *sum = anvil_build_add(
            right, anvil_const_i32(right, 4), anvil_const_i32(right, 5), "sum");
        if (sum && sum->data.instr) {
            sum->data.instr->operands[0] = anvil_const_i32(left, 4);
        }
        anvil_build_ret(right, sum);
        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "verifier must reject an operand owned by another context");
        CHECK(strstr(error, "another context") != NULL,
              "cross-context verifier failure should be diagnostic");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(right);
    anvil_ctx_destroy(left);
}

static void test_global_initializer_requires_constant_dag(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "initializer_dag", "make_value");
    if (fn) {
        anvil_value_t *global = anvil_module_add_global(
            mod, "g", anvil_type_i32(ctx), ANVIL_LINK_INTERNAL);
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *sum = anvil_build_add(
            ctx, anvil_const_i32(ctx, 1), anvil_const_i32(ctx, 2), "sum");
        anvil_build_ret(ctx, sum);

        anvil_global_set_initializer(global, sum);
        CHECK(global && global->data.global.init == NULL,
              "global initializer API must reject an SSA instruction result");

        /* Internal corruption must still be diagnosed at the verifier gate. */
        if (global) global->data.global.init = sum;
        char error[256] = { 0 };
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "verifier must reject a non-constant global initializer");
        CHECK(strstr(error, "constant DAG") != NULL,
              "invalid initializer should report the constant-DAG contract");
        if (global) global->data.global.init = NULL;
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_nested_and_malformed_array_constants(void)
{
    anvil_ctx_t *ctx = new_ctx();
    anvil_ctx_t *other = new_ctx();
    if (!ctx || !other) {
        anvil_ctx_destroy(ctx);
        anvil_ctx_destroy(other);
        return;
    }

    anvil_value_t *row1_values[] = {
        anvil_const_i32(ctx, 1), anvil_const_i32(ctx, 2)
    };
    anvil_value_t *row2_values[] = {
        anvil_const_i32(ctx, 3), anvil_const_i32(ctx, 4)
    };
    anvil_value_t *row1 = anvil_const_array(
        ctx, anvil_type_i32(ctx), row1_values, 2);
    anvil_value_t *row2 = anvil_const_array(
        ctx, anvil_type_i32(ctx), row2_values, 2);
    anvil_value_t *rows[] = { row1, row2 };
    anvil_value_t *matrix = row1
        ? anvil_const_array(ctx, row1->type, rows, 2)
        : NULL;
    CHECK(matrix && anvil_value_is_constant_dag(matrix, ctx),
          "nested array constants should form a valid same-context DAG");

    anvil_module_t *mod = anvil_module_create(ctx, "nested_constants");
    anvil_value_t *global = matrix
        ? anvil_module_add_global(mod, "matrix", matrix->type,
                                  ANVIL_LINK_INTERNAL)
        : NULL;
    anvil_global_set_initializer(global, matrix);
    char error[256] = { 0 };
    CHECK(global && global->data.global.init == matrix &&
          anvil_module_verify(mod, error, sizeof(error)),
          "nested constant array should be accepted as a global initializer");

    anvil_value_t *wrong_type_values[] = { anvil_const_i64(ctx, 9) };
    CHECK(anvil_const_array(ctx, anvil_type_i32(ctx), wrong_type_values, 1) == NULL,
          "array constructor must reject an element type mismatch");
    anvil_value_t *foreign_values[] = { anvil_const_i32(other, 9) };
    CHECK(anvil_const_array(ctx, anvil_type_i32(ctx), foreign_values, 1) == NULL,
          "array constructor must reject a foreign-context element");

    if (matrix) {
        anvil_value_t *saved = matrix->data.array.elements[0];
        matrix->data.array.elements[0] = NULL;
        CHECK(!anvil_value_is_constant_dag(matrix, ctx),
              "constant-DAG validation must reject a null nested element");
        memset(error, 0, sizeof(error));
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "verifier must reject a malformed nested array initializer");
        matrix->data.array.elements[0] = saved;
        memset(error, 0, sizeof(error));
        CHECK(anvil_module_verify(mod, error, sizeof(error)),
              "restored nested array initializer should verify again");

        anvil_type_t *saved_elem_type = matrix->type->data.array.elem;
        matrix->type->data.array.elem = matrix->type;
        matrix->data.array.elements[0] = matrix;
        CHECK(!anvil_value_is_constant_dag(matrix, ctx),
              "constant-DAG validation must reject a cycle");
        matrix->data.array.elements[0] = saved;
        matrix->type->data.array.elem = saved_elem_type;
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(other);
    anvil_ctx_destroy(ctx);
}

static void test_constant_dag_and_module_oom_diagnostics(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_value_t *leaves[12];
    for (size_t i = 0; i < 12; i++)
        leaves[i] = anvil_const_i32(ctx, (int32_t)i);
    anvil_value_t *wide =
        anvil_const_array(ctx, anvil_type_i32(ctx), leaves, 12);
    CHECK(wide != NULL, "wide constant DAG setup should succeed");
    anvil_ctx_clear_error(ctx);
    anvil_test_fail_alloc_after(ctx, 2); /* marks + frames, fail marks grow */
    CHECK(anvil_value_check_constant_dag(wide, ctx) ==
              ANVIL_CONST_DAG_NOMEM &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM,
          "constant mark-table growth OOM must be distinct from malformed DAG");
    anvil_test_disable_alloc_fail(ctx);

    anvil_value_t *deep = anvil_const_i32(ctx, 1);
    for (size_t i = 0; i < 18 && deep; i++) {
        anvil_value_t *one[] = { deep };
        deep = anvil_const_array(ctx, deep->type, one, 1);
    }
    CHECK(deep != NULL, "deep constant DAG setup should succeed");
    anvil_ctx_clear_error(ctx);
    /* Before depth 16 the traversal performs two mark-table grows after the
     * two initial allocations; the next allocation is the frame-stack grow. */
    anvil_test_fail_alloc_after(ctx, 4);
    CHECK(anvil_value_check_constant_dag(deep, ctx) ==
              ANVIL_CONST_DAG_NOMEM &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM,
          "constant frame-stack growth OOM must preserve NOMEM");
    anvil_test_disable_alloc_fail(ctx);

    anvil_type_t *decimal = anvil_type_decimal_packed(ctx, 5, 2);
    anvil_ctx_clear_error(ctx);
    CHECK(anvil_const_decimal(ctx, decimal, "12.345") == NULL &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_INVALID_ARG,
          "invalid decimal literals must set a structured diagnostic");
    anvil_ctx_clear_error(ctx);
    anvil_test_fail_alloc_after(ctx, 1); /* value succeeds; digits copy fails */
    CHECK(anvil_const_decimal(ctx, decimal, "12.34") == NULL &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM,
          "decimal literal copy must participate in fault injection");
    anvil_test_disable_alloc_fail(ctx);

    anvil_ctx_clear_error(ctx);
    anvil_test_fail_alloc_after(ctx, 0);
    CHECK(anvil_module_create(ctx, "module_oom") == NULL &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM &&
          ctx->modules == NULL,
          "module record OOM must be diagnosed and leave the context unchanged");
    anvil_test_disable_alloc_fail(ctx);
    anvil_ctx_clear_error(ctx);
    anvil_test_fail_alloc_after(ctx, 1);
    CHECK(anvil_module_create(ctx, "module_name_oom") == NULL &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM &&
          ctx->modules == NULL,
          "module name OOM must be diagnosed and transactional");
    anvil_test_disable_alloc_fail(ctx);
    anvil_module_t *retry = anvil_module_create(ctx, "module_retry");
    CHECK(retry != NULL, "module creation must be retryable after injected OOM");
    anvil_module_destroy(retry);

    anvil_ctx_destroy(ctx);
}

static void test_id_exhaustion_and_verifier_oom(void)
{
    anvil_ctx_t *value_ctx = new_ctx();
    if (value_ctx) {
        value_ctx->next_value_id = UINT32_MAX;
        anvil_ctx_clear_error(value_ctx);
        CHECK(anvil_const_i32(value_ctx, 1) == NULL &&
              value_ctx->next_value_id == UINT32_MAX &&
              anvil_ctx_get_last_error(value_ctx) == ANVIL_ERR_INVALID_OP,
              "value ID exhaustion must fail before allocation and never wrap");
        anvil_ctx_destroy(value_ctx);
    }

    anvil_ctx_t *func_ctx = new_ctx();
    if (func_ctx) {
        anvil_module_t *mod = anvil_module_create(func_ctx, "func_id_limit");
        anvil_type_t *type = anvil_type_func(func_ctx,
                                             anvil_type_void(func_ctx),
                                             NULL, 0, false);
        func_ctx->next_func_id = UINT32_MAX;
        CHECK(anvil_func_declare(mod, "too_many", type) == NULL &&
              func_ctx->next_func_id == UINT32_MAX &&
              anvil_module_symbol_count(mod) == 0 &&
              anvil_ctx_get_last_error(func_ctx) == ANVIL_ERR_INVALID_OP,
              "function ID exhaustion must be transactional and never wrap");
        anvil_module_destroy(mod);
        anvil_ctx_destroy(func_ctx);
    }

    anvil_ctx_t *block_ctx = new_ctx();
    if (block_ctx) {
        anvil_module_t *mod = NULL;
        anvil_func_t *fn = new_i32_func(block_ctx, &mod,
                                        "block_id_limit", "entry_ok");
        block_ctx->next_block_id = UINT32_MAX;
        size_t before = fn ? fn->num_blocks : 0;
        CHECK(fn && anvil_block_create(fn, "too_many") == NULL &&
              block_ctx->next_block_id == UINT32_MAX &&
              fn->num_blocks == before &&
              anvil_ctx_get_last_error(block_ctx) == ANVIL_ERR_INVALID_OP,
              "basic-block ID exhaustion must not mutate function topology");
        anvil_module_destroy(mod);
        anvil_ctx_destroy(block_ctx);
    }

    anvil_ctx_t *verify_ctx = new_ctx();
    if (verify_ctx) {
        anvil_module_t *mod = NULL;
        anvil_func_t *fn = new_i32_func(verify_ctx, &mod,
                                        "verify_oom", "verify_oom");
        if (fn) {
            anvil_set_insert_point(verify_ctx, anvil_func_get_entry(fn));
            anvil_build_ret(verify_ctx, anvil_const_i32(verify_ctx, 0));
            char error[256] = { 0 };
            anvil_ctx_clear_error(verify_ctx);
            anvil_test_fail_alloc_after(verify_ctx, 0);
            CHECK(!anvil_module_verify(mod, error, sizeof(error)) &&
                  anvil_ctx_get_last_error(verify_ctx) == ANVIL_ERR_NOMEM &&
                  strstr(error, "out of memory") != NULL,
                  "verifier CFG allocation must use fault injection and report OOM");
            anvil_test_disable_alloc_fail(verify_ctx);
            memset(error, 0, sizeof(error));
            CHECK(anvil_module_verify(mod, error, sizeof(error)),
                  "verification must be retryable after injected CFG OOM");
        }
        anvil_module_destroy(mod);
        anvil_ctx_destroy(verify_ctx);
    }
}

static void test_type_constructor_input_and_layout_guards(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    CHECK(anvil_type_struct(ctx, "bad", NULL, 1) == NULL,
          "non-empty struct requires a field array");
    CHECK(anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 1, false) == NULL,
          "function type with parameters requires a parameter array");
    CHECK(anvil_type_array(ctx, anvil_type_i64(ctx), SIZE_MAX) == NULL,
          "array layout must reject size multiplication overflow");
    CHECK(anvil_type_decimal_packed(ctx, UINT_MAX, 0) == NULL,
          "packed decimal layout must reject precision arithmetic overflow");

    anvil_type_t *empty = anvil_type_struct(ctx, "empty", NULL, 0);
    CHECK(empty && anvil_type_size(empty) == 0 && anvil_type_align(empty) == 1,
          "empty struct should have a defined zero-size, byte-aligned layout");

    anvil_ctx_destroy(ctx);
}

static size_t owned_instr_count(const anvil_ctx_t *ctx)
{
    size_t count = 0;
    for (const anvil_instr_t *instr = ctx ? ctx->owned_instrs : NULL;
         instr; instr = instr->ctx_next_owned) {
        count++;
    }
    return count;
}

static size_t owned_value_count(const anvil_ctx_t *ctx)
{
    size_t count = 0;
    for (const anvil_value_t *value = ctx ? ctx->owned_values : NULL;
         value; value = value->ctx_next_owned) count++;
    return count;
}

static void test_builders_reject_invalid_semantics_transactionally(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod,
                                    "builder_semantics", "checked");
    if (!fn) { anvil_ctx_destroy(ctx); return; }
    anvil_block_t *entry = anvil_func_get_entry(fn);
    anvil_set_insert_point(ctx, entry);

    anvil_type_t *callee_params[] = { anvil_type_i32(ctx) };
    anvil_type_t *callee_type = anvil_type_func(
        ctx, anvil_type_i32(ctx), callee_params, 1, false);
    anvil_func_t *callee = anvil_func_declare(mod, "callee", callee_type);
    anvil_type_t *other_type = anvil_type_func(
        ctx, anvil_type_i32(ctx), NULL, 0, false);
    anvil_func_t *other = anvil_func_create(
        mod, "other", other_type, ANVIL_LINK_INTERNAL);
    anvil_block_t *foreign_block = other ? anvil_func_get_entry(other) : NULL;
    anvil_value_t *i32 = anvil_const_i32(ctx, 1);
    anvil_value_t *u32 = anvil_const_u32(ctx, 1);
    anvil_value_t *i64 = anvil_const_i64(ctx, 1);
    anvil_value_t *f32 = anvil_const_f32(ctx, 1.0f);
    anvil_value_t *cond = anvil_const_i1(ctx, true);
    anvil_ctx_t *foreign_ctx = new_ctx();
    anvil_value_t *foreign_index = foreign_ctx
        ? anvil_const_i32(foreign_ctx, 0) : NULL;
    anvil_value_t *foreign_indices[] = { foreign_index };
    anvil_value_t *slot = anvil_build_alloca(ctx, anvil_type_i32(ctx), "slot");
    anvil_type_t *aggregate_fields[] = { anvil_type_i32(ctx) };
    anvil_type_t *aggregate_type = anvil_type_literal_struct(
        ctx, aggregate_fields, 1, false);
    anvil_value_t *aggregate_slot = anvil_build_alloca(
        ctx, aggregate_type, "aggregate.slot");
    anvil_value_t *aggregate = anvil_build_load(
        ctx, aggregate_type, aggregate_slot, "aggregate");
    anvil_instr_t *cursor = ctx->insert_point;
    size_t instrs = owned_instr_count(ctx);
    size_t values = owned_value_count(ctx);

#define CHECK_INVALID_BUILD(expr, msg) do { \
    anvil_ctx_clear_error(ctx); \
    CHECK(!(expr) && anvil_ctx_get_last_error(ctx) == ANVIL_ERR_INVALID_ARG && \
          ctx->insert_point == cursor && entry->last == cursor && \
          owned_instr_count(ctx) == instrs && owned_value_count(ctx) == values, \
          msg); \
} while (0)

    CHECK_INVALID_BUILD(anvil_build_add(ctx, i32, i64, "bad") != NULL,
                        "invalid binop must fail before allocating/inserting IR");
    CHECK_INVALID_BUILD(anvil_build_udiv(ctx, i32, i32, "bad") != NULL,
                        "unsigned division must reject signed operand types");
    CHECK_INVALID_BUILD(anvil_build_sdiv(ctx, u32, u32, "bad") != NULL,
                        "signed division must reject unsigned operand types");
    CHECK_INVALID_BUILD(anvil_build_cmp_ult(ctx, i32, i32, "bad") != NULL,
                        "unsigned comparison must reject signed operand types");
    CHECK_INVALID_BUILD(anvil_build_cmp_lt(ctx, u32, u32, "bad") != NULL,
                        "signed comparison must reject unsigned operand types");
    CHECK_INVALID_BUILD(anvil_build_cmp_eq(ctx, i32, f32, "bad") != NULL,
                        "invalid comparison must be transactional");
    CHECK_INVALID_BUILD(anvil_build_fneg(ctx, i32, "bad") != NULL,
                        "invalid unary operation must be transactional");
    CHECK_INVALID_BUILD(anvil_build_trunc(ctx, i32, anvil_type_i32(ctx),
                                          "bad") != NULL,
                        "invalid cast must be transactional");
    CHECK_INVALID_BUILD(anvil_build_uitofp(ctx, i32, anvil_type_f32(ctx),
                                           "bad") != NULL,
                        "UITOFP must reject signed source types");
    CHECK_INVALID_BUILD(anvil_build_sitofp(ctx, u32, anvil_type_f32(ctx),
                                           "bad") != NULL,
                        "SITOFP must reject unsigned source types");
    CHECK_INVALID_BUILD(anvil_build_fptoui(ctx, f32, anvil_type_i32(ctx),
                                           "bad") != NULL,
                        "FPTOUI must reject signed result types");
    CHECK_INVALID_BUILD(anvil_build_fptosi(ctx, f32, anvil_type_u32(ctx),
                                           "bad") != NULL,
                        "FPTOSI must reject unsigned result types");
    CHECK_INVALID_BUILD(anvil_build_bitcast(ctx, aggregate,
                                            anvil_type_i32(ctx),
                                            "bad") != NULL,
                        "BITCAST must reject aggregate values unsupported by lowerings");
    CHECK_INVALID_BUILD(anvil_build_load(ctx, anvil_type_i32(ctx), i32,
                                         "bad") != NULL,
                        "invalid load address must be transactional");
    CHECK_INVALID_BUILD(anvil_build_store(ctx, i64, slot),
                        "invalid store type must be transactional");
    anvil_value_t *bad_args[] = { i64 };
    CHECK_INVALID_BUILD(anvil_build_call(ctx, anvil_type_i32(ctx),
                                         anvil_func_get_value(callee),
                                         bad_args, 1, "bad") != NULL,
                        "invalid call signature must be transactional");
    CHECK_INVALID_BUILD(anvil_build_br(ctx, foreign_block),
                        "cross-function branch must be transactional");
    CHECK_INVALID_BUILD(anvil_build_br_cond(ctx, i32, entry, entry),
                        "non-boolean conditional branch must be transactional");
    CHECK_INVALID_BUILD(anvil_build_select(ctx, cond, i32, i64,
                                           "bad") != NULL,
                        "invalid select alternatives must be transactional");
    CHECK_INVALID_BUILD(anvil_build_ret(ctx, i64),
                        "mismatched return must be transactional");
    CHECK_INVALID_BUILD(anvil_build_alloca_dyn(ctx, anvil_type_i32(ctx),
                                               f32, "bad") != NULL,
                        "dynamic alloca count must be integer and transactional");
    CHECK_INVALID_BUILD(anvil_build_alloca(ctx, anvil_type_void(ctx),
                                           "bad") != NULL,
                        "alloca of void must fail before allocation");
    CHECK_INVALID_BUILD(anvil_build_alloca(ctx, callee_type,
                                           "bad") != NULL,
                        "alloca of function type must fail before allocation");
    CHECK_INVALID_BUILD(anvil_build_phi(ctx, anvil_type_void(ctx),
                                        "bad") != NULL,
                        "void PHI must fail before allocation");
    CHECK_INVALID_BUILD(anvil_build_gep(ctx, anvil_type_i32(ctx), slot,
                                        foreign_indices, 1, "bad") != NULL,
                        "GEP index from another context must be transactional");
#undef CHECK_INVALID_BUILD

    anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "checked.phi");
    anvil_value_t *phi_first = anvil_const_i32(ctx, 4);
    anvil_value_t *phi_duplicate = anvil_const_i32(ctx, 5);
    anvil_value_t *phi_wrong_type = anvil_const_i64(ctx, 6);
    CHECK(phi && anvil_phi_add_incoming(phi, phi_first, entry),
          "valid PHI incoming setup should succeed");
    anvil_instr_t *phi_instr = phi ? phi->data.instr : NULL;
    size_t phi_count = phi_instr ? phi_instr->num_phi_incoming : 0;
    CHECK(!anvil_phi_add_incoming(phi, phi_duplicate, entry) &&
          phi_instr->num_phi_incoming == phi_count &&
          phi_instr->num_operands == phi_count,
          "duplicate PHI predecessor must fail without partial mutation");
    CHECK(!anvil_phi_add_incoming(phi, phi_wrong_type, entry) &&
          phi_instr->num_phi_incoming == phi_count,
          "PHI type mismatch must fail without partial mutation");

    cursor = ctx->insert_point;
    instrs = owned_instr_count(ctx);
    anvil_ctx_clear_error(ctx);
    anvil_test_fail_alloc_after(ctx, 0);
    CHECK(!anvil_build_store(ctx, i32, slot) &&
          ctx->insert_point == cursor && entry->last == cursor &&
          owned_instr_count(ctx) == instrs &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM,
          "checked void builder must surface OOM without mutating the block");
    anvil_test_disable_alloc_fail(ctx);

    anvil_ctx_clear_error(ctx);
    CHECK(!anvil_func_set_cc(fn, (anvil_cc_t)-1) &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_INVALID_ARG,
          "function calling convention setter must reject invalid enums");
    anvil_module_destroy(mod);
    anvil_ctx_clear_error(ctx);
    CHECK(!anvil_func_set_cc(fn, ANVIL_CC_CDECL) &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_INVALID_OP,
          "destroyed function handles must be tombstoned and diagnosed");
    anvil_ctx_destroy(foreign_ctx);
    anvil_ctx_destroy(ctx);
}

static void test_i1_and_all_fcmp_predicates(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;
    anvil_type_t *i1 = anvil_type_i1(ctx);
    CHECK(i1 && anvil_type_is_bool(i1) && anvil_type_is_integer(i1) &&
          anvil_type_size(i1) == 1 && anvil_type_align(i1) == 1 &&
          anvil_type_bit_width(i1) == 1 &&
          anvil_ctx_get_data_layout(ctx)->i1.size == 1,
          "i1 must expose one-bit semantics with byte-addressable storage");
    anvil_value_t *truth = anvil_const_i1(ctx, true);
    anvil_value_t *falsehood = anvil_const_i1(ctx, false);
    CHECK(truth && falsehood && truth->data.u == 1 && falsehood->data.u == 0 &&
          anvil_value_is_bool(truth),
          "i1 constants must be canonical booleans");

    anvil_module_t *mod = anvil_module_create(ctx, "i1_fcmp");
    anvil_type_t *params[] = { anvil_type_f64(ctx), anvil_type_f64(ctx) };
    anvil_type_t *fn_type = anvil_type_func(ctx, i1, params, 2, false);
    anvil_func_t *fn = anvil_func_create(mod, "all_predicates", fn_type,
                                         ANVIL_LINK_INTERNAL);
    if (!fn) { anvil_module_destroy(mod); anvil_ctx_destroy(ctx); return; }
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *lhs = anvil_func_get_param(fn, 0);
    anvil_value_t *rhs = anvil_func_get_param(fn, 1);
    anvil_value_t *combined = falsehood;
    for (unsigned p = ANVIL_FCMP_FALSE; p <= ANVIL_FCMP_TRUE; p++) {
        anvil_value_t *cmp = anvil_build_fcmp(
            ctx, (anvil_fcmp_pred_t)p, lhs, rhs, "fp.pred");
        CHECK(cmp && cmp->type == i1 && cmp->data.instr->fcmp_pred ==
                  (anvil_fcmp_pred_t)p,
              "every public FCMP predicate must build an i1 result");
        combined = anvil_build_or(ctx, combined, cmp, "any.pred");
    }
    anvil_value_t *eq = anvil_build_cmp_eq(ctx, truth, falsehood, "bool.eq");
    CHECK(eq && eq->type == i1,
          "integer equality on i1 must remain a boolean operation");
    CHECK(anvil_build_not(ctx, truth, "bool.not") != NULL &&
          anvil_build_xor(ctx, truth, falsehood, "bool.xor") != NULL,
          "i1 must support canonical boolean bitwise operations");
    CHECK(anvil_build_zext(ctx, truth, anvil_type_i8(ctx), "to.i8") &&
          anvil_build_zext(ctx, truth, anvil_type_i32(ctx), "to.i32") &&
          anvil_build_trunc(ctx, anvil_const_i8(ctx, 3), i1, "to.i1"),
          "integer casts must compare semantic bit width, not storage size");

    anvil_value_t *legacy_i8_cond = anvil_const_i8(ctx, 1);
    anvil_value_t *legacy_u8_cond = anvil_const_u8(ctx, 1);
    anvil_value_t *gep_base = anvil_build_alloca(ctx, anvil_type_i32(ctx),
                                                 "gep.base");
    anvil_value_t *bool_index[] = { truth };
    size_t before_instrs = owned_instr_count(ctx);
    size_t before_values = owned_value_count(ctx);
    anvil_instr_t *before_cursor = ctx->insert_point;
#define CHECK_BAD_I1(expr, msg) do { \
    anvil_ctx_clear_error(ctx); \
    CHECK(!(expr) && anvil_ctx_get_last_error(ctx) == ANVIL_ERR_INVALID_ARG && \
          owned_instr_count(ctx) == before_instrs && \
          owned_value_count(ctx) == before_values && \
          ctx->insert_point == before_cursor, msg); \
} while (0)
    CHECK_BAD_I1(anvil_build_fcmp(ctx, (anvil_fcmp_pred_t)-1,
                                  lhs, rhs, "bad") != NULL,
                 "invalid FCMP predicate must fail before allocating IR");
    CHECK_BAD_I1(anvil_build_add(ctx, truth, falsehood, "bad") != NULL,
                 "i1 arithmetic must be rejected");
    CHECK_BAD_I1(anvil_build_udiv(ctx, truth, falsehood, "bad") != NULL,
                 "i1 division must be rejected");
    CHECK_BAD_I1(anvil_build_shl(ctx, truth, falsehood, "bad") != NULL,
                 "i1 shifts must be rejected");
    CHECK_BAD_I1(anvil_build_cmp_ult(ctx, truth, falsehood, "bad") != NULL,
                 "i1 relational comparisons must be rejected");
    CHECK_BAD_I1(anvil_build_neg(ctx, truth, "bad") != NULL,
                 "i1 arithmetic negation must be rejected");
    CHECK_BAD_I1(anvil_build_gep(ctx, anvil_type_i32(ctx), gep_base,
                                 bool_index, 1, "bad") != NULL,
                 "i1 must not be accepted as a GEP index");
    CHECK_BAD_I1(anvil_build_switch(ctx, truth, fn->entry) != NULL,
                 "i1 must not be accepted as an integer switch selector");
    CHECK_BAD_I1(anvil_build_bitcast(ctx, truth, anvil_type_i8(ctx),
                                     "bad") != NULL,
                 "i1/i8 bitcast must not confuse equal storage size with bit width");
    CHECK_BAD_I1(anvil_build_br_cond(ctx, legacy_i8_cond,
                                     fn->entry, fn->entry),
                 "i8 must no longer be accepted as a branch condition");
    CHECK_BAD_I1(anvil_build_br_cond(ctx, legacy_u8_cond,
                                     fn->entry, fn->entry),
                 "u8 must no longer be accepted as a branch condition");
#undef CHECK_BAD_I1

    CHECK(anvil_build_ret(ctx, combined),
          "i1-returning function should accept an i1 value");
    char error[256] = { 0 };
    CHECK(anvil_module_verify(mod, error, sizeof(error)),
          "all FCMP predicates and i1 operations should verify");
    char *dump = anvil_module_to_string(mod);
    CHECK(dump && strstr(dump, "fcmp false") && strstr(dump, "fcmp oeq") &&
          strstr(dump, "fcmp uno") && strstr(dump, "fcmp true") &&
          strstr(dump, " i1") != NULL,
          "IR dump must print i1 and FCMP predicate mnemonics");
    free(dump);

    anvil_instr_t *first_fcmp = fn->entry->first;
    while (first_fcmp && first_fcmp->op != ANVIL_OP_FCMP)
        first_fcmp = first_fcmp->next;
    if (first_fcmp) {
        anvil_fcmp_pred_t saved = first_fcmp->fcmp_pred;
        first_fcmp->fcmp_pred = (anvil_fcmp_pred_t)-1;
        CHECK(!anvil_module_verify(mod, error, sizeof(error)) &&
              strstr(error, "predicate") != NULL,
              "verifier must independently reject a corrupted FCMP predicate");
        first_fcmp->fcmp_pred = saved;
    }
    truth->data.u = 2;
    CHECK(anvil_value_check_constant_dag(truth, ctx) ==
              ANVIL_CONST_DAG_INVALID &&
          !anvil_module_verify(mod, error, sizeof(error)),
          "i1 constants outside {0,1} must be rejected by DAG validation/verifier");
    truth->data.u = 1;

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_builder_without_insertion_point_is_owned_and_diagnostic(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "builder_no_ip", "no_ip");
    if (fn) {
        size_t before = owned_instr_count(ctx);
        anvil_set_insert_point(ctx, NULL);
        anvil_ctx_clear_error(ctx);
        anvil_value_t *result = anvil_build_add(
            ctx, anvil_const_i32(ctx, 1), anvil_const_i32(ctx, 2), "orphan");
        CHECK(result == NULL,
              "value builder without insertion point must report failure");
        CHECK(anvil_ctx_get_last_error(ctx) == ANVIL_ERR_INVALID_OP &&
              strstr(anvil_ctx_get_error(ctx), "insertion") != NULL,
              "missing insertion point should set a sticky diagnostic");
        CHECK(owned_instr_count(ctx) == before + 1,
              "uninserted instruction must remain in the ownership registry");
        CHECK(anvil_func_get_entry(fn)->first == NULL,
              "failed insertion must not mutate the live instruction list");

        anvil_ctx_clear_error(ctx);
        CHECK(anvil_ctx_get_last_error(ctx) == ANVIL_OK &&
              anvil_ctx_get_error(ctx)[0] == '\0',
              "public error clear should reset sticky builder status");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_phi_and_switch_growth_stays_synchronized(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "builder_growth", "growth");
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *dest = anvil_block_create(fn, "dest");
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "many");
        for (size_t i = 0; i < 33; i++) {
            anvil_block_t *pred = anvil_block_create(fn, NULL);
            CHECK(anvil_phi_add_incoming(
                      phi, anvil_const_i32(ctx, (int32_t)i), pred),
                  "PHI growth should succeed");
        }
        anvil_instr_t *phi_instr = phi ? phi->data.instr : NULL;
        CHECK(phi_instr && phi_instr->num_operands == 33 &&
              phi_instr->num_phi_incoming == 33 &&
              phi_instr->operands_capacity >= 33 &&
              phi_instr->phi_capacity >= 33,
              "PHI operand/block arrays must grow in lockstep");

        anvil_instr_t *switch_instr = anvil_build_switch(
            ctx, anvil_const_i32(ctx, 0), dest);
        for (size_t i = 0; i < 33; i++) {
            CHECK(anvil_switch_add_case(
                      switch_instr, anvil_const_i32(ctx, (int32_t)i), dest),
                  "switch growth should succeed");
        }
        CHECK(switch_instr && switch_instr->num_operands == 34 &&
              switch_instr->num_switch_cases == 33 &&
              switch_instr->operands_capacity >= 34 &&
              switch_instr->switch_capacity >= 33,
              "switch operand/destination arrays must grow in lockstep");
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_builder_allocation_failure_is_transactional(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "builder_oom", "oom");
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *dest = anvil_block_create(fn, "dest");
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_instr_t *phi_instr = phi ? phi->data.instr : NULL;

        anvil_value_t *failed_phi_value = anvil_const_i32(ctx, 7);
        anvil_ctx_clear_error(ctx);
        anvil_test_fail_alloc_after(ctx, 1);
        CHECK(!anvil_phi_add_incoming(phi, failed_phi_value, entry),
              "injected PHI operand allocation failure should be observable");
        anvil_test_disable_alloc_fail(ctx);
        CHECK(phi_instr && phi_instr->num_operands == 0 &&
              phi_instr->num_phi_incoming == 0,
              "failed PHI append must commit neither operand nor predecessor");
        CHECK(anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM,
              "PHI OOM should remain available through sticky status");

        anvil_ctx_clear_error(ctx);
        CHECK(anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 8), entry) &&
              phi_instr->num_operands == 1 &&
              phi_instr->num_phi_incoming == 1,
              "PHI should remain usable after transactional allocation failure");

        anvil_instr_t *switch_instr = anvil_build_switch(
            ctx, anvil_const_i32(ctx, 0), dest);
        for (int i = 0; i < 3; i++) {
            CHECK(anvil_switch_add_case(
                      switch_instr, anvil_const_i32(ctx, i), dest),
                  "switch setup should succeed");
        }
        size_t old_operands = switch_instr->num_operands;
        size_t old_cases = switch_instr->num_switch_cases;
        anvil_value_t *failed_switch_value = anvil_const_i32(ctx, 99);
        anvil_ctx_clear_error(ctx);
        anvil_test_fail_alloc_after(ctx, 0);
        CHECK(!anvil_switch_add_case(
                  switch_instr, failed_switch_value, dest),
              "injected switch operand growth failure should be observable");
        anvil_test_disable_alloc_fail(ctx);
        CHECK(switch_instr->num_operands == old_operands &&
              switch_instr->num_switch_cases == old_cases,
              "failed switch append must commit neither operand nor destination");
        CHECK(anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM,
              "switch OOM should remain available through sticky status");
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_data_layouts_and_target_freeze(void)
{
    struct layout_case {
        anvil_arch_t arch;
        size_t ptr_size, i64_align, field1, field2, struct_size;
    } cases[] = {
        { ANVIL_ARCH_X86_64, 8, 8, 8, 16, 24 },
        { ANVIL_ARCH_X86,    4, 4, 4, 12, 20 },
        { ANVIL_ARCH_PPC32,  4, 8, 8, 16, 24 },
    };

    for (size_t n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(cases[n].arch);
        CHECK(ctx != NULL, "target-specific layout context should be created");
        if (!ctx) continue;
        if (cases[n].arch == ANVIL_ARCH_X86_64) {
            CHECK(anvil_ctx_set_abi(ctx, ANVIL_ABI_WIN64) == ANVIL_OK,
                  "ABI may be selected before target-dependent types exist");
        }
        anvil_type_t *i32ptr = anvil_type_ptr(ctx, anvil_type_i32(ctx));
        anvil_type_t *array = anvil_type_array(ctx, anvil_type_i64(ctx), 3);
        anvil_type_t *fields[] = { anvil_type_i8(ctx), anvil_type_i64(ctx),
                                   anvil_type_f64(ctx) };
        anvil_type_t *record =
            anvil_type_literal_struct(ctx, fields, 3, false);
        const anvil_data_layout_t *dl = anvil_ctx_get_data_layout(ctx);
        CHECK(dl && dl->pointer.size == cases[n].ptr_size &&
              anvil_type_size(i32ptr) == cases[n].ptr_size,
              "pointer layout must follow the selected target");
        CHECK(dl && dl->i1.size == 1 && dl->i1.abi_align == 1 &&
              anvil_type_size(anvil_type_i1(ctx)) == 1 &&
              anvil_type_bit_width(anvil_type_i1(ctx)) == 1,
              "i1 storage and semantic width must be explicit on every target");
        CHECK(anvil_type_align(anvil_type_i64(ctx)) == cases[n].i64_align &&
              anvil_type_align(anvil_type_f64(ctx)) == cases[n].i64_align,
              "i64/f64 ABI alignment must follow the selected ABI");
        CHECK(anvil_type_size(array) == 24 &&
              anvil_type_align(array) == cases[n].i64_align,
              "array layout must inherit element size and ABI alignment");
        CHECK(anvil_type_struct_field_offset(record, 1) == cases[n].field1 &&
              anvil_type_struct_field_offset(record, 2) == cases[n].field2 &&
              anvil_type_size(record) == cases[n].struct_size,
              "aggregate offsets/tail padding must use DataLayout");
        anvil_arch_t before = anvil_ctx_get_target(ctx);
        anvil_arch_t other = before == ANVIL_ARCH_ARM64
                                 ? ANVIL_ARCH_X86 : ANVIL_ARCH_ARM64;
        CHECK(anvil_ctx_set_target(ctx, other) == ANVIL_ERR_INVALID_OP &&
              anvil_ctx_get_target(ctx) == before &&
              anvil_type_size(i32ptr) == cases[n].ptr_size,
              "retarget after composite creation must fail transactionally");
        anvil_abi_t frozen_abi = anvil_ctx_get_abi(ctx);
        anvil_abi_t other_abi = frozen_abi == ANVIL_ABI_DARWIN
                                   ? ANVIL_ABI_SYSV : ANVIL_ABI_DARWIN;
        CHECK(anvil_ctx_set_abi(ctx, other_abi) == ANVIL_ERR_INVALID_OP &&
              anvil_ctx_get_abi(ctx) == frozen_abi,
              "ABI/layout mutation after freeze must also be transactional");
        anvil_ctx_destroy(ctx);
    }
}

static void test_identified_recursive_structs(void)
{
    anvil_ctx_t *table_oom_ctx =
        anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    if (table_oom_ctx) {
        anvil_test_fail_alloc_after(table_oom_ctx, 2);
        CHECK(anvil_type_named_struct(table_oom_ctx, "TableOOM") == NULL,
              "named symbol-table allocation failure should be observable");
        anvil_test_disable_alloc_fail(table_oom_ctx);
        CHECK(anvil_type_named_struct(table_oom_ctx, "TableOOM") != NULL,
              "named table must remain usable after transactional allocation failure");
        anvil_ctx_destroy(table_oom_ctx);
    }

    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *node = anvil_type_named_struct(ctx, "Node");
    CHECK(node && anvil_type_struct_is_identified(node) &&
          anvil_type_struct_is_opaque(node) &&
          anvil_type_named_struct(ctx, "Node") == node,
          "named struct lookup must intern one opaque nominal type");
    anvil_type_t *node_ptr = anvil_type_ptr(ctx, node);
    anvil_type_t *node_fields[] = { anvil_type_i32(ctx), node_ptr };
    CHECK(anvil_type_struct_set_body(node, node_fields, 2, false),
          "self-recursive Node body through pointer should be accepted");
    CHECK(!anvil_type_struct_is_opaque(node) &&
          anvil_type_struct_field_count(node) == 2 &&
          anvil_type_struct_field_type(node, 1) == node_ptr &&
          anvil_type_struct_field_offset(node, 1) == 8 &&
          anvil_type_size(node) == 16,
          "recursive Node introspection/layout should be complete");
    CHECK(!anvil_type_struct_set_body(node, node_fields, 2, false),
          "identified struct body must not be redefined");

    anvil_type_t *a = anvil_type_named_struct(ctx, "A");
    anvil_type_t *b = anvil_type_named_struct(ctx, "B");
    anvil_type_t *a_fields[] = { anvil_type_ptr(ctx, b) };
    anvil_type_t *b_fields[] = { anvil_type_ptr(ctx, a) };
    CHECK(anvil_type_struct_set_body(a, a_fields, 1, false) &&
          anvil_type_struct_set_body(b, b_fields, 1, false),
          "mutually recursive nominal structs should be accepted through pointers");

    anvil_type_t *x = anvil_type_named_struct(ctx, "SameLayoutX");
    anvil_type_t *y = anvil_type_named_struct(ctx, "SameLayoutY");
    anvil_type_t *same[] = { anvil_type_i32(ctx) };
    CHECK(anvil_type_struct_set_body(x, same, 1, false) &&
          anvil_type_struct_set_body(y, same, 1, false) &&
          !anvil_types_equal(x, y),
          "distinct identified names must remain nominally unequal");
    anvil_ctx_clear_error(ctx);
    CHECK(anvil_type_struct(ctx, "SameLayoutX", same, 1) == NULL &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_INVALID_TYPE &&
          strstr(anvil_ctx_get_error(ctx), "only be set once") != NULL,
          "duplicate convenience definition must fail with a clear diagnostic");
    anvil_type_t *lit1 = anvil_type_literal_struct(ctx, same, 1, false);
    anvil_type_t *lit2 = anvil_type_literal_struct(ctx, same, 1, false);
    CHECK(anvil_types_equal(lit1, lit2),
          "literal structs with equal bodies must compare structurally");

    anvil_type_t *bad = anvil_type_named_struct(ctx, "BadByValue");
    anvil_type_t *bad_fields[] = { bad };
    CHECK(!anvil_type_struct_set_body(bad, bad_fields, 1, false) &&
          anvil_type_struct_is_opaque(bad),
          "direct unsized recursive fields must be rejected without partial body");

    anvil_type_t *oom = anvil_type_named_struct(ctx, "OOMBody");
    anvil_type_t *oom_fields[] = { anvil_type_i8(ctx), anvil_type_i64(ctx) };
    anvil_ctx_clear_error(ctx);
    anvil_test_fail_alloc_after(ctx, 1);
    CHECK(!anvil_type_struct_set_body(oom, oom_fields, 2, true),
          "injected body allocation failure should be reported");
    anvil_test_disable_alloc_fail(ctx);
    CHECK(anvil_type_struct_is_opaque(oom) &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM,
          "OOM must leave identified struct opaque and retryable");
    CHECK(anvil_type_struct_set_body(oom, oom_fields, 2, true) &&
          anvil_type_struct_is_packed(oom) &&
          anvil_type_align(oom) == 1 && anvil_type_size(oom) == 9 &&
          anvil_type_struct_field_offset(oom, 1) == 1,
          "packed body should succeed after transactional OOM recovery");

    anvil_module_t *dump_mod = anvil_module_create(ctx, "named_dump");
    char *type_dump = dump_mod ? anvil_module_to_string(dump_mod) : NULL;
    CHECK(type_dump &&
          strstr(type_dump, "%Node = type {i32, ptr<%Node>}") != NULL &&
          strstr(type_dump, "%OOMBody = type <{i8, i64}>") != NULL,
          "IR dump must emit recursive identified and packed type definitions");
    free(type_dump);
    anvil_module_destroy(dump_mod);

    anvil_ctx_destroy(ctx);
}

static void test_typed_gep_walk_and_struct_gep_contract(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "typed_gep", "typed_gep");
    if (!fn) { anvil_ctx_destroy(ctx); return; }
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));

    anvil_type_t *array = anvil_type_array(ctx, anvil_type_i64(ctx), 4);
    anvil_type_t *fields[] = { anvil_type_i32(ctx), array };
    anvil_type_t *record = anvil_type_literal_struct(ctx, fields, 2, false);
    anvil_value_t *base = anvil_build_alloca(ctx, record, "base");
    anvil_value_t *indices[] = { anvil_const_i32(ctx, 0),
                                 anvil_const_i32(ctx, 1),
                                 anvil_const_i32(ctx, 3) };
    anvil_value_t *element =
        anvil_build_gep(ctx, record, base, indices, 3, "nested.element");
    CHECK(element && anvil_types_equal(element->type->data.pointee,
                                       anvil_type_i64(ctx)),
          "typed GEP must infer nested struct/array result type");
    anvil_value_t *dynamic_bad[] = { anvil_const_i32(ctx, 0),
                                     anvil_build_add(ctx, anvil_const_i32(ctx, 0),
                                                     anvil_const_i32(ctx, 1), "dyn") };
    CHECK(anvil_build_gep(ctx, record, base, dynamic_bad, 2, "bad") == NULL,
          "struct walk must reject a dynamic field index");
    CHECK(anvil_build_struct_gep(ctx, record, base, 1, "array.field") != NULL,
          "struct_gep must accept ptr<same struct>");
    anvil_type_t *other = anvil_type_literal_struct(ctx, fields, 2, false);
    anvil_value_t *other_base = anvil_build_alloca(ctx, other, "other");
    CHECK(anvil_build_struct_gep(ctx, record, other_base, 1, "mismatch") != NULL,
          "structural literal equality should permit equivalent literal base types");
    anvil_value_t *scalar = anvil_build_alloca(ctx, anvil_type_i64(ctx), "scalar");
    anvil_value_t *huge_unsigned = anvil_const_u64(ctx, UINT64_MAX);
    CHECK(anvil_build_gep(ctx, anvil_type_i64(ctx), scalar,
                          &huge_unsigned, 1, "overflow") == NULL,
          "unsigned GEP index above INT64_MAX must not wrap negative");
    int64_t accumulated = INT64_MAX;
    CHECK(!anvil_gep_accumulate_offset(&accumulated, 1) &&
          accumulated == INT64_MAX,
          "constant GEP accumulated-offset overflow must be transactional");
    anvil_build_ret(ctx, anvil_const_i32(ctx, 0));
    char error[256] = { 0 };
    CHECK(anvil_module_verify(mod, error, sizeof(error)),
          "well-typed nested GEP should verify");
    char *dump = anvil_module_to_string(mod);
    CHECK(dump && strstr(dump, "gep ptr<i64> source {i32, [4 x i64]}") != NULL,
          "IR dump must preserve the formal GEP source element type");
    free(dump);

    anvil_instr_t *gep_instr = element->data.instr;
    anvil_type_t *saved = gep_instr->aux_type;
    gep_instr->aux_type = array;
    CHECK(!anvil_module_verify(mod, error, sizeof(error)),
          "verifier must independently reject a mismatched GEP source type");
    gep_instr->aux_type = saved;

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_module_symbol_table_and_redeclarations(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;
    anvil_module_t *mod = anvil_module_create(ctx, "symbols");
    anvil_type_t *params[] = { anvil_type_i32(ctx) };
    anvil_type_t *fn_type = anvil_type_func(ctx, anvil_type_i32(ctx),
                                            params, 1, false);
    anvil_func_t *decl = anvil_func_declare(mod, "typed_fn", fn_type);
    anvil_func_t *decl_again = anvil_func_declare(mod, "typed_fn", fn_type);
    CHECK(decl && decl_again == decl && anvil_module_symbol_count(mod) == 1 &&
          anvil_module_lookup_symbol(mod, "typed_fn") ==
              anvil_func_get_value(decl),
          "compatible repeated function declarations must reuse one symbol");

    anvil_value_t *stable_fn_value = anvil_func_get_value(decl);
    anvil_func_t *definition = anvil_func_create(
        mod, "typed_fn", fn_type, ANVIL_LINK_EXTERNAL);
    CHECK(definition == decl && !definition->is_declaration &&
          anvil_func_get_value(definition) == stable_fn_value &&
          anvil_func_get_param(definition, 0) != NULL &&
          anvil_func_get_entry(definition) != NULL && mod->num_funcs == 1,
          "declaration-to-definition must materialize the same function identity");
    if (definition) {
        anvil_set_insert_point(ctx, definition->entry);
        anvil_value_t *retired = anvil_build_add(
            ctx, definition->params[0], anvil_const_i32(ctx, 1), "retired");
        anvil_build_ret(ctx, definition->params[0]);
        CHECK(retired && anvil_pass_dce(definition) &&
              retired->data.instr->parent == NULL &&
              anvil_value_get_module(retired) == mod,
              "DCE should unlink dead results without losing module ownership");
    }
    CHECK(anvil_func_declare(mod, "typed_fn", fn_type) == definition,
          "a compatible declaration after definition must reuse it");
    CHECK(anvil_func_create(mod, "typed_fn", fn_type,
                            ANVIL_LINK_EXTERNAL) == NULL &&
          anvil_ctx_get_last_error(ctx) == ANVIL_ERR_INVALID_OP,
          "duplicate function definitions must be diagnosed");
    CHECK(anvil_func_create(mod, "typed_fn", fn_type,
                            ANVIL_LINK_INTERNAL) == NULL,
          "function linkage mismatches must be rejected");
    CHECK(anvil_module_add_global(mod, "typed_fn", anvil_type_i32(ctx),
                                  ANVIL_LINK_EXTERNAL) == NULL,
          "global/function names must share one namespace");

    anvil_func_t *weak_decl = anvil_func_declare(mod, "weak_fn", fn_type);
    anvil_func_t *weak_def = anvil_func_create(
        mod, "weak_fn", fn_type, ANVIL_LINK_WEAK);
    CHECK(weak_def == weak_decl && weak_def->linkage == ANVIL_LINK_WEAK,
          "external function declarations may become compatible weak definitions");
    if (weak_def) {
        anvil_set_insert_point(ctx, weak_def->entry);
        anvil_build_ret(ctx, weak_def->params[0]);
    }

    anvil_func_t *internal_decl = anvil_func_declare_linkage(
        mod, "internal_fn", fn_type, ANVIL_LINK_INTERNAL);
    CHECK(internal_decl && anvil_func_declare_linkage(
              mod, "internal_fn", fn_type, ANVIL_LINK_INTERNAL) == internal_decl,
          "compatible internal function declarations must reuse one symbol");
    anvil_func_t *internal_def = anvil_func_create(
        mod, "internal_fn", fn_type, ANVIL_LINK_INTERNAL);
    CHECK(internal_def == internal_decl &&
          anvil_func_declare_linkage(mod, "internal_fn", fn_type,
                                     ANVIL_LINK_INTERNAL) == internal_def,
          "internal declaration-to-definition-to-declaration must preserve identity");
    CHECK(anvil_func_declare(mod, "internal_fn", fn_type) == NULL,
          "external/internal function declaration mismatch must fail");
    if (internal_def) {
        anvil_set_insert_point(ctx, internal_def->entry);
        anvil_build_ret(ctx, internal_def->params[0]);
    }

    anvil_value_t *g_decl = anvil_module_add_extern(
        mod, "shared_global", anvil_type_i32(ctx));
    CHECK(g_decl && anvil_module_add_extern(
              mod, "shared_global", anvil_type_i32(ctx)) == g_decl,
          "compatible global extern declarations must reuse one symbol");
    anvil_value_t *g_def = anvil_module_add_global(
        mod, "shared_global", anvil_type_i32(ctx), ANVIL_LINK_WEAK);
    CHECK(g_def == g_decl && !g_def->data.global.is_declaration &&
          g_def->data.global.linkage == ANVIL_LINK_WEAK &&
          anvil_module_add_extern(mod, "shared_global",
                                  anvil_type_i32(ctx)) == g_def,
          "extern/weak global redeclarations must preserve symbol identity");
    CHECK(anvil_module_add_global(mod, "shared_global", anvil_type_i32(ctx),
                                  ANVIL_LINK_EXTERNAL) == NULL,
          "duplicate global definitions must be rejected");

    anvil_value_t *internal_g = anvil_module_declare_global(
        mod, "internal_global", anvil_type_i32(ctx), ANVIL_LINK_INTERNAL);
    CHECK(internal_g && anvil_module_add_global(
              mod, "internal_global", anvil_type_i32(ctx),
              ANVIL_LINK_INTERNAL) == internal_g &&
          anvil_module_declare_global(mod, "internal_global",
                                      anvil_type_i32(ctx),
                                      ANVIL_LINK_INTERNAL) == internal_g,
          "internal global declaration/definition/redeclaration must reuse identity");
    CHECK(anvil_module_add_extern(mod, "internal_global",
                                  anvil_type_i32(ctx)) == NULL,
          "external/internal global declaration mismatch must fail");

    anvil_value_t *common = anvil_module_declare_global(
        mod, "tentative", anvil_type_i32(ctx), ANVIL_LINK_COMMON);
    CHECK(common && anvil_module_declare_global(
              mod, "tentative", anvil_type_i32(ctx),
              ANVIL_LINK_COMMON) == common &&
          anvil_module_add_global(mod, "tentative", anvil_type_i32(ctx),
                                  ANVIL_LINK_COMMON) == common &&
          anvil_module_add_global(mod, "tentative", anvil_type_i32(ctx),
                                  ANVIL_LINK_COMMON) == common &&
          anvil_module_add_extern(mod, "tentative",
                                  anvil_type_i32(ctx)) == common,
          "compatible COMMON/tentative declarations must coalesce");

    for (size_t i = 0; i < 2048; i++) {
        char name[48];
        snprintf(name, sizeof(name), "collision_probe_%zu", i);
        CHECK(anvil_module_add_extern(mod, name, anvil_type_i32(ctx)) != NULL,
              "large symbol tables must survive hash collisions/growth");
    }
    CHECK(anvil_module_symbol_count(mod) == 2054,
          "symbol enumeration cardinality must exclude compatible redeclarations");
    for (size_t i = 0; i < anvil_module_symbol_count(mod); i++)
        CHECK(anvil_value_get_module(anvil_module_symbol_at(mod, i)) == mod,
              "enumerated symbols must carry explicit module ownership");

    anvil_module_t *other = anvil_module_create(ctx, "other_symbols");
    anvil_value_t *other_g = anvil_module_add_extern(
        other, "shared_global", anvil_type_i32(ctx));
    CHECK(other_g && other_g != g_def &&
          anvil_module_lookup_symbol(other, "shared_global") == other_g &&
          anvil_module_lookup_symbol(mod, "shared_global") == g_def,
          "same spelling in different modules must produce distinct owned symbols");

    anvil_module_t *oom_mod = anvil_module_create(ctx, "symbol_oom");
    anvil_test_fail_alloc_after(ctx, 0);
    CHECK(anvil_module_add_extern(oom_mod, "oom_global",
                                  anvil_type_i32(ctx)) == NULL &&
          anvil_module_symbol_count(oom_mod) == 0 &&
          anvil_module_lookup_symbol(oom_mod, "oom_global") == NULL,
          "symbol-table growth OOM must leave no partial symbol");
    anvil_test_disable_alloc_fail(ctx);
    CHECK(anvil_module_add_extern(oom_mod, "oom_global",
                                  anvil_type_i32(ctx)) != NULL,
          "symbol insertion must be retryable after OOM");

    anvil_func_t *oom_decl = anvil_func_declare(oom_mod, "oom_fn", fn_type);
    anvil_test_fail_alloc_after(ctx, 0);
    CHECK(oom_decl && anvil_func_create(oom_mod, "oom_fn", fn_type,
                                        ANVIL_LINK_EXTERNAL) == NULL &&
          oom_decl->is_declaration && oom_decl->params == NULL &&
          oom_decl->entry == NULL,
          "definition materialization OOM must preserve the declaration");
    anvil_test_disable_alloc_fail(ctx);
    CHECK(anvil_func_create(oom_mod, "oom_fn", fn_type,
                            ANVIL_LINK_EXTERNAL) == oom_decl,
          "definition materialization must be retryable after OOM");

    char error[256] = { 0 };
    CHECK(anvil_module_verify(mod, error, sizeof(error)),
          "module symbol table and redeclarations must verify");
    anvil_val_kind_t saved_kind = stable_fn_value->kind;
    stable_fn_value->kind = ANVIL_VAL_GLOBAL;
    CHECK(!anvil_module_verify(mod, error, sizeof(error)),
          "verifier must reject a function symbol with non-function value kind");
    stable_fn_value->kind = saved_kind;
    anvil_func_t *saved_func = stable_fn_value->data.func;
    stable_fn_value->data.func = NULL;
    CHECK(!anvil_module_verify(mod, error, sizeof(error)),
          "verifier must reject a function value with wrong back-reference");
    stable_fn_value->data.func = saved_func;
    anvil_module_t *saved_owner = stable_fn_value->owner_module;
    stable_fn_value->owner_module = other;
    CHECK(!anvil_module_verify(mod, error, sizeof(error)),
          "verifier must reject a function value with wrong owner module");
    stable_fn_value->owner_module = saved_owner;
    anvil_type_t *saved_callable_type = stable_fn_value->type;
    stable_fn_value->type = anvil_type_ptr(ctx, anvil_type_i32(ctx));
    CHECK(!anvil_module_verify(mod, error, sizeof(error)),
          "verifier must require ptr<exact function type> for callable symbols");
    stable_fn_value->type = saved_callable_type;
    anvil_linkage_t saved_linkage = definition->linkage;
    definition->linkage = ANVIL_LINK_COMMON;
    CHECK(!anvil_module_verify(mod, error, sizeof(error)),
          "verifier must reject COMMON linkage on a function");
    definition->linkage = saved_linkage;
    anvil_module_destroy(other);
    CHECK(anvil_value_get_module(other_g) == NULL,
          "destroying a module must invalidate symbol ownership tags");
    anvil_module_destroy(oom_mod);
    anvil_value_t *retired_handle = NULL;
    for (anvil_value_t *value = ctx->owned_values; value;
         value = value->ctx_next_owned) {
        if (value->name && strcmp(value->name, "retired") == 0) {
            retired_handle = value;
            break;
        }
    }
    anvil_module_destroy(mod);
    CHECK(retired_handle && anvil_value_get_module(retired_handle) == NULL,
          "module destroy must invalidate ownership of optimizer-retired results");
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
    test_ir_verifier_rejects_sibling_branch_use();
    test_ir_verifier_rejects_same_block_use_before_definition();
    test_ir_verifier_rejects_duplicate_phi_predecessor();
    test_ir_verifier_rejects_missing_phi_predecessor();
    test_ir_verifier_rejects_phi_value_not_dominating_edge();
    test_ir_verifier_rejects_unreachable_block();
    test_ir_verifier_accepts_dominated_uses_and_phi_edges();
    test_context_owns_constants_shared_across_modules();
    test_cross_context_types_and_values_are_rejected();
    test_global_initializer_requires_constant_dag();
    test_nested_and_malformed_array_constants();
    test_constant_dag_and_module_oom_diagnostics();
    test_id_exhaustion_and_verifier_oom();
    test_type_constructor_input_and_layout_guards();
    test_builders_reject_invalid_semantics_transactionally();
    test_i1_and_all_fcmp_predicates();
    test_builder_without_insertion_point_is_owned_and_diagnostic();
    test_phi_and_switch_growth_stays_synchronized();
    test_builder_allocation_failure_is_transactional();
    test_data_layouts_and_target_freeze();
    test_identified_recursive_structs();
    test_typed_gep_walk_and_struct_gep_contract();
    test_module_symbol_table_and_redeclarations();

    if (failures) {
        fprintf(stderr, "IR verifier regression tests failed: %d\n", failures);
        return 1;
    }

    printf("IR verifier regression tests passed\n");
    return 0;
}
