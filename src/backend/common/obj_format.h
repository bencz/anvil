/* Object-file format policy for GNU-syntax printers: section names, label
 * spelling and directives.  Backends pick a descriptor from their ABI or
 * platform instead of branching on the format in printing code. */
#ifndef ANVIL_BACKEND_OBJ_FORMAT_H
#define ANVIL_BACKEND_OBJ_FORMAT_H

#include "anvil/anvil_internal.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct anvil_obj_format {
    const char *name;
    const char *text_section;      /* executable code */
    const char *rodata_section;    /* read-only data */
    const char *relro_section;     /* read-only data the loader relocates */
    const char *cstring_section;   /* NUL-terminated string literals */
    const char *literal16_section; /* 16-byte constants (SIMD masks) */
    const char *data_section;
    const char *bss_section;
    const char *local_label_prefix;
    bool symbol_type_size;         /* `.type`/`.size` directives (ELF) */
    bool weak_definition;          /* weak = `.globl` + `.weak_definition` (Mach-O) */
    bool plain_bss;                /* zero-fill section takes ordinary data directives */
    const char *module_end;        /* trailer (ELF: non-executable stack note) */
    bool cfi;                      /* DWARF call-frame directives (`.cfi_*`) */
    bool comm_align_log2;          /* `.comm` alignment as a power of two */
    bool got_relocations;          /* sym@GOT / sym@GOTOFF / _GLOBAL_OFFSET_TABLE_ (ELF) */
} anvil_obj_format_t;

extern const anvil_obj_format_t anvil_obj_format_elf;
extern const anvil_obj_format_t anvil_obj_format_macho;
extern const anvil_obj_format_t anvil_obj_format_coff;

/* `.p2align log2(align)`: one spelling every GNU-compatible assembler reads
 * the same way on every format (unlike `.align`). */
void anvil_obj_emit_align(anvil_strbuf_t *out, size_t align_bytes);

/* The binding directives for a defined symbol: `.globl` for external and
 * common, the format's weak form for weak, nothing for internal. */
void anvil_obj_emit_binding(anvil_strbuf_t *out, const anvil_obj_format_t *format, anvil_linkage_t linkage, const char *symbol);

/* The format's module trailer. */
void anvil_obj_emit_module_end(anvil_strbuf_t *out, const anvil_obj_format_t *format);

/* Code, then data, then the trailer, as one module; consumes both buffers. */
anvil_error_t anvil_obj_finish_module(anvil_strbuf_t *code, anvil_strbuf_t *data, const anvil_obj_format_t *format,
                                      char **output, size_t *len);

#endif
