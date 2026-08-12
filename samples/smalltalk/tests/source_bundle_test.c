#define _POSIX_C_SOURCE 200809L

#include "st_source_bundle.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                         \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

#define TEST_PATH_CAPACITY 1024u
#define TEST_MAX_CREATED_PATHS 32u

typedef struct {
    char root[TEST_PATH_CAPACITY];
    char paths[TEST_MAX_CREATED_PATHS][TEST_PATH_CAPACITY];
    bool directories[TEST_MAX_CREATED_PATHS];
    size_t count;
} temp_tree_t;

static bool make_path(char *out, size_t capacity,
                      const char *left, const char *right)
{
    int result = snprintf(out, capacity, "%s/%s", left, right);
    return result >= 0 && (size_t)result < capacity;
}

static bool remember_path(temp_tree_t *tree, const char *path, bool directory)
{
    size_t length = strlen(path);
    if (tree->count == TEST_MAX_CREATED_PATHS
            || length >= sizeof(tree->paths[0])) {
        return false;
    }
    memcpy(tree->paths[tree->count], path, length + 1u);
    tree->directories[tree->count] = directory;
    tree->count++;
    return true;
}

static bool temp_tree_init(temp_tree_t *tree)
{
    char pattern[] = "/tmp/anvil-st-source-bundle-XXXXXX";
    char *root;
    memset(tree, 0, sizeof(*tree));
    root = mkdtemp(pattern);
    if (root == NULL || strlen(root) >= sizeof(tree->root)) return false;
    memcpy(tree->root, root, strlen(root) + 1u);
    return remember_path(tree, root, true);
}

static bool temp_tree_mkdir(temp_tree_t *tree, const char *relative)
{
    char path[TEST_PATH_CAPACITY];
    if (!make_path(path, sizeof(path), tree->root, relative)) return false;
    if (mkdir(path, 0700) != 0) return false;
    return remember_path(tree, path, true);
}

static bool write_all(int fd, const char *contents, size_t length)
{
    size_t offset = 0u;
    while (offset < length) {
        ssize_t amount = write(fd, contents + offset, length - offset);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return false;
        offset += (size_t)amount;
    }
    return true;
}

static bool temp_tree_write(temp_tree_t *tree, const char *relative,
                            const char *contents)
{
    char path[TEST_PATH_CAPACITY];
    int fd;
    bool ok;
    if (!make_path(path, sizeof(path), tree->root, relative)) return false;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    ok = write_all(fd, contents, strlen(contents));
    if (close(fd) != 0) ok = false;
    if (!ok) {
        unlink(path);
        return false;
    }
    return remember_path(tree, path, false);
}

static bool temp_tree_link(temp_tree_t *tree, const char *existing_relative,
                           const char *new_relative, bool symbolic)
{
    char existing[TEST_PATH_CAPACITY];
    char created[TEST_PATH_CAPACITY];
    int result;
    if (!make_path(existing, sizeof(existing), tree->root, existing_relative)
            || !make_path(created, sizeof(created), tree->root, new_relative)) {
        return false;
    }
    result = symbolic ? symlink(existing, created) : link(existing, created);
    return result == 0 && remember_path(tree, created, false);
}

static void temp_tree_destroy(temp_tree_t *tree)
{
    while (tree->count != 0u) {
        size_t index = --tree->count;
        if (tree->directories[index]) {
            (void)rmdir(tree->paths[index]);
        } else {
            (void)unlink(tree->paths[index]);
        }
    }
    memset(tree, 0, sizeof(*tree));
}

static bool prepare_image(temp_tree_t *tree, char *image, size_t capacity,
                          const char *manifest)
{
    if (!temp_tree_mkdir(tree, "st-image")
            || !make_path(image, capacity, tree->root, "st-image")
            || !temp_tree_write(tree, "st-image/manifest.txt", manifest)) {
        return false;
    }
    return true;
}

static bool ast_class_name_is(const st_source_file_t *file, const char *name)
{
    const st_ast_node_t *declaration;
    size_t length = strlen(name);
    if (file->ast.declarations.count != 1u) return false;
    declaration = file->ast.declarations.items[0];
    return declaration != NULL && declaration->kind == ST_AST_CLASS
        && declaration->as.class_decl.name != NULL
        && declaration->as.class_decl.name->kind == ST_AST_VARIABLE
        && declaration->as.class_decl.name->as.variable.name.length == length
        && memcmp(declaration->as.class_decl.name->as.variable.name.data,
                  name, length) == 0;
}

