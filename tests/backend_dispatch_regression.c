/* Exercise family dispatch through the registered backend entry points. */

#include <anvil/anvil_internal.h>
#include <anvil/anvil_mainframe_mir.h>
#include <anvil/anvil_ppc_mir.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    anvil_arch_t arch;
    anvil_abi_t abi;
    anvil_syntax_t syntax;
    const char *entry_marker;
    const char *mode_marker;
} target_case_t;

static int failures;

static void check(bool condition, anvil_arch_t arch, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Target %d: %s\n", (int)arch, message);
        failures++;
    }
}

static void check_result(anvil_error_t error, char *text, size_t length, bool valid, const target_case_t *target, bool has_body)
{
    if (!valid) {
        check(error == ANVIL_ERR_CODEGEN, target->arch, "unsupported configuration must fail codegen");
        check(text == NULL && length == 0, target->arch, "failure must not publish partial assembly");
    } else {
        check(error == ANVIL_OK && text != NULL, target->arch, "supported configuration must emit");

        if (text) {
            check(length == strlen(text), target->arch, "reported assembly length must match output");

            if (has_body) {
                check(strstr(text, target->entry_marker) != NULL, target->arch, "wrong linkage entry sequence");
                check(strstr(text, target->mode_marker) != NULL, target->arch, "wrong target mode or ABI version");
            }
        }
    }

    free(text);
}

static void test_target_config(const target_case_t *target, unsigned config)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    check(ctx != NULL, target->arch, "create context");

    if (!ctx)
        return;

    check(anvil_ctx_set_target(ctx, target->arch) == ANVIL_OK, target->arch, "register target");
    check(anvil_ctx_set_abi(ctx, target->abi) == ANVIL_OK, target->arch, "select ABI");
    check(anvil_ctx_set_syntax(ctx, target->syntax) == ANVIL_OK, target->arch, "select syntax");

    if (config == 1) {
        anvil_abi_t unsupported = target->abi == ANVIL_ABI_MVS ? ANVIL_ABI_SYSV : ANVIL_ABI_MVS;
        check(anvil_ctx_set_abi(ctx, unsupported) == ANVIL_OK, target->arch, "configure unsupported ABI before creating IR");
        check(anvil_ctx_set_abi(ctx, target->abi) == ANVIL_OK, target->arch, "restore ABI for constructing valid IR");
    }

    if (config == 2) {
        anvil_syntax_t unsupported = target->syntax == ANVIL_SYNTAX_GAS ? ANVIL_SYNTAX_HLASM : ANVIL_SYNTAX_GAS;
        check(anvil_ctx_set_syntax(ctx, unsupported) == ANVIL_ERR_INVALID_ARG, target->arch, "public syntax setter must reject unsupported printer");

    }

    anvil_module_t *mod = anvil_module_create(ctx, "dispatch");
    check(mod != NULL, target->arch, "create module");

    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    /* Keep the source IR valid while independently testing backend boundaries.
     * Unsupported ABIs can also prevent construction of a function signature.
     */
    anvil_ctx_t codegen_ctx = *ctx;
    anvil_backend_t backend = *ctx->backend;
    backend.ctx = &codegen_ctx;

    if (config == 1)
        codegen_ctx.abi = target->abi == ANVIL_ABI_MVS ? ANVIL_ABI_SYSV : ANVIL_ABI_MVS;

    if (config == 2)
        codegen_ctx.syntax = target->syntax == ANVIL_SYNTAX_GAS ? ANVIL_SYNTAX_HLASM : ANVIL_SYNTAX_GAS;

    char *text = NULL;
    size_t length = 123;
    anvil_error_t error = backend.ops->codegen_module(&backend, mod, &text, &length);
    check_result(error, text, length, config == 0, target, false);

    anvil_type_t *integer = anvil_type_i32(ctx);
    anvil_type_t *params[] = {integer};
    anvil_type_t *signature = anvil_type_func(ctx, integer, params, 1, false);
    anvil_func_t *declaration = anvil_func_declare(mod, "callee", signature);
    check(declaration != NULL, target->arch, "create declaration");

    if (declaration) {
        text = NULL;
        length = 123;
        error = backend.ops->codegen_func(&backend, declaration, &text, &length);
        check_result(error, text, length, config == 0, target, false);
    }

    anvil_func_t *function = anvil_func_create(mod, "probe", signature, ANVIL_LINK_EXTERNAL);
    check(function != NULL, target->arch, "create function");

    if (function) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(function));
        anvil_build_ret(ctx, anvil_func_get_param(function, 0));

        text = NULL;
        length = 123;
        error = backend.ops->codegen_func(&backend, function, &text, &length);
        check_result(error, text, length, config == 0, target, true);

        text = NULL;
        length = 123;
        error = backend.ops->codegen_module(&backend, mod, &text, &length);
        check_result(error, text, length, config == 0, target, true);
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    const target_case_t targets[] = {{ANVIL_ARCH_PPC32, ANVIL_ABI_SYSV, ANVIL_SYNTAX_GAS, "probe:", "\t.text"},
                                     {ANVIL_ARCH_PPC64, ANVIL_ABI_SYSV, ANVIL_SYNTAX_GAS, ".quad .L.probe,.TOC.@tocbase,0", ".abiversion 1"},
                                     {ANVIL_ARCH_PPC64LE, ANVIL_ABI_SYSV, ANVIL_SYNTAX_GAS, ".localentry probe", ".abiversion 2"},
                                     {ANVIL_ARCH_S370, ANVIL_ABI_MVS, ANVIL_SYNTAX_HLASM, "STM   R14,R12,12(R13)", "AMODE 24"},
                                     {ANVIL_ARCH_S370_XA, ANVIL_ABI_MVS, ANVIL_SYNTAX_HLASM, "STM   R14,R12,12(R13)", "AMODE 31"},
                                     {ANVIL_ARCH_S390, ANVIL_ABI_MVS, ANVIL_SYNTAX_HLASM, "STM   R14,R12,12(R13)", "AMODE 31"},
                                     {ANVIL_ARCH_ZARCH, ANVIL_ABI_MVS, ANVIL_SYNTAX_HLASM, "STMG  R14,R12,24(R13)", "AMODE 64"}};

    for (size_t index = 0; index < sizeof(targets) / sizeof(targets[0]); index++) {
        for (unsigned config = 0; config < 3; config++)
            test_target_config(&targets[index], config);
    }

    check(anvil_ppc_get_target_desc((anvil_ppc_variant_t)-1) == NULL, ANVIL_ARCH_PPC32, "reject invalid PPC variant");
    check(anvil_mainframe_get_target_desc((anvil_mainframe_variant_t)-1) == NULL, ANVIL_ARCH_S370, "reject invalid IBM ISA variant");

    if (failures)
        return 1;

    puts("Backend dispatch: 7 targets, 3 configurations, 4 entry cases passed");
    return 0;
}
