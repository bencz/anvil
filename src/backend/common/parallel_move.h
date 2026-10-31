/* Register-to-register parallel moves for printers: argument registers,
 * result pairs and fixed instruction operands, whose sources may sit in
 * one another's destinations.  The resolver orders the moves and breaks
 * cycles with the target's swap, or through a scratch register. */
#ifndef ANVIL_BACKEND_PARALLEL_MOVE_H
#define ANVIL_BACKEND_PARALLEL_MOVE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    void (*move)(void *user, int dst, int src);
    /* Exchanges two registers; NULL when the target has no swap. */
    void (*swap)(void *user, int a, int b);
    /* Used to break cycles when `swap` is NULL; never a source or
     * destination of the moves. */
    int scratch;
} anvil_parallel_move_ops_t;

#define ANVIL_PARALLEL_MOVE_MAX 16

/* Moves src[i] to dst[i] for every i as if all at once. Destinations must
 * be distinct. False when `count` exceeds ANVIL_PARALLEL_MOVE_MAX or a
 * cycle needs a scratch register that is not given. */
bool anvil_parallel_move(const anvil_parallel_move_ops_t *ops, void *user, const int *dst, const int *src, size_t count);

#endif