static void test_image_then_application_order(void)
{
    temp_tree_t tree;
    st_source_bundle_t bundle;
    char image[TEST_PATH_CAPACITY];
    char app_one[TEST_PATH_CAPACITY];
    char app_two[TEST_PATH_CAPACITY];
    const char *apps[2];

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image),
          "# bootstrap order is semantic\r\nObject.st\r\nCore/True.st\r\n"));
    CHECK(temp_tree_mkdir(&tree, "st-image/Core"));
    CHECK(temp_tree_write(&tree, "st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(temp_tree_write(&tree, "st-image/Core/True.st",
                          "ImageTrue := ImageObject [ ]"));
    CHECK(temp_tree_write(&tree, "st-image/Ignored.st",
                          "Ignored := Object [ ]"));
    CHECK(temp_tree_write(&tree, "AppOne.st", "AppOne := Object [ ]"));
    CHECK(temp_tree_write(&tree, "AppTwo.st", "AppTwo := Object [ ]"));
    CHECK(make_path(app_one, sizeof(app_one), tree.root, "AppOne.st"));
    CHECK(make_path(app_two, sizeof(app_two), tree.root, "AppTwo.st"));
    apps[0] = app_two;
    apps[1] = app_one;

    CHECK(st_source_bundle_load(&bundle, image, apps, 2u, NULL)
          == ST_SOURCE_LOAD_OK);
    CHECK(bundle.diagnostic.status == ST_SOURCE_LOAD_OK);
    CHECK(bundle.count == 4u);
    CHECK(bundle.image_count == 2u);
    CHECK(bundle.files[0].origin == ST_SOURCE_ORIGIN_IMAGE);
    CHECK(bundle.files[1].origin == ST_SOURCE_ORIGIN_IMAGE);
    CHECK(bundle.files[2].origin == ST_SOURCE_ORIGIN_APPLICATION);
    CHECK(bundle.files[3].origin == ST_SOURCE_ORIGIN_APPLICATION);
    CHECK(bundle.files[0].ordinal == 0u && bundle.files[3].ordinal == 3u);
    CHECK(bundle.files[0].manifest_line == 2u);
    CHECK(bundle.files[1].manifest_line == 3u);
    CHECK(strcmp(bundle.files[0].path, "Object.st") == 0);
    CHECK(strcmp(bundle.files[1].path, "Core/True.st") == 0);
    CHECK(strcmp(bundle.files[2].path, app_two) == 0);
    CHECK(strcmp(bundle.files[3].path, app_one) == 0);
    CHECK(ast_class_name_is(&bundle.files[0], "ImageObject"));
    CHECK(ast_class_name_is(&bundle.files[1], "ImageTrue"));
    CHECK(ast_class_name_is(&bundle.files[2], "AppTwo"));
    CHECK(ast_class_name_is(&bundle.files[3], "AppOne"));
    CHECK(bundle.files[0].source_length
          == strlen("ImageObject := Object [ ]"));
    CHECK(memcmp(bundle.files[0].source, "ImageObject := Object [ ]",
                 bundle.files[0].source_length) == 0);
    st_source_bundle_destroy(&bundle);
    CHECK(bundle.files == NULL && bundle.count == 0u);
    temp_tree_destroy(&tree);
}

