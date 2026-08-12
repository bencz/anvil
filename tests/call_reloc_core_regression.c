#include "anvil/anvil.h"
#include "anvil/anvil_debug.h"
#include "anvil/anvil_internal.h"
#include "anvil/anvil_machine.h"
#include "anvil/anvil_x86_64_mir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, msg) do {                                                   \
    if (!(cond)) {                                                              \
        fprintf(stderr, "FAIL:%d: %s\n", __LINE__, (msg));                    \
        failures++;                                                             \
    }                                                                           \
} while (0)

static void check_x64_vtable_runtime(const char *assembly)
{
#if defined(__x86_64__) && !defined(_WIN32)
    char asm_path[128], c_path[128], exe_path[128], command[512];
    long pid = (long)getpid();
    snprintf(asm_path, sizeof(asm_path), "/tmp/anvil-vtable-%ld.s", pid);
    snprintf(c_path, sizeof(c_path), "/tmp/anvil-vtable-%ld.c", pid);
    snprintf(exe_path, sizeof(exe_path), "/tmp/anvil-vtable-%ld", pid);
    FILE *asm_file = fopen(asm_path, "w");
    FILE *c_file = fopen(c_path, "w");
    CHECK(asm_file && c_file, "VTable runtime temporary files should open");
    if (!asm_file || !c_file) {
        if (asm_file) fclose(asm_file);
        if (c_file) fclose(c_file);
        unlink(asm_path); unlink(c_path);
        return;
    }
    fputs(assembly, asm_file);
    fputs("extern int dispatch_slot_1(void *, int);\n"
          "int main(void) { return dispatch_slot_1((void *)0, 40) == 42 ? 0 : 1; }\n",
          c_file);
    CHECK(fclose(asm_file) == 0 && fclose(c_file) == 0,
          "VTable runtime temporary files should flush");
    snprintf(command, sizeof(command),
             "cc -no-pie %s %s -o %s >/dev/null 2>&1",
             asm_path, c_path, exe_path);
    CHECK(system(command) == 0,
          "generated x64 VTable assembly should link with a C harness");
    snprintf(command, sizeof(command), "%s", exe_path);
    CHECK(system(command) == 0,
          "x64 indirect VTable dispatch should execute the selected method");
    unlink(asm_path); unlink(c_path); unlink(exe_path);
#else
    (void)assembly;
#endif
}

static void cross_assemble_data(const char *assembly, const char *target,
                                const char *message)
{
    if (!assembly || !target || access("/usr/bin/clang", X_OK) != 0) return;
    char source[] = "/tmp/anvil-vtable-data-XXXXXX.s";
    int fd = mkstemps(source, 2);
    if (fd < 0) { CHECK(false, message); return; }
    FILE *file = fdopen(fd, "wb");
    bool ok = file && fputs(assembly, file) >= 0;
    if (file) ok = fclose(file) == 0 && ok;
    else close(fd);
    char object[160], command[512];
    snprintf(object, sizeof(object), "%s.o", source);
    snprintf(command, sizeof(command),
             "/usr/bin/clang --target=%s -c %s -o %s >/dev/null 2>&1",
             target, source, object);
    if (ok) ok = system(command) == 0;
    CHECK(ok, message);
    unlink(source); unlink(object);
}

static anvil_ctx_t *new_ctx(anvil_arch_t arch)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    CHECK(ctx != NULL, "target context creation should succeed");
    return ctx;
}

static size_t owned_value_count(const anvil_ctx_t *ctx)
{
    size_t count = 0;
    for (const anvil_value_t *value = ctx ? ctx->owned_values : NULL;
         value; value = value->ctx_next_owned) count++;
    return count;
}

static size_t owned_instr_count(const anvil_ctx_t *ctx)
{
    size_t count = 0;
    for (const anvil_instr_t *instr = ctx ? ctx->owned_instrs : NULL;
         instr; instr = instr->ctx_next_owned) count++;
    return count;
}

