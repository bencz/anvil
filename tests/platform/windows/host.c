#include "../host.h"
#include <stdbool.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cross-assemble target text on a Windows host; no target execution implied. */
static int cross_assemble(const char *assembly, const char *target)
{
    const char *compiler = getenv("ANVIL_TEST_CLANG");
    char directory[MAX_PATH];
    char source[MAX_PATH];
    char object[MAX_PATH + 8];
    char command[2048];

    if (!compiler || !assembly || !target)
        return 1;

    DWORD length = GetTempPathA(MAX_PATH, directory);

    if (!length || length >= MAX_PATH || !GetTempFileNameA(directory, "anv", 0, source))
        return 1;

    FILE *file = fopen(source, "wb");
    bool ok = file != NULL;
    bool numeric_registers = strncmp(target, "powerpc", 7) == 0;

    for (size_t i = 0; ok && assembly[i]; i++)
    {
        if (numeric_registers && (assembly[i] == 'r' || assembly[i] == 'f') && assembly[i + 1] >= '0' && assembly[i + 1] <= '9')
            continue;

        ok = fputc((unsigned char)assembly[i], file) != EOF;
    }

    if (file && fclose(file) != 0)
        ok = false;

    snprintf(object, sizeof(object), "%s.obj", source);
    int written =
        snprintf(command, sizeof(command), "\"\"%s\" --target=%s -x assembler -c \"%s\" -o \"%s\"\"", compiler, target, source, object);

    if (ok)
        ok = written > 0 && (size_t)written < sizeof(command) && system(command) == 0;

    remove(source);
    remove(object);

    return ok ? 0 : 1;
}

static int assemble_i386(const char *assembly, const char *message)
{
    (void)message;
    return cross_assemble(assembly, "i386-pc-linux-gnu");
}

static int run_i386(const char *assembly, const char *driver, const char *message)
{
    (void)assembly;
    (void)driver;
    fprintf(stderr, "[skip] %s (requires an i386 SysV execution host)\n", message);
    return -1;
}

const anvil_test_host_ops_t anvil_test_host = {
    .cross_assemble = cross_assemble,
    .assemble_i386 = assemble_i386,
    .run_i386 = run_i386,
};