static void test_two_manifest_product_load(void)
{
    temp_tree_t tree;
    st_source_bundle_t bundle;
    char image[TEST_PATH_CAPACITY];
    char application[TEST_PATH_CAPACITY];

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "Object.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(temp_tree_mkdir(&tree, "application"));
    CHECK(temp_tree_mkdir(&tree, "application/Support"));
    CHECK(temp_tree_write(
        &tree, "application/application.manifest",
        "# application order is semantic\r\nMain.st\r\nSupport/Helper.st\r\n"));
    CHECK(temp_tree_write(&tree, "application/Main.st",
                          "Main := ImageObject [ ]"));
    CHECK(temp_tree_write(&tree, "application/Support/Helper.st",
                          "Helper := ImageObject [ ]"));
    CHECK(make_path(application, sizeof(application), tree.root,
                    "application"));

    CHECK(st_source_bundle_load_manifests(
              &bundle, image, application, NULL) == ST_SOURCE_LOAD_OK);
    CHECK(bundle.count == 3u && bundle.image_count == 1u);
    CHECK(bundle.files[0].origin == ST_SOURCE_ORIGIN_IMAGE);
    CHECK(bundle.files[1].origin == ST_SOURCE_ORIGIN_APPLICATION);
    CHECK(bundle.files[2].origin == ST_SOURCE_ORIGIN_APPLICATION);
    CHECK(bundle.files[1].manifest_line == 2u);
    CHECK(bundle.files[2].manifest_line == 3u);
    CHECK(strcmp(bundle.files[1].path, "Main.st") == 0);
    CHECK(strcmp(bundle.files[2].path, "Support/Helper.st") == 0);
    CHECK(strncmp(bundle.files[1].source_name, application,
                  strlen(application)) == 0);
    CHECK(ast_class_name_is(&bundle.files[0], "ImageObject"));
    CHECK(ast_class_name_is(&bundle.files[1], "Main"));
    CHECK(ast_class_name_is(&bundle.files[2], "Helper"));
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "Object.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(temp_tree_mkdir(&tree, "application"));
    CHECK(temp_tree_write(&tree, "application/application.manifest",
                          "../Escape.st\n"));
    CHECK(make_path(application, sizeof(application), tree.root,
                    "application"));
    CHECK(st_source_bundle_load_manifests(
              &bundle, image, application, NULL)
          == ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    CHECK(bundle.diagnostic.phase == ST_SOURCE_PHASE_APPLICATION_MANIFEST);
    CHECK(bundle.diagnostic.manifest_line == 1u);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "Object.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(temp_tree_mkdir(&tree, "application"));
    CHECK(temp_tree_write(&tree, "application/application.manifest",
                          "Linked.st\n"));
    CHECK(temp_tree_write(&tree, "Outside.st", "Outside := Object [ ]"));
    CHECK(temp_tree_link(&tree, "Outside.st", "application/Linked.st", true));
    CHECK(make_path(application, sizeof(application), tree.root,
                    "application"));
    CHECK(st_source_bundle_load_manifests(
              &bundle, image, application, NULL)
          == ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    CHECK(bundle.diagnostic.phase == ST_SOURCE_PHASE_APPLICATION_SOURCE);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);
}

static void test_manifest_root_symlinks_rejected(void)
{
    temp_tree_t tree;
    st_source_bundle_t bundle;
    char image[TEST_PATH_CAPACITY];
    char application[TEST_PATH_CAPACITY];
    char linked[TEST_PATH_CAPACITY];

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "Object.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(temp_tree_link(&tree, "st-image", "image-link", true));
    CHECK(make_path(linked, sizeof(linked), tree.root, "image-link"));
    CHECK(st_source_bundle_load(&bundle, linked, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);

    CHECK(temp_tree_init(&tree));
    CHECK(temp_tree_mkdir(&tree, "container"));
    CHECK(temp_tree_mkdir(&tree, "container/st-image"));
    CHECK(temp_tree_write(&tree, "container/st-image/manifest.txt",
                          "Object.st\n"));
    CHECK(temp_tree_write(&tree, "container/st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(temp_tree_link(&tree, "container", "container-link", true));
    CHECK(make_path(linked, sizeof(linked), tree.root,
                    "container-link/st-image"));
    CHECK(st_source_bundle_load(&bundle, linked, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "Object.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(temp_tree_mkdir(&tree, "application"));
    CHECK(temp_tree_write(&tree, "application/application.manifest",
                          "Main.st\n"));
    CHECK(temp_tree_write(&tree, "application/Main.st",
                          "Main := ImageObject [ ]"));
    CHECK(temp_tree_link(&tree, "application", "application-link", true));
    CHECK(make_path(linked, sizeof(linked), tree.root, "application-link"));
    CHECK(st_source_bundle_load_manifests(&bundle, image, linked, NULL)
          == ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    CHECK(bundle.diagnostic.phase == ST_SOURCE_PHASE_APPLICATION_MANIFEST);
    st_source_bundle_destroy(&bundle);

    CHECK(temp_tree_mkdir(&tree, "outer"));
    CHECK(temp_tree_link(&tree, "application", "outer/app-link", true));
    CHECK(make_path(application, sizeof(application), tree.root,
                    "outer/app-link"));
    CHECK(st_source_bundle_load_manifests(
              &bundle, image, application, NULL)
          == ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);
}

static void test_missing_file_is_transactional(void)
{
    temp_tree_t tree;
    st_source_bundle_t bundle;
    char image[TEST_PATH_CAPACITY];

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image),
                        "Object.st\nAbsent.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_ERR_MISSING_FILE);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    CHECK(bundle.diagnostic.phase == ST_SOURCE_PHASE_IMAGE_SOURCE);
    CHECK(bundle.diagnostic.source_index == 1u);
    CHECK(bundle.diagnostic.manifest_line == 2u);
    CHECK(strstr(bundle.diagnostic.path, "Absent.st") != NULL);
    CHECK(bundle.diagnostic.system_error == ENOENT);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);
}

static void test_parse_error_is_transactional(void)
{
    temp_tree_t tree;
    st_source_bundle_t bundle;
    char image[TEST_PATH_CAPACITY];
    char app[TEST_PATH_CAPACITY];
    const char *apps[1];

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "Object.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(temp_tree_write(&tree, "Broken.st", "Broken := Object ["));
    CHECK(make_path(app, sizeof(app), tree.root, "Broken.st"));
    apps[0] = app;
    CHECK(st_source_bundle_load(&bundle, image, apps, 1u, NULL)
          == ST_SOURCE_LOAD_ERR_PARSE);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    CHECK(bundle.diagnostic.phase == ST_SOURCE_PHASE_APPLICATION_SOURCE);
    CHECK(bundle.diagnostic.source_index == 1u);
    CHECK(bundle.diagnostic.parse_error.status != ST_PARSE_OK);
    CHECK(strcmp(bundle.diagnostic.path, app) == 0);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);
}

static void test_image_failure_precedes_application_access(void)
{
    temp_tree_t tree;
    st_source_bundle_t bundle;
    char image[TEST_PATH_CAPACITY];
    char absent_app[TEST_PATH_CAPACITY];
    const char *apps[1];

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "Broken.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/Broken.st",
                          "BrokenImage := Object ["));
    CHECK(make_path(absent_app, sizeof(absent_app), tree.root,
                    "AbsentApplication.st"));
    apps[0] = absent_app;
    CHECK(st_source_bundle_load(&bundle, image, apps, 1u, NULL)
          == ST_SOURCE_LOAD_ERR_PARSE);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    CHECK(bundle.diagnostic.phase == ST_SOURCE_PHASE_IMAGE_SOURCE);
    CHECK(bundle.diagnostic.source_index == 0u);
    CHECK(bundle.diagnostic.manifest_line == 1u);
    CHECK(strstr(bundle.diagnostic.path, "Broken.st") != NULL);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);
}

static void expect_unsafe_manifest(const char *manifest)
{
    temp_tree_t tree;
    st_source_bundle_t bundle;
    char image[TEST_PATH_CAPACITY];
    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), manifest));
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    CHECK(bundle.diagnostic.phase == ST_SOURCE_PHASE_IMAGE_MANIFEST);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);
}

