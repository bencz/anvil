#include "backend.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static struct {
    int arch;
    AnvilBackend* backend;
} registered_backends[ANVIL_MAX_BACKENDS];

static int num_backends = 0;

extern AnvilBackend* anvil_create_x86_64_backend(void);
extern AnvilBackend* anvil_create_arm64_backend(void);
extern AnvilBackend* anvil_create_ppc64_backend(void);

void anvil_backends_init(void) {
    anvil_register_backend(ANVIL_ARCH_X86_64, anvil_create_x86_64_backend());
    anvil_register_backend(ANVIL_ARCH_ARM64, anvil_create_arm64_backend());
    anvil_register_backend(ANVIL_ARCH_PPC64, anvil_create_ppc64_backend());
}

void anvil_register_backend(int arch, AnvilBackend* backend) {
    if (num_backends >= ANVIL_MAX_BACKENDS) {
        return;
    }
    registered_backends[num_backends].arch = arch;
    registered_backends[num_backends].backend = backend;
    num_backends++;
}

AnvilBackend* anvil_get_backend(int arch) {
    for (int i = 0; i < num_backends; i++) {
        if (registered_backends[i].arch == arch) {
            return registered_backends[i].backend;
        }
    }
    return NULL;
}

const char* anvil_list_backends(void) {
    static char buffer[1024];
    buffer[0] = '\0';
    for (int i = 0; i < num_backends; i++) {
        if (i > 0) strcat(buffer, ", ");
        strcat(buffer, registered_backends[i].backend->name);
    }
    return buffer;
}

void anvil_asm_init(AnvilAsmBuffer* buf) {
    anvil_str_builder_init(&buf->sb);
}

void anvil_asm_free(AnvilAsmBuffer* buf) {
    anvil_str_builder_free(&buf->sb);
}

void anvil_asm_append(AnvilAsmBuffer* buf, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    char temp[1024];
    vsnprintf(temp, sizeof(temp), fmt, args);
    anvil_str_builder_append(&buf->sb, temp);
    
    va_end(args);
}

void anvil_asm_newline(AnvilAsmBuffer* buf) {
    anvil_str_builder_append_char(&buf->sb, '\n');
}

char* anvil_asm_take(AnvilAsmBuffer* buf) {
    return anvil_str_builder_take(&buf->sb);
}
