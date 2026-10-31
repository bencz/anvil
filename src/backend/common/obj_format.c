#include "obj_format.h"

#include <stdlib.h>

const anvil_obj_format_t anvil_obj_format_elf = {
    .name = "ELF",
    .text_section = "\t.text\n",
    .rodata_section = "\t.section .rodata\n",
    .relro_section = "\t.section .data.rel.ro,\"aw\"\n",
    .cstring_section = "\t.section .rodata.str1.1,\"aMS\",@progbits,1\n",
    .literal16_section = "\t.section .rodata.cst16,\"aM\",@progbits,16\n",
    .data_section = "\t.data\n",
    .bss_section = "\t.bss\n",
    .local_label_prefix = ".L",
    .symbol_type_size = true,
    .plain_bss = true,
    .module_end = "\t.section .note.GNU-stack,\"\",@progbits\n",
    .cfi = true,
    .got_relocations = true,
};

const anvil_obj_format_t anvil_obj_format_macho = {
    .name = "Mach-O",
    .text_section = "\t.section __TEXT,__text,regular,pure_instructions\n",
    .rodata_section = "\t.section __TEXT,__const\n",
    .relro_section = "\t.section __DATA,__const\n",
    .cstring_section = "\t.section __TEXT,__cstring,cstring_literals\n",
    .literal16_section = "\t.section __TEXT,__literal16,16byte_literals\n",
    .data_section = "\t.section __DATA,__data\n",
    .bss_section = "\t.section __DATA,__bss\n",
    .local_label_prefix = "L",
    .symbol_type_size = false,
    .weak_definition = true,
    .comm_align_log2 = true,
    .module_end = "\t.subsections_via_symbols\n",
    .cfi = true,
};

const anvil_obj_format_t anvil_obj_format_coff = {
    .name = "COFF",
    .text_section = "\t.text\n",
    .rodata_section = "\t.section .rdata,\"dr\"\n",
    .relro_section = "\t.section .rdata,\"dr\"\n",
    .cstring_section = "\t.section .rdata,\"dr\"\n",
    .literal16_section = "\t.section .rdata,\"dr\"\n",
    .data_section = "\t.data\n",
    .bss_section = "\t.bss\n",
    .local_label_prefix = ".L",
    .symbol_type_size = false,
    .plain_bss = true,
    .comm_align_log2 = true,
};

void anvil_obj_emit_binding(anvil_strbuf_t *out, const anvil_obj_format_t *format, anvil_linkage_t linkage, const char *symbol)
{
    switch (linkage) {
    case ANVIL_LINK_INTERNAL:
        break;
    case ANVIL_LINK_WEAK:
        if (format->weak_definition)
            anvil_strbuf_appendf(out, "\t.globl %s\n\t.weak_definition %s\n", symbol, symbol);
        else
            anvil_strbuf_appendf(out, "\t.weak %s\n", symbol);
        break;
    default:
        anvil_strbuf_appendf(out, "\t.globl %s\n", symbol);
        break;
    }
}

void anvil_obj_emit_align(anvil_strbuf_t *out, size_t align_bytes)
{
    unsigned log2 = 0;
    while (((size_t)1 << log2) < align_bytes && log2 < 63)
        log2++;
    if (log2 > 0)
        anvil_strbuf_appendf(out, "\t.p2align %u\n", log2);
}

void anvil_obj_emit_module_end(anvil_strbuf_t *out, const anvil_obj_format_t *format)
{
    if (format && format->module_end)
        anvil_strbuf_append(out, format->module_end);
}

anvil_error_t anvil_obj_finish_module(anvil_strbuf_t *code, anvil_strbuf_t *data, const anvil_obj_format_t *format,
                                      char **output, size_t *len)
{
    size_t data_len = 0;
    char *data_str = anvil_strbuf_detach(data, &data_len);
    if (!data_str || code->failed) {
        free(data_str);
        anvil_strbuf_destroy(code);
        return ANVIL_ERR_NOMEM;
    }
    anvil_strbuf_append(code, data_str);
    free(data_str);
    anvil_obj_emit_module_end(code, format);
    if (code->failed) {
        anvil_strbuf_destroy(code);
        return ANVIL_ERR_NOMEM;
    }
    *output = anvil_strbuf_detach(code, len);
    return *output ? ANVIL_OK : ANVIL_ERR_NOMEM;
}
