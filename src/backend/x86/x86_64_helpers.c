/*
 * ANVIL - x86-64 Backend Helper Functions
 *
 * Register name tables and type size/alignment helpers.
 */

#include "x86_64_internal.h"

const char *x64_reg64_names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};

const char *x64_reg32_names[16] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};

const char *x64_reg16_names[16] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};

const char *x64_reg8_names[16] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};

const char *x64_xmm_names[16] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};

int x64_type_size(anvil_type_t *type)
{
    if (!type)
        return 8;

    switch (type->kind) {
    case ANVIL_TYPE_VOID:
        return 0;
    case ANVIL_TYPE_I1:
    case ANVIL_TYPE_I8:
    case ANVIL_TYPE_U8:
        return 1;
    case ANVIL_TYPE_I16:
    case ANVIL_TYPE_U16:
        return 2;
    case ANVIL_TYPE_I32:
    case ANVIL_TYPE_U32:
    case ANVIL_TYPE_F32:
        return 4;
    case ANVIL_TYPE_I64:
    case ANVIL_TYPE_U64:
    case ANVIL_TYPE_F64:
    case ANVIL_TYPE_PTR:
        return 8;
    case ANVIL_TYPE_ARRAY:
        return type->data.array.count * x64_type_size(type->data.array.elem);
    case ANVIL_TYPE_STRUCT: {
        int size = 0;
        for (size_t i = 0; i < type->data.struc.num_fields; i++) {
            int field_size = x64_type_size(type->data.struc.fields[i]);
            int field_align = x64_type_align(type->data.struc.fields[i]);
            size = (size + field_align - 1) & ~(field_align - 1);
            size += field_size;
        }
        int struct_align = x64_type_align(type);
        size = (size + struct_align - 1) & ~(struct_align - 1);
        return size > 0 ? size : 8;
    }
    case ANVIL_TYPE_FUNC:
        return 8;
    default:
        return 8;
    }
}

int x64_type_align(anvil_type_t *type)
{
    if (!type)
        return 8;

    switch (type->kind) {
    case ANVIL_TYPE_VOID:
        return 1;
    case ANVIL_TYPE_I1:
    case ANVIL_TYPE_I8:
    case ANVIL_TYPE_U8:
        return 1;
    case ANVIL_TYPE_I16:
    case ANVIL_TYPE_U16:
        return 2;
    case ANVIL_TYPE_I32:
    case ANVIL_TYPE_U32:
    case ANVIL_TYPE_F32:
        return 4;
    case ANVIL_TYPE_I64:
    case ANVIL_TYPE_U64:
    case ANVIL_TYPE_F64:
    case ANVIL_TYPE_PTR:
        return 8;
    case ANVIL_TYPE_ARRAY:
        return x64_type_align(type->data.array.elem);
    case ANVIL_TYPE_STRUCT: {
        int max_align = 1;
        for (size_t i = 0; i < type->data.struc.num_fields; i++) {
            int field_align = x64_type_align(type->data.struc.fields[i]);
            if (field_align > max_align)
                max_align = field_align;
        }
        return max_align;
    }
    case ANVIL_TYPE_FUNC:
        return 8;
    default:
        return 8;
    }
}

bool x64_type_is_float(anvil_type_t *type)
{
    if (!type)
        return false;
    return type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64;
}
