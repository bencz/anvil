#include <anvil/anvil_internal.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool check_contracts(anvil_arch_t architecture)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(architecture);
    bool valid = anvil_ctx_set_abi(ctx, ANVIL_ABI_DEFAULT) == ANVIL_OK && anvil_ctx_get_abi(ctx) == ANVIL_ABI_SYSV;
    anvil_module_t *module = anvil_module_create(ctx, "atomic_contracts");
    anvil_type_t *integer = anvil_type_u64(ctx);
    anvil_type_t *pointer = anvil_type_ptr(ctx, integer);
    anvil_type_t *type = anvil_type_func(ctx, integer, &pointer, 1, false);
    anvil_func_t *func = anvil_func_create(module, "observe", type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(func));
    anvil_value_t *address = anvil_func_get_param(func, 0);
    anvil_value_t *one = anvil_const_u64(ctx, 1);
    valid &= anvil_ctx_set_abi(ctx, ANVIL_ABI_DEFAULT) == ANVIL_OK;
    valid &= anvil_atomic_is_lock_free(ctx, integer) && !anvil_atomic_is_lock_free(ctx, anvil_type_f64(ctx));
    valid &= anvil_build_atomic_load(ctx, address, ANVIL_ORDER_RELEASE, "invalid") == NULL;
    anvil_ctx_clear_error(ctx);
    valid &= !anvil_build_atomic_store(ctx, one, address, ANVIL_ORDER_ACQUIRE);
    anvil_ctx_clear_error(ctx);
    valid &= anvil_build_atomic_cmpxchg(ctx, address, one, one, ANVIL_ORDER_RELEASE, ANVIL_ORDER_ACQUIRE, "invalid") == NULL;
    anvil_ctx_clear_error(ctx);

    anvil_build_store(ctx, one, address);
    anvil_value_t *before = anvil_build_load(ctx, integer, address, "before");
    anvil_build_atomic_rmw(ctx, ANVIL_ATOMIC_ADD, address, one, ANVIL_ORDER_RELAXED, "unused");
    anvil_value_t *after = anvil_build_load(ctx, integer, address, "after");
    anvil_build_atomic_fence(ctx, ANVIL_ORDER_SEQ_CST);
    anvil_build_store(ctx, anvil_const_u64(ctx, 42), address);
    anvil_build_ret(ctx, anvil_build_add(ctx, before, after, "sum"));

    anvil_pass_manager_t *manager = anvil_pass_manager_create(ctx);
    anvil_pass_manager_set_level(manager, ANVIL_OPT_AGGRESSIVE);
    valid &= anvil_pass_manager_run_func(manager, func) != ANVIL_PASS_RUN_ERROR;
    size_t atomics = 0;
    bool kept_after = false;
    for (anvil_instr_t *instr = func->entry->first; instr; instr = instr->next)
    {
        atomics += anvil_op_is_atomic(instr->op);
        kept_after |= instr->result == after;
    }
    valid &= atomics == 2 && kept_after;

    char *assembly = NULL;
    size_t length = 0;
    valid &= anvil_module_codegen(module, &assembly, &length) == ANVIL_OK;
    if (architecture == ANVIL_ARCH_ARM64)
        valid &= assembly && strstr(assembly, "ldxr") && strstr(assembly, "stxr") && strstr(assembly, "dmb ish");
    else
        valid &= assembly && strstr(assembly, "lock xaddq") && strstr(assembly, "mfence");
    if (!valid)
        fprintf(stderr, "atomic contracts failed: %s\n", anvil_ctx_get_error(ctx));

    free(assembly);
    anvil_pass_manager_destroy(manager);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

int main(void)
{
    bool x64 = check_contracts(ANVIL_ARCH_X86_64);
    bool arm64 = check_contracts(ANVIL_ARCH_ARM64);
    return x64 && arm64 ? 0 : 1;
}
