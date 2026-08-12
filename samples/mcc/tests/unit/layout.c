#include "mcc.h"

#include <limits.h>
#include <stdint.h>

/* Internal lowering entry point: this unit specifically verifies the opaque
 * identified-record lifecycle that the public wrapper delegates to. */
anvil_type_t *codegen_type(mcc_codegen_t *cg, mcc_type_t *type);

static size_t align_up(size_t value, size_t align)
{
    return (value + align - 1) & ~(align - 1);
}

static int check_arch(mcc_arch_t mcc_arch, anvil_arch_t anvil_arch)
{
    int rc = 0;
    mcc_context_t *ctx = mcc_context_create();
    anvil_ctx_t *anvil = NULL;
#define CHECK(condition, code) do { if (!(condition)) { rc = (code); goto out; } } while (0)

    CHECK(ctx != NULL, 1);
    ctx->options.arch = mcc_arch;
    mcc_type_context_t *types = mcc_type_context_create(ctx);
    anvil = anvil_ctx_create_for_target(anvil_arch);
    CHECK(types && anvil, 2);
    const anvil_data_layout_t *layout = anvil_ctx_get_data_layout(anvil);
    CHECK(layout != NULL, 3);
    CHECK(types->arch == anvil_arch &&
          types->layout.pointer.size == layout->pointer.size &&
          types->layout.pointer.abi_align == layout->pointer.abi_align &&
          types->layout.i64.size == layout->i64.size &&
          types->layout.i64.abi_align == layout->i64.abi_align &&
          types->layout.f64.size == layout->f64.size &&
          types->layout.f64.abi_align == layout->f64.abi_align &&
          types->layout.aggregate_abi_align == layout->aggregate_abi_align,
          4);

    /* Probe { char, long long, double }: catches i386's four-byte i64/f64
     * ABI alignment versus PPC32/x86-64's natural eight-byte alignment. */
    mcc_type_t *probe_types[] = {
        mcc_type_char(types), mcc_type_llong(types), mcc_type_double(types)
    };
    anvil_type_t *anvil_fields[] = {
        anvil_type_i8(anvil), anvil_type_i64(anvil), anvil_type_f64(anvil)
    };
    anvil_type_t *anvil_probe = anvil_type_struct(
        anvil, NULL, anvil_fields, 3);
    CHECK(anvil_probe != NULL, 5);

    mcc_struct_field_t fields[3] = {0};
    for (size_t i = 0; i < 3; i++) {
        fields[i].type = probe_types[i];
        fields[i].next = i + 1 < 3 ? &fields[i + 1] : NULL;
        CHECK(probe_types[i]->size == anvil_type_size(anvil_fields[i]) &&
              probe_types[i]->align == anvil_type_align(anvil_fields[i]), 6);
    }
    mcc_type_t probe = {0};
    probe.kind = TYPE_STRUCT;
    CHECK(mcc_type_complete_struct(types, &probe, fields, 3), 7);
    CHECK(probe.size == anvil_type_size(anvil_probe) &&
          probe.align == anvil_type_align(anvil_probe), 8);
    for (size_t i = 0; i < 3; i++) {
        CHECK(fields[i].offset ==
              anvil_type_struct_field_offset(anvil_probe, i), 9);
    }

    /* Arrays and nested offsets must compose without a host-size heuristic. */
    mcc_type_t *shorts = mcc_type_array(types, mcc_type_short(types), 3);
    anvil_type_t *anvil_shorts = anvil_type_array(
        anvil, anvil_type_i16(anvil), 3);
    CHECK(shorts && anvil_shorts &&
          shorts->size == anvil_type_size(anvil_shorts) &&
          shorts->align == anvil_type_align(anvil_shorts), 10);
    mcc_type_t *ptr = mcc_type_pointer(types, mcc_type_int(types));
    anvil_type_t *anvil_ptr = anvil_type_ptr(anvil, anvil_type_i32(anvil));
    CHECK(ptr && ptr->size == anvil_type_size(anvil_ptr) &&
          ptr->align == anvil_type_align(anvil_ptr), 11);

    mcc_struct_field_t nested_fields[3] = {0};
    nested_fields[0].type = mcc_type_char(types);
    nested_fields[1].type = shorts;
    nested_fields[2].type = ptr;
    nested_fields[0].next = &nested_fields[1];
    nested_fields[1].next = &nested_fields[2];
    anvil_type_t *anvil_nested_fields[] = {
        anvil_type_i8(anvil), anvil_shorts, anvil_ptr
    };
    anvil_type_t *anvil_nested = anvil_type_struct(
        anvil, NULL, anvil_nested_fields, 3);
    mcc_type_t nested = {0};
    nested.kind = TYPE_STRUCT;
    CHECK(anvil_nested &&
          mcc_type_complete_struct(types, &nested, nested_fields, 3), 12);
    CHECK(nested.size == anvil_type_size(anvil_nested) &&
          nested.align == anvil_type_align(anvil_nested), 13);
    for (size_t i = 0; i < 3; i++) {
        CHECK(nested_fields[i].offset ==
              anvil_type_struct_field_offset(anvil_nested, i), 14);
    }

    /* Union { char bytes[9]; int word; }: max member size rounded to the
     * maximum member/aggregate ABI alignment, with all offsets exactly zero. */
    mcc_type_t *bytes = mcc_type_array(types, mcc_type_char(types), 9);
    mcc_struct_field_t union_fields[2] = {0};
    union_fields[0].type = bytes;
    union_fields[0].next = &union_fields[1];
    union_fields[1].type = mcc_type_int(types);
    mcc_type_t uni = {0};
    uni.kind = TYPE_UNION;
    CHECK(bytes && mcc_type_complete_union(types, &uni, union_fields, 2), 15);
    size_t union_align = layout->aggregate_abi_align;
    if (layout->i8.abi_align > union_align) union_align = layout->i8.abi_align;
    if (layout->i32.abi_align > union_align) union_align = layout->i32.abi_align;
    CHECK(uni.align == union_align && uni.size == align_up(9, union_align) &&
          union_fields[0].offset == 0 && union_fields[1].offset == 0, 16);

    mcc_type_t *enumeration = mcc_type_enum(types, "E");
    CHECK(enumeration && enumeration->size == layout->i32.size &&
          enumeration->align == layout->i32.abi_align, 17);

    /* Lowering an incomplete recursive tag must cache only its identity, then
     * define the exact body when the same tag is completed later. */
    mcc_codegen_t cg = {0};
    cg.mcc_ctx = ctx;
    cg.types = types;
    cg.anvil_ctx = anvil;
    mcc_type_t *node = mcc_type_struct(types, "Node");
    CHECK(node != NULL, 18);
    anvil_type_t *opaque_node = codegen_type(&cg, node);
    CHECK(opaque_node && !node->anvil_body_lowered &&
          !node->anvil_lower_failed, 19);
    mcc_type_t *node_ptr = mcc_type_pointer(types, node);
    mcc_struct_field_t node_fields[2] = {0};
    node_fields[0].name = "next";
    node_fields[0].type = node_ptr;
    node_fields[0].next = &node_fields[1];
    node_fields[1].name = "value";
    node_fields[1].type = mcc_type_int(types);
    CHECK(node_ptr && mcc_type_complete_struct(types, node, node_fields, 2), 20);
    anvil_type_t *complete_node = codegen_type(&cg, node);
    CHECK(complete_node == opaque_node && node->anvil_body_lowered &&
          !node->anvil_lowering && !node->anvil_lower_failed &&
          anvil_type_size(complete_node) == node->size &&
          anvil_type_align(complete_node) == node->align, 21);

    /* Overflow and unsupported-layout paths are fail-closed. */
    CHECK(mcc_type_array(types, mcc_type_llong(types), SIZE_MAX) == NULL &&
          mcc_has_errors(ctx), 22);
    mcc_type_t huge = {0};
    huge.kind = TYPE_ARRAY;
    huge.size = SIZE_MAX;
    huge.align = layout->i64.abi_align;
    mcc_struct_field_t huge_field = {0};
    huge_field.type = &huge;
    mcc_type_t overflowing = {0};
    overflowing.kind = TYPE_STRUCT;
    CHECK(!mcc_type_complete_struct(types, &overflowing, &huge_field, 1) &&
          !overflowing.data.record.is_complete, 23);
    mcc_struct_field_t bits = {0};
    bits.type = mcc_type_uint(types);
    bits.bitfield_width = 1;
    mcc_type_t bit_record = {0};
    bit_record.kind = TYPE_STRUCT;
    CHECK(!mcc_type_complete_struct(types, &bit_record, &bits, 1), 24);
    CHECK(mcc_type_array(types, mcc_type_void(types), 4) == NULL, 25);
    mcc_struct_field_t void_field = {0};
    void_field.type = mcc_type_void(types);
    mcc_type_t void_record = {0};
    void_record.kind = TYPE_STRUCT;
    CHECK(!mcc_type_complete_struct(types, &void_record, &void_field, 1), 26);

out:
    anvil_ctx_destroy(anvil);
    mcc_context_destroy(ctx);
    return rc;
#undef CHECK
}

int main(void)
{
    int rc = check_arch(MCC_ARCH_X86, ANVIL_ARCH_X86);
    if (rc) return 100 + rc;
    rc = check_arch(MCC_ARCH_PPC32, ANVIL_ARCH_PPC32);
    if (rc) return 200 + rc;
    rc = check_arch(MCC_ARCH_X86_64, ANVIL_ARCH_X86_64);
    if (rc) return 300 + rc;

    mcc_context_t *unset = mcc_context_create();
    if (!unset) return 401;
    if (mcc_type_context_create(unset) != NULL || !mcc_has_errors(unset)) {
        mcc_context_destroy(unset);
        return 402;
    }
    mcc_context_destroy(unset);
    puts("MCC target DataLayout tests passed");
    return 0;
}