static void test_cc_canonicalization_matrix(void)
{
    anvil_ctx_t *ctx = new_ctx(ANVIL_ARCH_X86);
    if (ctx) {
        anvil_type_t *a = anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 0,
                                          false);
        anvil_type_t *b = anvil_type_func_cc(
            ctx, anvil_type_i32(ctx), NULL, 0, false, ANVIL_CC_CDECL);
        anvil_type_t *stdcall = anvil_type_func_cc(
            ctx, anvil_type_i32(ctx), NULL, 0, false, ANVIL_CC_STDCALL);
        CHECK(a && b && stdcall, "x86 supported CCs should construct");
        CHECK(anvil_type_func_cc_value(a) == ANVIL_CC_CDECL &&
              anvil_types_equal(a, b),
              "x86 DEFAULT must canonicalize to CDECL");
        CHECK(!anvil_types_equal(a, stdcall),
              "calling convention must participate in function type equality");
        CHECK(anvil_type_func_cc(ctx, anvil_type_i32(ctx), NULL, 0, true,
                                 ANVIL_CC_STDCALL) == NULL &&
              anvil_type_func_cc(ctx, anvil_type_i32(ctx), NULL, 0, true,
                                 ANVIL_CC_FASTCALL) == NULL,
              "x86 variadic signatures must reject callee-cleanup conventions");
        CHECK(anvil_type_func_cc(ctx, anvil_type_i32(ctx), NULL, 0, false,
                                 ANVIL_CC_SYSV) == NULL,
              "x86 must reject the x64 SYSV convention instead of falling back");
        anvil_ctx_destroy(ctx);
    }

    ctx = new_ctx(ANVIL_ARCH_X86);
    if (ctx) {
        CHECK(anvil_ctx_set_abi(ctx, ANVIL_ABI_WIN64) == ANVIL_OK,
              "x86 must retain its Win32 COFF platform selection");
        CHECK(anvil_type_func_cc(ctx, anvil_type_i32(ctx), NULL, 0, false,
                                 ANVIL_CC_STDCALL) != NULL &&
              anvil_type_func_cc(ctx, anvil_type_i32(ctx), NULL, 0, false,
                                 ANVIL_CC_FASTCALL) != NULL,
              "x86 COFF must support stdcall and fastcall signatures");
        anvil_ctx_destroy(ctx);
    }

    ctx = new_ctx(ANVIL_ARCH_X86_64);
    if (ctx) {
        anvil_type_t *def = anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 0,
                                            false);
        anvil_type_t *alias = anvil_type_func_cc(
            ctx, anvil_type_i32(ctx), NULL, 0, false, ANVIL_CC_CDECL);
        anvil_type_t *sysv = anvil_type_func_cc(
            ctx, anvil_type_i32(ctx), NULL, 0, false, ANVIL_CC_SYSV);
        CHECK(def && alias && sysv && anvil_types_equal(def, alias) &&
              anvil_types_equal(def, sysv) &&
              anvil_type_func_cc_value(def) == ANVIL_CC_SYSV,
              "x64 SysV DEFAULT/CDECL aliases must canonicalize identically");
        CHECK(anvil_type_func_cc(ctx, anvil_type_i32(ctx), NULL, 0, false,
                                 ANVIL_CC_WIN64) == NULL,
              "SysV x64 target must reject explicit Win64");
        anvil_ctx_destroy(ctx);
    }

    ctx = new_ctx(ANVIL_ARCH_X86_64);
    if (ctx) {
        CHECK(anvil_ctx_set_abi(ctx, ANVIL_ABI_WIN64) == ANVIL_OK,
              "x64 ABI should be configurable before composite IR exists");
        anvil_type_t *def = anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 0,
                                            false);
        anvil_type_t *alias = anvil_type_func_cc(
            ctx, anvil_type_i32(ctx), NULL, 0, false, ANVIL_CC_CDECL);
        CHECK(def && alias && anvil_types_equal(def, alias) &&
              anvil_type_func_cc_value(def) == ANVIL_CC_WIN64,
              "Win64 DEFAULT/CDECL aliases must canonicalize to Win64");
        CHECK(anvil_type_func_cc(ctx, anvil_type_i32(ctx), NULL, 0, false,
                                 ANVIL_CC_SYSV) == NULL,
              "Win64 target must reject explicit SysV");
        anvil_ctx_destroy(ctx);
    }

    const anvil_arch_t sysv_arches[] = {
        ANVIL_ARCH_ARM64, ANVIL_ARCH_PPC32,
        ANVIL_ARCH_PPC64, ANVIL_ARCH_PPC64LE
    };
    for (size_t i = 0; i < sizeof(sysv_arches) / sizeof(sysv_arches[0]); i++) {
        ctx = new_ctx(sysv_arches[i]);
        if (!ctx) continue;
        anvil_type_t *type = anvil_type_func(
            ctx, anvil_type_i32(ctx), NULL, 0, false);
        CHECK(type && anvil_type_func_cc_value(type) == ANVIL_CC_SYSV,
              "ARM/PPC DEFAULT must canonicalize to the target SysV ABI class");
        CHECK(anvil_type_func_cc(ctx, anvil_type_i32(ctx), NULL, 0, false,
                                 ANVIL_CC_STDCALL) == NULL,
              "ARM/PPC must reject x86-only calling conventions");
        anvil_ctx_destroy(ctx);
    }

    const anvil_arch_t mainframe_arches[] = {
        ANVIL_ARCH_S370, ANVIL_ARCH_S370_XA,
        ANVIL_ARCH_S390, ANVIL_ARCH_ZARCH
    };
    for (size_t i = 0;
         i < sizeof(mainframe_arches) / sizeof(mainframe_arches[0]); i++) {
        ctx = new_ctx(mainframe_arches[i]);
        if (!ctx) continue;
        anvil_type_t *type = anvil_type_func(
            ctx, anvil_type_i32(ctx), NULL, 0, false);
        CHECK(type && anvil_type_func_cc_value(type) == ANVIL_CC_MVS,
              "mainframe DEFAULT must canonicalize to MVS");
        CHECK(anvil_type_func_cc(ctx, anvil_type_i32(ctx), NULL, 0, false,
                                 ANVIL_CC_CDECL) == NULL,
              "mainframe must reject CDECL instead of silently treating it as MVS");
        anvil_ctx_destroy(ctx);
    }
}

