/*
 * ANVIL - x86 (32-bit) Backend Helper Functions
 *
 * Register name tables and type size/alignment helpers.
 */

#include "x86_internal.h"

const char *x86_reg32_names[8] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"
};

const char *x86_reg16_names[8] = {
    "ax", "cx", "dx", "bx", "sp", "bp", "si", "di"
};

const char *x86_reg8_names[8] = {
    "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"
};

const char *x86_xmm_names[8] = {
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"
};

bool x86_reg_has_byte(int phys_reg)
{
    return phys_reg == X86_EAX || phys_reg == X86_ECX ||
           phys_reg == X86_EDX || phys_reg == X86_EBX;
}

const char *x86_byte_reg_name(int phys_reg)
{
    if (!x86_reg_has_byte(phys_reg)) return NULL;
    return x86_reg8_names[phys_reg];
}

int x86_type_size(anvil_type_t *type)
{
    if (!type) return 4;

    switch (type->kind) {
        case ANVIL_TYPE_VOID:
            return 0;
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return 1;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return 2;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_F32:
        case ANVIL_TYPE_PTR:
            return 4;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F64:
            return 8;
        case ANVIL_TYPE_ARRAY:
            return type->data.array.count * x86_type_size(type->data.array.elem);
        case ANVIL_TYPE_STRUCT:
            {
                int size = 0;
                for (size_t i = 0; i < type->data.struc.num_fields; i++) {
                    int field_size = x86_type_size(type->data.struc.fields[i]);
                    int field_align = x86_type_align(type->data.struc.fields[i]);
                    size = (size + field_align - 1) & ~(field_align - 1);
                    size += field_size;
                }
                int struct_align = x86_type_align(type);
                size = (size + struct_align - 1) & ~(struct_align - 1);
                return size > 0 ? size : 4;
            }
        case ANVIL_TYPE_FUNC:
            return 4;
        default:
            return 4;
    }
}

int x86_type_align(anvil_type_t *type)
{
    if (!type) return 4;

    switch (type->kind) {
        case ANVIL_TYPE_VOID:
            return 1;
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return 1;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return 2;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_F32:
        case ANVIL_TYPE_PTR:
            return 4;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F64:
            return 4;
        case ANVIL_TYPE_ARRAY:
            return x86_type_align(type->data.array.elem);
        case ANVIL_TYPE_STRUCT:
            {
                int max_align = 1;
                for (size_t i = 0; i < type->data.struc.num_fields; i++) {
                    int field_align = x86_type_align(type->data.struc.fields[i]);
                    if (field_align > max_align) max_align = field_align;
                }
                return max_align;
            }
        case ANVIL_TYPE_FUNC:
            return 4;
        default:
            return 4;
    }
}

bool x86_type_is_float(anvil_type_t *type)
{
    if (!type) return false;
    return type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64;
}