static void test_manifest_path_validation(void)
{
    expect_unsafe_manifest("../Escape.st\n");
    expect_unsafe_manifest("/absolute.st\n");
    expect_unsafe_manifest("Core/../Escape.st\n");
    expect_unsafe_manifest("Core//Object.st\n");
    expect_unsafe_manifest("Core/./Object.st\n");
    expect_unsafe_manifest("Core\\Object.st\n");
    expect_unsafe_manifest("C:/Object.st\n");
    expect_unsafe_manifest("Object.st/\n");
}

static void test_duplicate_spelling_and_file_identity(void)
{
    temp_tree_t tree;
    st_source_bundle_t bundle;
    char image[TEST_PATH_CAPACITY];
    char alias[TEST_PATH_CAPACITY];
    const char *apps[1];

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image),
                        "Object.st\nObject.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_ERR_DUPLICATE_SOURCE);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    CHECK(bundle.diagnostic.manifest_line == 2u);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "Object.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(temp_tree_link(&tree, "st-image/Object.st", "Alias.st", false));
    CHECK(make_path(alias, sizeof(alias), tree.root, "Alias.st"));
    apps[0] = alias;
    CHECK(st_source_bundle_load(&bundle, image, apps, 1u, NULL)
          == ST_SOURCE_LOAD_ERR_DUPLICATE_SOURCE);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    CHECK(bundle.diagnostic.phase == ST_SOURCE_PHASE_APPLICATION_SOURCE);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);
}