static anvil_func_t *define_method(anvil_module_t *mod, const char *name,
                                   anvil_type_t *method_type, int delta)
{
    anvil_ctx_t *ctx = mod->ctx;
    anvil_func_t *func = anvil_func_create(mod, name, method_type,
                                            ANVIL_LINK_INTERNAL);
    if (!func) return NULL;
    anvil_set_insert_point(ctx, anvil_func_get_entry(func));
    anvil_value_t *arg = anvil_func_get_param(func, 1);
    anvil_value_t *result = delta == 0
        ? arg : anvil_build_add(ctx, arg, anvil_const_i32(ctx, delta), "sum");
    if (!result || !anvil_build_ret(ctx, result)) return NULL;
    return func;
}

static void test_static_vtable_and_checked_calls(void)
{
    anvil_ctx_t *ctx = new_ctx(ANVIL_ARCH_X86_64);
    if (!ctx) return;
    anvil_module_t *mod = anvil_module_create(ctx, "vtable.core");
    anvil_module_t *other = anvil_module_create(ctx, "other");
    anvil_type_t *receiver = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *method_params[] = { receiver, anvil_type_i32(ctx) };
    anvil_type_t *method_type = anvil_type_func_cc(
        ctx, anvil_type_i32(ctx), method_params, 2, false, ANVIL_CC_SYSV);
    anvil_type_t *method_ptr = anvil_type_ptr(ctx, method_type);
    anvil_func_t *plus_one = define_method(mod, "plus_one", method_type, 1);
    anvil_func_t *plus_two = define_method(mod, "plus_two", method_type, 2);
    CHECK(mod && other && receiver && method_type && method_ptr &&
          plus_one && plus_two, "uniform VTable method setup should succeed");
    if (!plus_one || !plus_two) { anvil_ctx_destroy(ctx); return; }

    anvil_value_t *addr_one = anvil_const_symbol_addr(
        anvil_func_get_value(plus_one));
    anvil_value_t *addr_two = anvil_const_symbol_addr(
        anvil_func_get_value(plus_two));
    anvil_type_t *vtable_fields[] = { method_ptr, method_ptr };
    anvil_type_t *vtable_type = anvil_type_literal_struct(
        ctx, vtable_fields, 2, false);
    anvil_value_t *vtable_values[] = { addr_one, addr_two };
    anvil_value_t *vtable_init = anvil_const_struct(
        ctx, vtable_type, vtable_values, 2);
    anvil_value_t *vtable = anvil_module_add_global(
        mod, "SmallInteger.vtable", vtable_type, ANVIL_LINK_INTERNAL);
    CHECK(addr_one && addr_two && vtable_init && vtable &&
          vtable_init->owner_module == mod &&
          anvil_global_set_initializer(vtable, vtable_init),
          "static VTable must retain exact-module relocations");

    anvil_value_t *vtable_addr = anvil_const_symbol_addr(vtable);
    anvil_value_t *indices[] = {
        anvil_const_i32(ctx, 0), anvil_const_i32(ctx, 1)
    };
    anvil_value_t *slot_addr = anvil_const_gep(
        vtable_addr, vtable_type, indices, 2);
    CHECK(slot_addr && slot_addr->kind == ANVIL_VAL_CONST_GEP &&
          slot_addr->data.reloc.symbol == vtable &&
          slot_addr->data.reloc.addend ==
              (int64_t)anvil_type_struct_field_offset(vtable_type, 1) &&
          slot_addr->type->kind == ANVIL_TYPE_PTR &&
          anvil_types_equal(slot_addr->type->data.pointee, method_ptr),
          "typed constant GEP must preserve provenance, slot type, and exact addend");

    anvil_type_t *dispatch_type = anvil_type_func_cc(
        ctx, anvil_type_i32(ctx), method_params, 2, false, ANVIL_CC_SYSV);
    anvil_func_t *dispatch = anvil_func_create(
        mod, "dispatch_slot_1", dispatch_type, ANVIL_LINK_EXTERNAL);
    CHECK(dispatch != NULL, "dispatch function should be created");
    anvil_value_t *call_result = NULL;
    anvil_instr_t *call_instr = NULL;
    if (dispatch && slot_addr) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(dispatch));
        anvil_value_t *loaded = anvil_build_load(
            ctx, method_ptr, slot_addr, "method");
        anvil_value_t *args[] = {
            anvil_func_get_param(dispatch, 0),
            anvil_func_get_param(dispatch, 1)
        };
        CHECK(loaded && anvil_build_call_checked(
                  ctx, loaded, args, 2, "dispatched", &call_result) &&
              call_result && anvil_build_ret(ctx, call_result),
              "VTable slot load and indirect checked call should build");
        call_instr = call_result ? call_result->data.instr : NULL;
        CHECK(call_instr && call_instr->call_cc == ANVIL_CC_SYSV,
              "indirect call must preserve the method signature's effective CC");
    }

    char error[256] = { 0 };
    CHECK(anvil_module_verify(mod, error, sizeof(error)),
          error[0] ? error : "complete VTable module should verify");

    anvil_type_t *variadic_params[] = { anvil_type_i32(ctx) };
    anvil_type_t *variadic_type = anvil_type_func(
        ctx, anvil_type_void(ctx), variadic_params, 1, true);
    anvil_func_t *variadic_decl = anvil_func_declare(
        mod, "trace_values", variadic_type);
    anvil_type_t *zero_fixed_variadic_type = anvil_type_func(
        ctx, anvil_type_void(ctx), NULL, 0, true);
    anvil_func_t *zero_fixed_variadic_decl = anvil_func_declare(
        mod, "trace_any", zero_fixed_variadic_type);
    CHECK(variadic_decl && zero_fixed_variadic_decl,
          "variadic function signatures should construct and declare");

    char *dump = anvil_module_to_string(mod);
    CHECK(dump && strstr(dump, "cc(sysv)") &&
          strstr(dump, "addr(@plus_one)") &&
          strstr(dump, "reloc(@SmallInteger.vtable+8)") &&
          strstr(dump, "@trace_any(...)"),
          "IR dump must expose effective CC and relocatable provenance/addend");
    free(dump);

    anvil_func_t *other_method = define_method(
        other, "foreign_method", method_type, 3);
    anvil_value_t *foreign_addr = other_method
        ? anvil_const_symbol_addr(anvil_func_get_value(other_method)) : NULL;
    anvil_value_t *mixed[] = { foreign_addr, addr_one };
    CHECK(anvil_const_array(ctx, method_ptr, mixed, 2) == NULL,
          "aggregate constants must reject cross-module relocations");
    anvil_value_t *foreign_values[] = { foreign_addr, foreign_addr };
    anvil_value_t *foreign_init = anvil_const_struct(
        ctx, vtable_type, foreign_values, 2);
    CHECK(foreign_init && foreign_init->owner_module == other &&
          !anvil_global_set_initializer(vtable, foreign_init),
          "global initializer must reject a relocation owned by another module");

    if (slot_addr) {
        int64_t saved = slot_addr->data.reloc.addend;
        slot_addr->data.reloc.addend++;
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "verifier must recompute and reject a corrupted relocation addend");
        slot_addr->data.reloc.addend = saved;
    }
    if (call_instr) {
        anvil_cc_t saved = call_instr->call_cc;
        call_instr->call_cc = ANVIL_CC_WIN64;
        CHECK(!anvil_module_verify(mod, error, sizeof(error)),
              "verifier must reject a call-site CC that differs from ptr<func>");
        call_instr->call_cc = saved;
    }
    CHECK(anvil_module_verify(mod, error, sizeof(error)),
          "restored VTable IR should verify again");

    anvil_mir_func_t *dispatch_mir = dispatch
        ? anvil_x86_64_lower_func_to_mir(dispatch) : NULL;
    bool saw_sysv_indirect_call = false;
    for (size_t i = 0; dispatch_mir &&
                       i < anvil_mir_num_instrs(dispatch_mir); i++) {
        anvil_mir_instr_info_t info;
        if (anvil_mir_get_instr_info(dispatch_mir, i, &info) &&
            info.op == ANVIL_MIR_OP_CALL && !info.symbol &&
            info.call_cc == ANVIL_CC_SYSV) {
            saw_sysv_indirect_call = true;
        }
    }
    CHECK(saw_sysv_indirect_call,
          "MIR indirect call must retain the effective call-site CC");
    anvil_mir_func_destroy(dispatch_mir);

    anvil_mir_func_t *bad_cc = anvil_mir_func_create("bad_x64_cc");
    if (bad_cc) {
        CHECK(anvil_mir_add_call(bad_cc, ANVIL_MIR_NO_VREG, NULL, 0,
                                 "callee", ANVIL_CC_MVS, false, 0) &&
              anvil_mir_add_instr(bad_cc, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0) &&
              !anvil_x86_64_verify_mir_legal(bad_cc, error, sizeof(error)),
              "x64 legalizer must reject an explicit unsupported call CC");
        anvil_mir_func_destroy(bad_cc);
    }

    char *assembly = NULL;
    size_t assembly_len = 0;
    anvil_error_t codegen_error =
        anvil_module_codegen(mod, &assembly, &assembly_len);
    if (codegen_error != ANVIL_OK)
        fprintf(stderr, "VTable codegen error: %s\n", anvil_ctx_get_error(ctx));
    CHECK(codegen_error == ANVIL_OK && assembly && assembly_len > 0,
          "complete x64 VTable module should lower and emit");
    if (assembly) {
        CHECK(strstr(assembly, "\t.quad plus_one\n") &&
              strstr(assembly, "\t.quad plus_two\n") &&
              strstr(assembly, "SmallInteger.vtable+8") &&
              strstr(assembly, "\tcall *%r11\n"),
              "x64 codegen must preserve VTable relocations and indirect dispatch");
        check_x64_vtable_runtime(assembly);
        free(assembly);
    }

    anvil_type_t *void_type = anvil_type_func(
        ctx, anvil_type_void(ctx), NULL, 0, false);
    anvil_func_t *void_decl = anvil_func_declare(mod, "observe", void_type);
    anvil_func_t *void_caller = anvil_func_create(
        mod, "call_observe", void_type, ANVIL_LINK_INTERNAL);
    if (void_caller && void_decl) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(void_caller));
        anvil_value_t *sentinel = (anvil_value_t *)(uintptr_t)1;
        anvil_instr_t *before_invalid = void_caller->entry->last;
        CHECK(!anvil_build_call_checked(
                  ctx, anvil_func_get_value(plus_one), NULL, 0,
                  "wrong.arity", &sentinel) &&
              sentinel == NULL && void_caller->entry->last == before_invalid,
              "fixed-arity checked calls must reject missing arguments transactionally");

        anvil_value_t *variadic_args[] = {
            anvil_const_i32(ctx, 7), anvil_const_f64(ctx, 3.5)
        };
        sentinel = (anvil_value_t *)(uintptr_t)1;
        CHECK(variadic_decl && anvil_build_call_checked(
                  ctx, anvil_func_get_value(variadic_decl), variadic_args, 2,
                  "trace.call", &sentinel) && sentinel == NULL,
              "variadic checked calls must accept typed fixed and extra arguments");
        before_invalid = void_caller->entry->last;
        sentinel = (anvil_value_t *)(uintptr_t)1;
        CHECK(variadic_decl && !anvil_build_call_checked(
                  ctx, anvil_func_get_value(variadic_decl), NULL, 0,
                  "trace.missing.fixed", &sentinel) &&
              sentinel == NULL && void_caller->entry->last == before_invalid,
              "variadic checked calls must enforce their fixed-argument prefix");

        CHECK(anvil_build_call_checked(ctx, anvil_func_get_value(void_decl),
                                       NULL, 0, "observe.call", &sentinel) &&
              sentinel == NULL && anvil_build_ret_void(ctx),
              "checked void call must succeed while unambiguously returning NULL");
        anvil_mir_func_t *void_mir =
            anvil_x86_64_lower_func_to_mir(void_caller);
        char *void_text = NULL;
        size_t void_len = 0;
        bool void_alloc = void_mir && anvil_x86_64_regalloc_mir(void_mir);
        bool void_emit = void_alloc &&
                         anvil_x86_64_emit_mir(void_mir, &void_text, &void_len);
        if (!void_emit) {
            char mir_error[256] = { 0 };
            anvil_x86_64_verify_mir_legal(void_mir, mir_error,
                                          sizeof(mir_error));
            fprintf(stderr, "void call MIR: alloc=%d emit=%d legal=%s\n",
                    void_alloc, void_emit, mir_error);
        }
        CHECK(void_mir && void_alloc && void_emit &&
              void_text && strstr(void_text, "\tcall observe\n"),
              "checked void call must survive the complete x64 MIR pipeline");
        free(void_text);
        anvil_mir_func_destroy(void_mir);
    }

    anvil_type_t *ignore_type = anvil_type_func(
        ctx, anvil_type_void(ctx), method_params, 2, false);
    anvil_func_t *ignore_caller = anvil_func_create(
        mod, "ignore_nonvoid_result", ignore_type, ANVIL_LINK_INTERNAL);
    if (ignore_caller) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(ignore_caller));
        anvil_value_t *args[] = {
            anvil_func_get_param(ignore_caller, 0),
            anvil_func_get_param(ignore_caller, 1)
        };
        CHECK(anvil_build_call_checked(
                  ctx, anvil_func_get_value(plus_one), args, 2,
                  "ignored", NULL) &&
              ignore_caller->entry->last &&
              ignore_caller->entry->last->op == ANVIL_OP_CALL &&
              ignore_caller->entry->last->result != NULL &&
              anvil_build_ret_void(ctx),
              "omitting result_out must still construct a typed non-void call result");
    }

    CHECK(anvil_module_verify(mod, error, sizeof(error)),
          error[0] ? error : "all checked call forms should verify");

    anvil_module_destroy(other);
    if (foreign_init) {
        CHECK(foreign_init->owner_module == NULL &&
              anvil_value_check_constant_dag(foreign_init, ctx) ==
                  ANVIL_CONST_DAG_INVALID,
              "destroying a module must tombstone its relocatable constant DAG without UAF");
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_constant_fault_injection(void)
{
    bool saw_symbol_success = false;
    for (size_t fail_after = 0; fail_after < 5; fail_after++) {
        anvil_ctx_t *ctx = new_ctx(ANVIL_ARCH_X86_64);
        if (!ctx) continue;
        anvil_module_t *mod = anvil_module_create(ctx, "symbol.fault");
        anvil_type_t *fn_type = anvil_type_func(
            ctx, anvil_type_i32(ctx), NULL, 0, false);
        anvil_func_t *fn = anvil_func_declare(mod, "target", fn_type);
        size_t before = owned_value_count(ctx);
        anvil_ctx_clear_error(ctx);
        anvil_test_fail_alloc_after(ctx, fail_after);
        anvil_value_t *addr = anvil_const_symbol_addr(
            anvil_func_get_value(fn));
        if (addr) saw_symbol_success = true;
        else CHECK(anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM &&
                   owned_value_count(ctx) == before,
                   "symbol relocation OOM must report NOMEM and roll back ownership");
        anvil_test_disable_alloc_fail(ctx);
        anvil_ctx_destroy(ctx);
    }

    bool saw_struct_success = false;
    bool saw_gep_success = false;
    for (size_t fail_after = 0; fail_after < 12; fail_after++) {
        anvil_ctx_t *ctx = new_ctx(ANVIL_ARCH_X86_64);
        if (!ctx) continue;
        anvil_module_t *mod = anvil_module_create(ctx, "faults");
        anvil_type_t *fn_type = anvil_type_func(
            ctx, anvil_type_i32(ctx), NULL, 0, false);
        anvil_func_t *fn = anvil_func_declare(mod, "target", fn_type);
        anvil_value_t *addr = anvil_const_symbol_addr(anvil_func_get_value(fn));
        anvil_type_t *fields[] = { addr->type, addr->type };
        anvil_type_t *struct_type = anvil_type_literal_struct(
            ctx, fields, 2, false);
        anvil_value_t *values[] = { addr, addr };

        size_t before = owned_value_count(ctx);
        anvil_ctx_clear_error(ctx);
        anvil_test_fail_alloc_after(ctx, fail_after);
        anvil_value_t *aggregate = anvil_const_struct(
            ctx, struct_type, values, 2);
        if (aggregate) saw_struct_success = true;
        else CHECK(anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM &&
                   owned_value_count(ctx) == before,
                   "struct constant OOM must report NOMEM and roll back ownership");
        anvil_test_disable_alloc_fail(ctx);

        anvil_value_t *global = anvil_module_add_global(
            mod, "table", struct_type, ANVIL_LINK_INTERNAL);
        anvil_value_t *global_addr = anvil_const_symbol_addr(global);
        anvil_value_t *indices[] = {
            anvil_const_i32(ctx, 0), anvil_const_i32(ctx, 1)
        };
        before = owned_value_count(ctx);
        anvil_ctx_clear_error(ctx);
        anvil_test_fail_alloc_after(ctx, fail_after);
        anvil_value_t *gep = anvil_const_gep(
            global_addr, struct_type, indices, 2);
        if (gep) saw_gep_success = true;
        else CHECK(anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM &&
                   owned_value_count(ctx) == before,
                   "constant GEP OOM must report NOMEM and roll back ownership");
        anvil_test_disable_alloc_fail(ctx);
        anvil_ctx_destroy(ctx);
    }
    CHECK(saw_symbol_success && saw_struct_success && saw_gep_success,
          "fault sweep must reach successful constructor paths too");
}

static void test_target_layout_relocations(void)
{
    struct target_case {
        anvil_arch_t arch;
        size_t pointer_size;
        const char *cross_target;
        bool hlasm;
    } cases[] = {
        { ANVIL_ARCH_X86, 4, NULL, false },
        { ANVIL_ARCH_X86_64, 8, NULL, false },
        { ANVIL_ARCH_ARM64, 8, "aarch64-linux-gnu", false },
        { ANVIL_ARCH_PPC32, 4, "powerpc-linux-gnu", false },
        { ANVIL_ARCH_PPC64, 8, "powerpc64-linux-gnu", false },
        { ANVIL_ARCH_PPC64LE, 8, "powerpc64le-linux-gnu", false },
        { ANVIL_ARCH_S370, 4, NULL, true },
        { ANVIL_ARCH_S370_XA, 4, NULL, true },
        { ANVIL_ARCH_S390, 4, NULL, true },
        { ANVIL_ARCH_ZARCH, 8, NULL, true },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = new_ctx(cases[c].arch);
        if (!ctx) continue;
        anvil_module_t *mod = anvil_module_create(ctx, "layout.reloc");
        anvil_type_t *method_type = anvil_type_func(
            ctx, anvil_type_i32(ctx), NULL, 0, false);
        anvil_func_t *method = anvil_func_declare(
            mod, "method_target", method_type);
        anvil_value_t *address = method
            ? anvil_const_symbol_addr(anvil_func_get_value(method)) : NULL;
        anvil_type_t *fields[] = {
            anvil_type_i8(ctx), address ? address->type : NULL,
            anvil_type_i16(ctx)
        };
        anvil_type_t *record = address
            ? anvil_type_literal_struct(ctx, fields, 3, false) : NULL;
        anvil_value_t *values[] = {
            anvil_const_i8(ctx, 0x12), address,
            anvil_const_i16(ctx, 0x3456)
        };
        anvil_value_t *initializer = record
            ? anvil_const_struct(ctx, record, values, 3) : NULL;
        anvil_value_t *global = record
            ? anvil_module_add_global(mod, "method_record", record,
                                      ANVIL_LINK_EXTERNAL) : NULL;
        CHECK(global && initializer &&
              anvil_global_set_initializer(global, initializer),
              "padded ptr<func> record should construct for every target");

        anvil_value_t *method_entries[] = { address, address };
        anvil_value_t *vtable_init = address
            ? anvil_const_array(ctx, address->type, method_entries, 2) : NULL;
        anvil_value_t *vtable = vtable_init
            ? anvil_module_add_global(mod, "method_vtable", vtable_init->type,
                                      ANVIL_LINK_INTERNAL) : NULL;
        anvil_value_t *vtable_addr = vtable
            ? anvil_const_symbol_addr(vtable) : NULL;
        anvil_value_t *slot_indices[] = {
            anvil_const_i32(ctx, 0), anvil_const_i32(ctx, 1)
        };
        anvil_value_t *slot_addr = vtable_addr
            ? anvil_const_gep(vtable_addr, vtable_init->type,
                              slot_indices, 2) : NULL;
        anvil_value_t *selected_slot = slot_addr
            ? anvil_module_add_global(mod, "selected_slot", slot_addr->type,
                                      ANVIL_LINK_EXTERNAL) : NULL;
        CHECK(vtable && selected_slot &&
              anvil_global_set_initializer(vtable, vtable_init) &&
              anvil_global_set_initializer(selected_slot, slot_addr),
              "typed VTable slot relocation should construct for every target");

        anvil_value_t *string_init = anvil_const_string(ctx, "dispatch");
        anvil_value_t *string_ptr = string_init
            ? anvil_module_add_global(mod, "dispatch_name", string_init->type,
                                      ANVIL_LINK_EXTERNAL) : NULL;
        CHECK(string_ptr && anvil_global_set_initializer(string_ptr,
                                                         string_init),
              "global string pointer should construct for every target");

        char *text = NULL;
        size_t length = 0;
        CHECK(global && anvil_module_codegen(mod, &text, &length) == ANVIL_OK &&
              text && length > 0,
              "padded ptr<func> record should emit for every target");
        if (text) {
            size_t ptr_offset = anvil_type_struct_field_offset(record, 1);
            size_t tail = anvil_type_size(record) -
                          (anvil_type_struct_field_offset(record, 2) + 2);
            char padding[64], trailing[64];
            if (cases[c].hlasm) {
                snprintf(padding, sizeof(padding), "DC    %zuX'00'",
                         ptr_offset - 1);
                snprintf(trailing, sizeof(trailing), "DC    %zuX'00'", tail);
                CHECK(strstr(text, cases[c].pointer_size == 8
                                   ? "DC    AD(METHOD_TARGET)"
                                   : "DC    A(METHOD_TARGET)"),
                      "HLASM relocation must use target-width A/AD syntax");
                CHECK(strstr(text, cases[c].pointer_size == 8
                                   ? "DC    AD(METHOD_VTABLE+8)"
                                   : "DC    A(METHOD_VTABLE+4)"),
                      "HLASM typed GEP relocation must retain its addend");
                CHECK(strstr(text, cases[c].pointer_size == 8
                                   ? "DC    AD(AVG00000)"
                                   : "DC    A(AVG00000)"),
                      "HLASM string pointer must relocate to literal storage");
                CHECK(strstr(text, "AVG00000 DC    X'646973706174636800'"),
                      "HLASM string storage must preserve all bytes and NUL");
            } else {
                snprintf(padding, sizeof(padding), "\t.zero %zu\n",
                         ptr_offset - 1);
                snprintf(trailing, sizeof(trailing), "\t.zero %zu\n", tail);
                CHECK(strstr(text, cases[c].pointer_size == 8
                                   ? "\t.quad method_target\n"
                                   : "\t.long method_target\n"),
                      "GNU relocation must use the target pointer width");
                CHECK(strstr(text, cases[c].pointer_size == 8
                                   ? "\t.quad method_vtable+8\n"
                                   : "\t.long method_vtable+4\n"),
                      "GNU typed GEP relocation must retain its addend");
                CHECK(strstr(text, cases[c].pointer_size == 8
                                   ? "\t.quad .Lanvil_global_string_0\n"
                                   : "\t.long .Lanvil_global_string_0\n"),
                      "GNU string pointer must use a target-width relocation");
                CHECK(strstr(text,
                             ".Lanvil_global_string_0:\n\t.asciz \"dispatch\"\n"),
                      "GNU string storage must be materialized exactly once");
            }
            CHECK(strstr(text, padding) && strstr(text, trailing),
                  "struct initializer must emit internal and tail padding");
            if (cases[c].cross_target)
                cross_assemble_data(text, cases[c].cross_target,
                                    "ARM/PPC relocation data should cross-assemble");
            free(text);
        }
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

static void test_x86_coff_function_pointer_decorations(void)
{
    anvil_ctx_t *ctx = new_ctx(ANVIL_ARCH_X86);
    if (!ctx) return;
    CHECK(anvil_ctx_set_abi(ctx, ANVIL_ABI_WIN64) == ANVIL_OK,
          "x86 COFF target selection should succeed");
    anvil_module_t *mod = anvil_module_create(ctx, "x86.coff.vtable");
    anvil_type_t *params[] = { anvil_type_i32(ctx), anvil_type_i64(ctx) };
    anvil_type_t *stdcall_type = anvil_type_func_cc(
        ctx, anvil_type_i32(ctx), params, 2, false, ANVIL_CC_STDCALL);
    anvil_type_t *fastcall_type = anvil_type_func_cc(
        ctx, anvil_type_i32(ctx), params, 2, false, ANVIL_CC_FASTCALL);
    anvil_func_t *stdcall_func = stdcall_type
        ? anvil_func_declare(mod, "stdcall_method", stdcall_type) : NULL;
    anvil_func_t *fastcall_func = fastcall_type
        ? anvil_func_declare(mod, "fastcall_method", fastcall_type) : NULL;
    anvil_value_t *entries[] = {
        stdcall_func
            ? anvil_const_symbol_addr(anvil_func_get_value(stdcall_func)) : NULL,
        fastcall_func
            ? anvil_const_symbol_addr(anvil_func_get_value(fastcall_func)) : NULL
    };
    anvil_value_t *stdcall_table = entries[0]
        ? anvil_module_add_global(mod, "stdcall_slot", entries[0]->type,
                                  ANVIL_LINK_INTERNAL) : NULL;
    anvil_value_t *fastcall_table = entries[1]
        ? anvil_module_add_global(mod, "fastcall_slot", entries[1]->type,
                                  ANVIL_LINK_INTERNAL) : NULL;
    CHECK(stdcall_table && fastcall_table &&
          anvil_global_set_initializer(stdcall_table, entries[0]) &&
          anvil_global_set_initializer(fastcall_table, entries[1]),
          "x86 COFF function pointer globals should construct");

    char *text = NULL;
    size_t length = 0;
    CHECK(anvil_module_codegen(mod, &text, &length) == ANVIL_OK && text,
          "x86 COFF function pointer globals should emit");
    if (text) {
        CHECK(strstr(text, "\t.long _stdcall_method@12\n") &&
              strstr(text, "\t.long @fastcall_method@12\n"),
              "x86 COFF VTable relocations must match ABI-decorated symbols");
        free(text);
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_call_fault_injection(void)
{
    bool saw_success = false;
    for (size_t fail_after = 0; fail_after < 7; fail_after++) {
        anvil_ctx_t *ctx = new_ctx(ANVIL_ARCH_X86_64);
        if (!ctx) continue;
        anvil_module_t *mod = anvil_module_create(ctx, "call.fault");
        anvil_type_t *params[] = { anvil_type_i32(ctx) };
        anvil_type_t *callee_type = anvil_type_func(
            ctx, anvil_type_i32(ctx), params, 1, false);
        anvil_func_t *callee = anvil_func_declare(
            mod, "callee", callee_type);
        anvil_type_t *caller_type = anvil_type_func(
            ctx, anvil_type_i32(ctx), NULL, 0, false);
        anvil_func_t *caller = anvil_func_create(
            mod, "caller", caller_type, ANVIL_LINK_INTERNAL);
        anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
        anvil_value_t *args[] = { anvil_const_i32(ctx, 7) };
        size_t values = owned_value_count(ctx);
        size_t instrs = owned_instr_count(ctx);
        anvil_value_t *result = (anvil_value_t *)(uintptr_t)1;

        anvil_ctx_clear_error(ctx);
        anvil_test_fail_alloc_after(ctx, fail_after);
        bool ok = anvil_build_call_checked(
            ctx, anvil_func_get_value(callee), args, 1, "fault.call", &result);
        if (ok) {
            saw_success = true;
            anvil_test_disable_alloc_fail(ctx);
            CHECK(result != NULL && anvil_build_ret(ctx, result),
                  "successful call fault sweep should produce a usable result");
        } else {
            CHECK(result == NULL &&
                  anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM &&
                  owned_value_count(ctx) == values &&
                  owned_instr_count(ctx) == instrs &&
                  caller->entry->first == NULL && caller->entry->last == NULL,
                  "checked call OOM must clear result and roll back all IR ownership");
        }
        anvil_test_disable_alloc_fail(ctx);
        anvil_ctx_destroy(ctx);
    }
    CHECK(saw_success, "call fault sweep must reach the successful path");
}

int main(void)
{
    test_cc_canonicalization_matrix();
    test_static_vtable_and_checked_calls();
    test_target_layout_relocations();
    test_x86_coff_function_pointer_decorations();
    test_constant_fault_injection();
    test_call_fault_injection();
    if (failures) {
        fprintf(stderr, "%d call/relocation core regression(s) failed\n",
                failures);
        return 1;
    }
    puts("call/relocation core regressions passed");
    return 0;
}
