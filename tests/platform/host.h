#ifndef ANVIL_TEST_PLATFORM_HOST_H
#define ANVIL_TEST_PLATFORM_HOST_H

/* Assembly checks return 0 on success, 1 on failure, -1 when unavailable.
 * Runtime checks return the program exit status, or -1 when unavailable. */
typedef struct
{
    int (*cross_assemble)(const char *assembly, const char *target);
    int (*assemble_i386)(const char *assembly, const char *message);
    int (*run_i386)(const char *assembly, const char *driver, const char *message);
} anvil_test_host_ops_t;

extern const anvil_test_host_ops_t anvil_test_host;

#endif