static void test_symlink_and_nonregular_rejected(void)
{
    temp_tree_t tree;
    st_source_bundle_t bundle;
    char image[TEST_PATH_CAPACITY];
    char directory_app[TEST_PATH_CAPACITY];
    const char *apps[1];

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "Alias.st\n"));
    CHECK(temp_tree_write(&tree, "Actual.st", "Actual := Object [ ]"));
    CHECK(temp_tree_link(&tree, "Actual.st", "st-image/Alias.st", true));
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image),
                        "LinkedDirectory/Escape.st\n"));
    CHECK(temp_tree_mkdir(&tree, "Outside"));
    CHECK(temp_tree_write(&tree, "Outside/Escape.st",
                          "Escape := Object [ ]"));
    CHECK(temp_tree_link(&tree, "Outside", "st-image/LinkedDirectory", true));
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "Object.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/Object.st",
                          "ImageObject := Object [ ]"));
    CHECK(temp_tree_mkdir(&tree, "AppDirectory"));
    CHECK(make_path(directory_app, sizeof(directory_app), tree.root,
                    "AppDirectory"));
    apps[0] = directory_app;
    CHECK(st_source_bundle_load(&bundle, image, apps, 1u, NULL)
          == ST_SOURCE_LOAD_ERR_NOT_REGULAR_FILE);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);
}

static void test_missing_and_empty_manifest(void)
{
    temp_tree_t tree;
    st_source_bundle_t bundle;
    char image[TEST_PATH_CAPACITY];

    CHECK(temp_tree_init(&tree));
    CHECK(temp_tree_mkdir(&tree, "st-image"));
    CHECK(make_path(image, sizeof(image), tree.root, "st-image"));
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_ERR_MISSING_FILE);
    CHECK(bundle.diagnostic.phase == ST_SOURCE_PHASE_IMAGE_MANIFEST);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "# only comments\n\n"));
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_ERR_INVALID_MANIFEST);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);
}

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t outstanding;
} fault_allocator_t;

static void *fault_allocate(void *user, size_t size)
{
    fault_allocator_t *fault = user;
    void *memory;
    if (fault->calls++ >= fault->fail_at) return NULL;
    memory = malloc(size);
    if (memory != NULL) fault->outstanding++;
    return memory;
}

static void fault_deallocate(void *user, void *pointer)
{
    fault_allocator_t *fault = user;
    if (pointer != NULL) {
        CHECK(fault->outstanding != 0u);
        if (fault->outstanding != 0u) fault->outstanding--;
        free(pointer);
    }
}

static void test_allocator_fault_injection(void)
{
    temp_tree_t tree;
    char image[TEST_PATH_CAPACITY];
    char app[TEST_PATH_CAPACITY];
    char manifest[256];
    const char *apps[1];
    size_t fail_at;
    size_t manifest_length = 0u;
    size_t index;
    bool reached_success = false;

    CHECK(temp_tree_init(&tree));
    for (index = 0u; index < 9u; index++) {
        int amount = snprintf(manifest + manifest_length,
                              sizeof(manifest) - manifest_length,
                              "Image%zu.st\n", index);
        CHECK(amount > 0
              && (size_t)amount < sizeof(manifest) - manifest_length);
        if (amount > 0
                && (size_t)amount < sizeof(manifest) - manifest_length) {
            manifest_length += (size_t)amount;
        }
    }
    CHECK(prepare_image(&tree, image, sizeof(image), manifest));
    for (index = 0u; index < 9u; index++) {
        char relative[64];
        char contents[96];
        int relative_length = snprintf(relative, sizeof(relative),
                                       "st-image/Image%zu.st", index);
        int contents_length = snprintf(contents, sizeof(contents),
                                       "Image%zu := Object [ ]", index);
        CHECK(relative_length > 0
              && (size_t)relative_length < sizeof(relative));
        CHECK(contents_length > 0
              && (size_t)contents_length < sizeof(contents));
        if (relative_length > 0 && contents_length > 0
                && (size_t)relative_length < sizeof(relative)
                && (size_t)contents_length < sizeof(contents)) {
            CHECK(temp_tree_write(&tree, relative, contents));
        }
    }
    CHECK(temp_tree_write(&tree, "App.st", "App := Object [ ]"));
    CHECK(make_path(app, sizeof(app), tree.root, "App.st"));
    apps[0] = app;

    for (fail_at = 0u; fail_at < 256u; fail_at++) {
        fault_allocator_t fault;
        st_source_allocator_t allocator;
        st_source_bundle_t bundle;
        st_source_load_status_t status;
        memset(&fault, 0, sizeof(fault));
        fault.fail_at = fail_at;
        allocator.allocate = fault_allocate;
        allocator.deallocate = fault_deallocate;
        allocator.user = &fault;
        status = st_source_bundle_load(&bundle, image, apps, 1u, &allocator);
        if (status == ST_SOURCE_LOAD_OK) {
            CHECK(bundle.count == 10u);
            st_source_bundle_destroy(&bundle);
            CHECK(fault.outstanding == 0u);
            reached_success = true;
            break;
        }
        CHECK(status == ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY);
        CHECK(bundle.count == 0u && bundle.files == NULL);
        st_source_bundle_destroy(&bundle);
        CHECK(fault.outstanding == 0u);
    }
    CHECK(reached_success);
    temp_tree_destroy(&tree);
}

