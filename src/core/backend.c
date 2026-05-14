/*
 * ANVIL - Backend registration and management
 */

#include "anvil/anvil_internal.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Maximum number of registered backends */
#define MAX_BACKENDS 32

/* Registered backends. Accessed concurrently when multiple threads create
 * contexts simultaneously; protected with pthread_once during the built-in
 * registration and an explicit mutex for any later anvil_register_backend()
 * calls. */
static const anvil_backend_ops_t *registered_backends[MAX_BACKENDS];
static size_t num_backends = 0;
static pthread_once_t backends_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t backends_mutex = PTHREAD_MUTEX_INITIALIZER;

static void register_builtin_backends(void)
{
    anvil_register_backend(&anvil_backend_x86);
    anvil_register_backend(&anvil_backend_x86_64);
    anvil_register_backend(&anvil_backend_s370);
    anvil_register_backend(&anvil_backend_s370_xa);
    anvil_register_backend(&anvil_backend_s390);
    anvil_register_backend(&anvil_backend_zarch);
    anvil_register_backend(&anvil_backend_ppc32);
    anvil_register_backend(&anvil_backend_ppc64);
    anvil_register_backend(&anvil_backend_ppc64le);
    anvil_register_backend(&anvil_backend_arm64);
}

void anvil_init_backends(void)
{
    pthread_once(&backends_once, register_builtin_backends);
}

anvil_error_t anvil_register_backend(const anvil_backend_ops_t *ops)
{
    if (!ops) return ANVIL_ERR_INVALID_ARG;

    pthread_mutex_lock(&backends_mutex);
    if (num_backends >= MAX_BACKENDS) {
        pthread_mutex_unlock(&backends_mutex);
        return ANVIL_ERR_NOMEM;
    }
    registered_backends[num_backends++] = ops;
    pthread_mutex_unlock(&backends_mutex);
    return ANVIL_OK;
}

anvil_backend_t *anvil_get_backend(anvil_ctx_t *ctx, anvil_arch_t arch)
{
    if (!ctx) return NULL;
    
    /* Find backend for architecture */
    const anvil_backend_ops_t *ops = NULL;
    for (size_t i = 0; i < num_backends; i++) {
        if (registered_backends[i]->arch == arch) {
            ops = registered_backends[i];
            break;
        }
    }
    
    if (!ops) return NULL;
    
    /* Create backend instance */
    anvil_backend_t *be = calloc(1, sizeof(anvil_backend_t));
    if (!be) return NULL;
    
    be->ops = ops;
    be->ctx = ctx;
    be->syntax = ctx->syntax;
    
    /* Initialize backend */
    if (ops->init) {
        anvil_error_t err = ops->init(be, ctx);
        if (err != ANVIL_OK) {
            free(be);
            return NULL;
        }
    }
    
    return be;
}
