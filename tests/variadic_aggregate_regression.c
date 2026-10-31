/* Aggregates as unnamed (variadic) arguments: the call-site attribute API,
 * its verification, and every target's va_arg of an aggregate. */
#include <anvil/anvil.h>

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

typedef struct { anvil_arch_t arch; anvil_abi_t abi; } target_t;

/* long sink(int n, ...): reads one struct { long long a, b; } with va_arg;
 * caller(): sink(1, pair) with the pair BYVAL at the call site. */
static void test_target(const target_t *t)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx && anvil_ctx_set_target(ctx, t->arch) == ANVIL_OK && anvil_ctx_set_abi(ctx, t->abi) == ANVIL_OK, "target should be selectable");
    anvil_module_t *mod = anvil_module_create(ctx, "variadic_aggregates");
    anvil_type_t *i32 = anvil_type_i32(ctx), *i64 = anvil_type_i64(ctx);
    anvil_type_t *fields[] = { i64, i64 };
    anvil_type_t *pair = anvil_type_struct(ctx, "pair", fields, 2);
    anvil_type_t *named[] = { i32 };
    anvil_type_t *sink_type = anvil_type_func(ctx, i64, named, 1, true);

    anvil_func_t *sink = anvil_func_create(mod, "sink", sink_type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(sink));
    anvil_type_t *cursor = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_value_t *storage = anvil_build_alloca(ctx, cursor, "ap");
    anvil_build_store(ctx, anvil_build_va_start(ctx, "start"), storage);
    anvil_value_t *slot = anvil_build_va_arg(ctx, storage, pair, "pair.slot");
    CHECK(slot != NULL, "va_arg of an aggregate should build on every target");
    if (slot) {
        anvil_value_t *second = anvil_build_struct_gep(ctx, pair, slot, 1, "second");
        anvil_build_ret(ctx, anvil_build_load(ctx, i64, second, "b"));
    }

    anvil_func_t *caller = anvil_func_create(mod, "caller", anvil_type_func(ctx, i64, NULL, 0, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
    anvil_value_t *object = anvil_build_alloca(ctx, pair, "object");
    anvil_value_t *args[] = { anvil_const_i32(ctx, 1), object };
    anvil_param_attr_t attrs[] = { ANVIL_PARAM_PLAIN, ANVIL_PARAM_BYVAL };
    anvil_value_t *result = NULL;
    CHECK(anvil_build_call_attrs(ctx, anvil_func_get_value(sink), args, attrs, 2, "r", &result) && anvil_build_ret(ctx, result),
          "an unnamed aggregate should pass BYVAL");

    char *text = NULL;
    size_t len = 0;
    CHECK(anvil_module_codegen(mod, &text, &len) == ANVIL_OK && text, "variadic aggregates should generate code");
    free(text);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_call_site_rules(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_X86_64);
    anvil_module_t *mod = anvil_module_create(ctx, "call_site_rules");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *named[] = { i32 };
    anvil_func_t *callee = anvil_func_declare(mod, "callee", anvil_type_func(ctx, i32, named, 1, true));
    anvil_func_t *fn = anvil_func_create(mod, "rules", anvil_type_func(ctx, i32, NULL, 0, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *one = anvil_const_i32(ctx, 1), *result = NULL;

    anvil_value_t *scalar_args[] = { one, one };
    anvil_param_attr_t byval_scalar[] = { ANVIL_PARAM_PLAIN, ANVIL_PARAM_BYVAL };
    CHECK(!anvil_build_call_attrs(ctx, anvil_func_get_value(callee), scalar_args, byval_scalar, 2, "bad", &result),
          "BYVAL needs a pointer to an aggregate");
    anvil_param_attr_t contradict[] = { ANVIL_PARAM_BYVAL, ANVIL_PARAM_PLAIN };
    CHECK(!anvil_build_call_attrs(ctx, anvil_func_get_value(callee), scalar_args, contradict, 2, "bad", &result),
          "a declared parameter keeps its type's attribute");
    anvil_param_attr_t sret[] = { ANVIL_PARAM_PLAIN, ANVIL_PARAM_SRET };
    CHECK(!anvil_build_call_attrs(ctx, anvil_func_get_value(callee), scalar_args, sret, 2, "bad", &result),
          "an unnamed argument cannot be SRET");
    CHECK(anvil_build_call_attrs(ctx, anvil_func_get_value(callee), scalar_args, NULL, 2, "plain", &result) && anvil_build_ret(ctx, result),
          "no attributes is a plain call");
    char error[256] = { 0 };
    CHECK(anvil_module_verify(mod, error, sizeof(error)), "rejected builds leave a verifiable module");
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    static const target_t targets[] = {
        { ANVIL_ARCH_X86_64, ANVIL_ABI_SYSV }, { ANVIL_ARCH_X86_64, ANVIL_ABI_WIN64 }, { ANVIL_ARCH_X86_64, ANVIL_ABI_DARWIN },
        { ANVIL_ARCH_X86, ANVIL_ABI_SYSV }, { ANVIL_ARCH_ARM64, ANVIL_ABI_SYSV }, { ANVIL_ARCH_ARM64, ANVIL_ABI_DARWIN },
        { ANVIL_ARCH_PPC32, ANVIL_ABI_SYSV }, { ANVIL_ARCH_PPC64, ANVIL_ABI_SYSV }, { ANVIL_ARCH_PPC64LE, ANVIL_ABI_SYSV },
        { ANVIL_ARCH_S370, ANVIL_ABI_MVS }, { ANVIL_ARCH_S390, ANVIL_ABI_MVS }, { ANVIL_ARCH_ZARCH, ANVIL_ABI_MVS },
    };
    test_call_site_rules();
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
        test_target(&targets[i]);
    if (failures) {
        fprintf(stderr, "%d variadic aggregate check(s) failed\n", failures);
        return 1;
    }
    printf("variadic aggregate regression tests passed\n");
    return 0;
}