static void test_argument_and_overflow_errors(void)
{
    st_source_bundle_t bundle;
    const char *dummy = "unused.st";
    st_source_allocator_t invalid_allocator;
    st_source_limits_t invalid_limits;

    memset(&invalid_allocator, 0, sizeof(invalid_allocator));
    invalid_allocator.allocate = fault_allocate;
    CHECK(st_source_bundle_load(&bundle, "image", NULL, 1u, NULL)
          == ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT);
    st_source_bundle_destroy(&bundle);
    CHECK(st_source_bundle_load(&bundle, "image", &dummy, SIZE_MAX, NULL)
          == ST_SOURCE_LOAD_ERR_OVERFLOW);
    st_source_bundle_destroy(&bundle);
    CHECK(st_source_bundle_load(&bundle, "image", NULL, 0u,
                                &invalid_allocator)
          == ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT);
    st_source_bundle_destroy(&bundle);
    CHECK(st_source_bundle_load(NULL, "image", NULL, 0u, NULL)
          == ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT);
    memset(&invalid_limits, 0, sizeof(invalid_limits));
    CHECK(st_source_bundle_load_with_limits(&bundle, "image", NULL, 0u,
                                            NULL, &invalid_limits)
          == ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT);
    st_source_bundle_destroy(&bundle);
}

static void test_resource_limits(void)
{
    temp_tree_t tree;
    st_source_bundle_t bundle;
    st_source_limits_t limits;
    char image[TEST_PATH_CAPACITY];

    CHECK(temp_tree_init(&tree));
    CHECK(prepare_image(&tree, image, sizeof(image), "One.st\nTwo.st\n"));
    CHECK(temp_tree_write(&tree, "st-image/One.st",
                          "One := Object [ ]"));
    CHECK(temp_tree_write(&tree, "st-image/Two.st",
                          "Two := Object [ ]"));

    limits.max_manifest_bytes = 4u;
    limits.max_file_bytes = 64u;
    limits.max_total_bytes = 128u;
    limits.max_files = 8u;
    CHECK(st_source_bundle_load_with_limits(&bundle, image, NULL, 0u,
                                            NULL, &limits)
          == ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    st_source_bundle_destroy(&bundle);

    limits.max_manifest_bytes = 64u;
    limits.max_file_bytes = 8u;
    CHECK(st_source_bundle_load_with_limits(&bundle, image, NULL, 0u,
                                            NULL, &limits)
          == ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED);
    st_source_bundle_destroy(&bundle);

    limits.max_file_bytes = 64u;
    limits.max_total_bytes = strlen("One := Object [ ]");
    CHECK(st_source_bundle_load_with_limits(&bundle, image, NULL, 0u,
                                            NULL, &limits)
          == ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED);
    st_source_bundle_destroy(&bundle);

    limits.max_total_bytes = 128u;
    limits.max_files = 1u;
    CHECK(st_source_bundle_load_with_limits(&bundle, image, NULL, 0u,
                                            NULL, &limits)
          == ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED);
    CHECK(bundle.count == 0u && bundle.files == NULL);
    st_source_bundle_destroy(&bundle);
    temp_tree_destroy(&tree);
}

int main(void)
{
    test_image_then_application_order();
    test_two_manifest_product_load();
    test_manifest_root_symlinks_rejected();
    test_missing_file_is_transactional();
    test_parse_error_is_transactional();
    test_image_failure_precedes_application_access();
    test_manifest_path_validation();
    test_duplicate_spelling_and_file_identity();
    test_symlink_and_nonregular_rejected();
    test_missing_and_empty_manifest();
    test_allocator_fault_injection();
    test_argument_and_overflow_errors();
    test_resource_limits();
    if (failures != 0u) {
        fprintf(stderr, "smalltalk source bundle: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk source bundle: PASS");
    return EXIT_SUCCESS;
}
