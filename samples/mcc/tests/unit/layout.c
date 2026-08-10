#include "mcc.h"

static int check_arch(mcc_arch_t mcc_arch, anvil_arch_t anvil_arch)
{
    mcc_context_t *ctx = mcc_context_create();
    if (!ctx) return 1;
    ctx->options.arch = mcc_arch;
    mcc_type_context_t *types = mcc_type_context_create(ctx);
    anvil_ctx_t *anvil = anvil_ctx_create_for_target(anvil_arch);
    if (!types || !anvil) return 2;

    mcc_type_t *probe_types[] = {
        mcc_type_char(types), mcc_type_llong(types), mcc_type_double(types)
    };
    anvil_type_t *anvil_fields[] = {
        anvil_type_i8(anvil), anvil_type_i64(anvil), anvil_type_f64(anvil)
    };
    anvil_type_t *anvil_probe = anvil_type_struct(
        anvil, NULL, anvil_fields, 3);
    if (!anvil_probe) return 3;

    mcc_struct_field_t fields[3] = {0};
    for (size_t i = 0; i < 3; i++) {
        fields[i].type = probe_types[i];
        fields[i].next = i + 1 < 3 ? &fields[i + 1] : NULL;
        if (probe_types[i]->size != anvil_type_size(anvil_fields[i]) ||
            probe_types[i]->align != anvil_type_align(anvil_fields[i])) {
            return 4;
        }
    }
    mcc_type_t probe = {0};
    probe.kind = TYPE_STRUCT;
    if (!mcc_type_complete_struct(types, &probe, fields, 3)) return 5;
    if (probe.size != anvil_type_size(anvil_probe) ||
        probe.align != anvil_type_align(anvil_probe)) return 6;
    for (size_t i = 0; i < 3; i++) {
        if (fields[i].offset !=
            anvil_type_struct_field_offset(anvil_probe, i)) return 7;
    }

    mcc_type_t *ptr = mcc_type_pointer(types, mcc_type_int(types));
    anvil_type_t *anvil_ptr = anvil_type_ptr(anvil, anvil_type_i32(anvil));
    if (!ptr || ptr->size != anvil_type_size(anvil_ptr) ||
        ptr->align != anvil_type_align(anvil_ptr)) return 8;

    if (mcc_type_array(types, mcc_type_llong(types), SIZE_MAX) != NULL ||
        !mcc_has_errors(ctx)) return 9;

    anvil_ctx_destroy(anvil);
    mcc_context_destroy(ctx);
    return 0;
}

int main(void)
{
    if (check_arch(MCC_ARCH_X86, ANVIL_ARCH_X86) != 0) return 1;
    if (check_arch(MCC_ARCH_PPC32, ANVIL_ARCH_PPC32) != 0) return 2;
    if (check_arch(MCC_ARCH_X86_64, ANVIL_ARCH_X86_64) != 0) return 3;
    puts("MCC target layout tests passed");
    return 0;
}
