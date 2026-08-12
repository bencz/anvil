#ifndef ANVIL_SMALLTALK_ARTIFACT_MATERIALIZE_INTERNAL_H
#define ANVIL_SMALLTALK_ARTIFACT_MATERIALIZE_INTERNAL_H

#include "st_artifact_materialize.h"

#include <stdbool.h>
#include <stddef.h>

int st_artifact_open_directory_no_symlinks(const char *path);

bool st_artifact_write_one(
    int directory,
    const char *name,
    const void *bytes,
    size_t length,
    st_artifact_materialize_write_fn writer,
    void *write_user);

int st_artifact_publish_noreplace(
    int directory,
    const char *staging,
    const char *publication_name);

#endif
