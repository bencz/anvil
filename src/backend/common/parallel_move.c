#include "parallel_move.h"

bool anvil_parallel_move(const anvil_parallel_move_ops_t *ops, void *user, const int *dst, const int *src, size_t count)
{
    if (count > ANVIL_PARALLEL_MOVE_MAX)
        return false;

    int pending_dst[ANVIL_PARALLEL_MOVE_MAX], pending_src[ANVIL_PARALLEL_MOVE_MAX];
    size_t pending = 0;
    for (size_t i = 0; i < count; i++) {
        if (src[i] == dst[i])
            continue;
        pending_dst[pending] = dst[i];
        pending_src[pending++] = src[i];
    }

    while (pending) {
        /* A move is ready when no other pending move still reads its
         * destination. */
        size_t ready = pending;
        for (size_t m = 0; m < pending && ready == pending; m++) {
            bool read_later = false;
            for (size_t other = 0; other < pending; other++)
                read_later |= other != m && pending_src[other] == pending_dst[m];
            if (!read_later)
                ready = m;
        }

        if (ready == pending) {
            /* Only cycles remain. */
            int freed = pending_dst[0];
            int holder;
            if (ops->swap) {
                ops->swap(user, pending_src[0], pending_dst[0]);
                holder = pending_src[0];
                ready = 0;
            } else {
                if (ops->scratch < 0)
                    return false;
                ops->move(user, ops->scratch, freed);
                holder = ops->scratch;
                ready = pending;
            }
            for (size_t other = 0; other < pending; other++)
                if (other != ready && pending_src[other] == freed)
                    pending_src[other] = holder;
            if (ready == pending)
                continue;
        } else {
            ops->move(user, pending_dst[ready], pending_src[ready]);
        }

        pending_dst[ready] = pending_dst[pending - 1];
        pending_src[ready] = pending_src[pending - 1];
        pending--;
        /* Swapping can turn a remaining move into a self-move. */
        for (size_t m = 0; m < pending;) {
            if (pending_src[m] == pending_dst[m]) {
                pending_dst[m] = pending_dst[pending - 1];
                pending_src[m] = pending_src[pending - 1];
                pending--;
            } else {
                m++;
            }
        }
    }
    return true;
}
