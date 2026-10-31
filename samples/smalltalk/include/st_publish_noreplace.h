#ifndef ANVIL_SMALLTALK_PUBLISH_NOREPLACE_H
#define ANVIL_SMALLTALK_PUBLISH_NOREPLACE_H

/* Publishes the staging directory `staging` as `name` (both in `directory`)
 * without replacing an existing entry; fails with EEXIST/ENOTEMPTY when the
 * name is taken.
 *
 * Linux: renameat2(RENAME_NOREPLACE).  macOS: renameatx_np(RENAME_EXCL).
 * Elsewhere, and where the kernel or file system rejects the flag, the name
 * is claimed with mkdirat (atomic) and the staging directory renamed over the
 * claimed empty directory; observers may briefly see that empty directory.
 * Header-only: used by the compiler and the product toolchain. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

#if defined(__linux__) && defined(SYS_renameat2) && !defined(RENAME_NOREPLACE)
#define RENAME_NOREPLACE (1u << 0)
#endif

static inline int st_publish_noreplace(int directory, const char *staging, const char *name)
{
#if defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, directory, staging, directory, name, RENAME_NOREPLACE) == 0)
        return 0;
    if (errno != ENOSYS && errno != EINVAL)
        return -1;
#elif defined(__APPLE__) && defined(RENAME_EXCL)
    if (renameatx_np(directory, staging, directory, name, RENAME_EXCL) == 0)
        return 0;
    if (errno != ENOTSUP && errno != EINVAL)
        return -1;
#endif
    if (mkdirat(directory, name, 0700) != 0)
        return -1;
    if (renameat(directory, staging, directory, name) != 0) {
        int saved = errno;
        (void)unlinkat(directory, name, AT_REMOVEDIR);
        errno = saved;
        return -1;
    }
    return 0;
}

#endif
