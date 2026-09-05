#include "../host.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int cross_assemble(const char *assembly, const char *target)
{
    if (!assembly)
        return 1;

    if (access("/usr/bin/clang", X_OK) != 0)
    {
        fprintf(stderr, "[skip] cross-assembly (%s; Clang unavailable)\n", target);
        return -1;
    }
    char source[] = "/tmp/anvil-fcmp-XXXXXX.s";
    int fd = mkstemps(source, 2);
    if (fd < 0)
    {
        return 1;
    }
    FILE *out = fdopen(fd, "wb");
    bool ppc_numeric_regs = strncmp(target, "powerpc", 7) == 0;
    bool write_ok = out != NULL;
    for (size_t i = 0; write_ok && assembly[i]; i++)
    {
        if (ppc_numeric_regs && (assembly[i] == 'r' || assembly[i] == 'f') && assembly[i + 1] >= '0' && assembly[i + 1] <= '9')
            continue;
        write_ok = fputc((unsigned char)assembly[i], out) != EOF;
    }
    bool close_ok = out ? fclose(out) == 0 : false;
    if (!out || !write_ok || !close_ok)
    {
        if (!out)
            close(fd);
        unlink(source);
        return 1;
    }
    char object[128], command[512];
    snprintf(object, sizeof(object), "%s.o", source);
    snprintf(command, sizeof(command), "/usr/bin/clang --target=%s -c %s -o %s >/dev/null 2>&1", target, source, object);
    int status = system(command);
    unlink(source);
    unlink(object);
    return status == 0 ? 0 : 1;
}

static int g_have_as = -1;

static int have_as(void)
{
    if (g_have_as < 0)
    {
        g_have_as = (system("as --version >/dev/null 2>&1") == 0) ? 1 : 0;
    }
    return g_have_as;
}

static int assemble_i386(const char *asm_text, const char *msg)
{

    if (!asm_text)
    {
        return 1;
    }
    if (!have_as())
    {
        fprintf(stderr, "[skip] %s (no host assembler)\n", msg);
        return -1;
    }

    char src[] = "/tmp/anvil_x86_asm_XXXXXX";
    int fd = mkstemp(src);
    if (fd < 0)
    {
        return 1;
    }
    FILE *f = fdopen(fd, "w");
    if (!f)
    {
        close(fd);
        return 1;
    }
    fputs(asm_text, f);
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "as --32 %s -o %s.o 2>%s.err", src, src, src);
    int rc = system(cmd);
    if (rc != 0)
    {
        fprintf(stderr, "==== as --32 failed for: %s ====\n%s\n", msg, asm_text);
        char errcat[600];
        snprintf(errcat, sizeof(errcat), "cat %s.err 1>&2", src);
        (void)system(errcat);
        char errf[600];
        snprintf(errf, sizeof(errf), "%s.err", src);
        remove(errf);
    }
    else
    {
        char errf[600];
        snprintf(errf, sizeof(errf), "%s.err", src);
        remove(errf);
    }

    char obj[600];
    snprintf(obj, sizeof(obj), "%s.o", src);
    remove(src);
    remove(obj);
    return rc == 0 ? 0 : 1;
}
static int g_have_m32 = -1;

static int have_m32(void)
{
    if (g_have_m32 < 0)
    {
        g_have_m32 = (system("printf 'int main(void){return 0;}' "
                             "| gcc -m32 -x c - -o /tmp/anvil_m32_probe >/dev/null 2>&1") == 0)
                         ? 1
                         : 0;
        remove("/tmp/anvil_m32_probe");
    }
    return g_have_m32;
}

static int run_i386(const char *asm_text, const char *driver, const char *msg)
{

    if (!asm_text || !have_as() || !have_m32())
    {
        if (!have_as() || !have_m32())
        {
            fprintf(stderr, "[skip] %s (no 32-bit toolchain)\n", msg);
        }
        return -1;
    }

    char as_src[] = "/tmp/anvil_x86_run_XXXXXX";
    int fd = mkstemp(as_src);
    if (fd < 0)
    {
        return 1;
    }
    FILE *f = fdopen(fd, "w");
    if (!f)
    {
        close(fd);
        return 1;
    }
    fputs(asm_text, f);
    fclose(f);

    char drv_src[] = "/tmp/anvil_x86_drv_XXXXXX";
    int dfd = mkstemp(drv_src);
    if (dfd < 0)
    {
        remove(as_src);
        return 1;
    }
    FILE *df = fdopen(dfd, "w");
    if (!df)
    {
        close(dfd);
        remove(as_src);
        return 1;
    }
    fputs(driver, df);
    fclose(df);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "as --32 %s -o %s.o 2>/dev/null && "
             "gcc -m32 -x c %s -x none %s.o -o %s.bin -lm 2>/dev/null && "
             "%s.bin",
             as_src, as_src, drv_src, as_src, as_src, as_src);
    int rc = system(cmd);

    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.o", as_src);
    remove(tmp);
    snprintf(tmp, sizeof(tmp), "%s.bin", as_src);
    remove(tmp);
    remove(as_src);
    remove(drv_src);

    return rc != -1 && WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}

const anvil_test_host_ops_t anvil_test_host = {
    .cross_assemble = cross_assemble,
    .assemble_i386 = assemble_i386,
    .run_i386 = run_i386,
};
