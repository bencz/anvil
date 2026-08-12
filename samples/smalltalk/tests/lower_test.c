#include "st_lower.h"
#include "st_parser.h"
#include "st_core_primitives.h"
#include "st_block_primitives.h"
#include "st_heap_primitives.h"
#include "st_image_runtime.h"
#include "st_float_primitives.h"
#include "st_integer_primitives.h"
#include "st_stream_primitive_bridge.h"
#include "st_stream_primitives.h"
#include "st_string_primitives.h"
#include "st_dispatch.h"
#include "st_runtime.h"
#include "st_value.h"

#include "anvil/anvil_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned failures;

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
enum { EXECUTED_METHOD_COUNT = 15 };
enum { FIXTURE_COUNT = 21 };
enum { CLOSURE_METHOD_COUNT = 14 };
enum { CLOSURE_BLOCK_COUNT = 17 };

#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                     \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

typedef struct {
    char *source;
    st_ast_unit_t unit;
    st_parser_t parser;
} fixture_t;

typedef struct {
    st_lower_result_t result;
    char *assembly;
    size_t assembly_length;
} compiled_t;

static bool text_is(st_ast_string_t value, const char *expected)
{
    size_t length = strlen(expected);
    return value.length == length && value.data != NULL
        && memcmp(value.data, expected, length) == 0;
}

static bool read_file(const char *path, char **source_out)
{
    FILE *file = fopen(path, "rb");
    long length;
    char *source;
    if (!file || fseek(file, 0, SEEK_END) != 0
            || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file) fclose(file);
        return false;
    }
    source = malloc((size_t)length + 1u);
    if (!source) {
        fclose(file);
        return false;
    }
    bool ok = fread(source, 1u, (size_t)length, file) == (size_t)length;
    source[length] = '\0';
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        free(source);
        return false;
    }
    *source_out = source;
    return true;
}

static bool fixture_init_owned(fixture_t *fixture, const char *name,
                               char *source)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->source = source;
    if (!source || !st_ast_unit_init(&fixture->unit, name)
            || !st_parser_init_cstr(&fixture->parser, &fixture->unit, source)
            || !st_parse_compilation_unit(&fixture->parser)
            || !st_parser_at_end(&fixture->parser)) {
        fprintf(stderr, "parse failed for %s\n", name);
        st_parser_destroy(&fixture->parser);
        st_ast_unit_destroy(&fixture->unit);
        free(fixture->source);
        memset(fixture, 0, sizeof(*fixture));
        return false;
    }
    return true;
}

static bool fixture_init_text(fixture_t *fixture, const char *name,
                              const char *source)
{
    size_t length = strlen(source);
    char *copy = malloc(length + 1u);
    if (!copy) return false;
    memcpy(copy, source, length + 1u);
    return fixture_init_owned(fixture, name, copy);
}

static bool fixture_init_file(fixture_t *fixture, const char *path)
{
    char *source = NULL;
    const char *local_prefix = "samples/smalltalk/";
    const char *resolved = path;
    if (!read_file(resolved, &source)
            && strncmp(path, local_prefix, strlen(local_prefix)) == 0) {
        resolved = path + strlen(local_prefix);
        if (!read_file(resolved, &source)) return false;
    }
    return source != NULL && fixture_init_owned(fixture, resolved, source);
}

static void fixture_destroy(fixture_t *fixture)
{
    st_parser_destroy(&fixture->parser);
    st_ast_unit_destroy(&fixture->unit);
    free(fixture->source);
    memset(fixture, 0, sizeof(*fixture));
}

static const st_class_graph_entity_t *find_class(
    const st_class_graph_result_t *graph, const char *name)
{
    for (size_t index = 0u; index < graph->entity_count; index++) {
        const st_class_graph_entity_t *entity = &graph->entities[index];
        if (entity->kind == ST_CLASS_GRAPH_CLASS
                && entity->namespace_id == ST_CLASS_GRAPH_INVALID_ID
                && text_is(entity->name, name)) return entity;
    }
    return NULL;
}

static const st_class_graph_method_t *find_method(
    const st_class_graph_result_t *graph, const char *class_name,
    const char *selector)
{
    const st_class_graph_entity_t *entity = find_class(graph, class_name);
    if (!entity) return NULL;
    for (size_t index = 0u; index < graph->method_count; index++) {
        if (graph->methods[index].owner == entity->id
                && text_is(graph->methods[index].selector, selector))
            return &graph->methods[index];
    }
    return NULL;
}

static const st_class_graph_method_t *find_class_method(
    const st_class_graph_result_t *graph, const char *class_name,
    const char *selector)
{
    const st_class_graph_entity_t *entity = find_class(graph, class_name);
    if (!entity || entity->metaclass_id == ST_CLASS_GRAPH_INVALID_ID)
        return NULL;
    for (size_t index = 0u; index < graph->method_count; index++) {
        if (graph->methods[index].owner == entity->metaclass_id
                && graph->methods[index].class_side
                && text_is(graph->methods[index].selector, selector))
            return &graph->methods[index];
    }
    return NULL;
}

static const st_primitive_binding_t *find_primitive_binding(
    const st_primitive_result_t *primitives, const st_ast_node_t *method)
{
    if (primitives == NULL || !primitives->resolved
            || primitives->status != ST_PRIMITIVE_OK) return NULL;
    for (size_t index = 0u; index < primitives->binding_count; index++)
        if (primitives->bindings[index].method == method)
            return &primitives->bindings[index];
    return NULL;
}

static bool analyze(const st_class_graph_result_t *graph,
                    const st_class_graph_method_t *method,
                    st_sema_result_t *sema)
{
    st_class_graph_sema_view_t view;
    bool ok = false;
    st_sema_result_init(sema);
    st_class_graph_sema_view_init(&view);
    if (method != NULL
            && st_class_graph_sema_view_build_minimal(
                &view, graph, method->id) == ST_CLASS_GRAPH_OK)
        ok = st_sema_analyze_method(
                 sema, method->node, &view.catalog) == ST_SEMA_OK
          && st_sema_succeeded(sema);
    st_class_graph_sema_view_destroy(&view);
    return ok;
}

static bool analyze_with_external_global(
    const st_class_graph_result_t *graph,
    const st_class_graph_method_t *method, const char *name,
    uint32_t external_id, st_sema_result_t *sema)
{
    st_class_graph_sema_view_t view;
    st_sema_external_t *entries = NULL;
    bool ok = false;
    st_sema_result_init(sema);
    st_class_graph_sema_view_init(&view);
    if (method == NULL
            || st_class_graph_sema_view_build_minimal(
                &view, graph, method->id) != ST_CLASS_GRAPH_OK)
        goto done;
    if (view.catalog.count == SIZE_MAX
            || view.catalog.count + 1u
                > SIZE_MAX / sizeof(*entries))
        goto done;
    entries = malloc((view.catalog.count + 1u) * sizeof(*entries));
    if (entries == NULL) goto done;
    if (view.catalog.count != 0u)
        memcpy(entries, view.catalog.entries,
               view.catalog.count * sizeof(*entries));
    entries[view.catalog.count] = (st_sema_external_t) {
        .name = { name, strlen(name) },
        .kind = ST_SEMA_EXTERNAL_GLOBAL,
        .slot = ST_SEMA_INVALID_ID,
        .external_id = external_id
    };
    st_sema_catalog_t catalog = {
        .entries = entries,
        .count = view.catalog.count + 1u,
        .has_lexical_super = view.catalog.has_lexical_super
    };
    ok = st_sema_analyze_method(sema, method->node, &catalog) == ST_SEMA_OK
      && st_sema_succeeded(sema);
done:
    free(entries);
    st_class_graph_sema_view_destroy(&view);
    return ok;
}

static bool compile_method(anvil_ctx_t *ctx,
                           const st_class_graph_result_t *graph,
                           const char *class_name, const char *selector,
                           const char *symbol,
                           const st_selector_table_t *selectors,
                           const st_primitive_binding_t *primitive_binding,
                           compiled_t *compiled)
{
    const st_class_graph_method_t *method = find_method(
        graph, class_name, selector);
    if (method == NULL)
        method = find_class_method(graph, class_name, selector);
    st_sema_result_t sema;
    st_lower_options_t options;
    memset(compiled, 0, sizeof(*compiled));
    st_lower_result_init(&compiled->result);
    if (!analyze(graph, method, &sema)) return false;
    memset(&options, 0, sizeof(options));
    options.symbol_name = symbol;
    options.linkage = ANVIL_LINK_EXTERNAL;
    options.selectors = selectors;
    options.primitive_binding = primitive_binding;
    bool ok = st_lower_method(&compiled->result, ctx, graph, method->id,
                              &sema, &options) == ST_LOWER_OK
        && compiled->result.module != NULL
        && compiled->result.function != NULL
        && anvil_module_codegen(compiled->result.module,
                                &compiled->assembly,
                                &compiled->assembly_length) == ANVIL_OK
        && compiled->assembly != NULL && compiled->assembly_length != 0u;
    if (!ok) {
        fprintf(stderr, "lower %s>>%s failed: %s / %s\n",
                class_name, selector,
                st_lower_status_string(compiled->result.status),
                anvil_ctx_get_error(ctx));
    }
    st_sema_result_destroy(&sema);
    return ok;
}

static bool compile_method_with_global(
    anvil_ctx_t *ctx, const st_class_graph_result_t *graph,
    const char *class_name, const char *selector, const char *symbol,
    const st_selector_table_t *selectors,
    const st_primitive_binding_t *primitive_binding,
    const char *global_name, uint32_t image_slot, compiled_t *compiled)
{
    const st_class_graph_method_t *method = find_method(
        graph, class_name, selector);
    st_sema_result_t sema;
    st_lower_options_t options;
    st_lower_global_binding_t global = {0};
    bool found_global = false;

    memset(compiled, 0, sizeof(*compiled));
    st_lower_result_init(&compiled->result);
    if (!analyze(graph, method, &sema)) return false;
    for (size_t index = 0u; index < sema.binding_count; index++) {
        const st_sema_binding_t *binding = &sema.bindings[index];
        if (binding->kind == ST_SEMA_BIND_GLOBAL
                && text_is(binding->name, global_name)) {
            global.semantic_external_id = binding->external_id;
            global.runtime_index = image_slot;
            found_global = true;
            break;
        }
    }
    memset(&options, 0, sizeof(options));
    options.symbol_name = symbol;
    options.linkage = ANVIL_LINK_EXTERNAL;
    options.selectors = selectors;
    options.primitive_binding = primitive_binding;
    options.globals = &global;
    options.global_count = found_global ? 1u : 0u;
    bool ok = found_global
        && st_lower_method(&compiled->result, ctx, graph, method->id,
                           &sema, &options) == ST_LOWER_OK
        && compiled->result.module != NULL
        && compiled->result.function != NULL
        && anvil_module_codegen(compiled->result.module,
                                &compiled->assembly,
                                &compiled->assembly_length) == ANVIL_OK
        && compiled->assembly != NULL && compiled->assembly_length != 0u;
    if (!ok) {
        fprintf(stderr, "lower %s>>%s with %s failed: %s / %s\n",
                class_name, selector, global_name,
                st_lower_status_string(compiled->result.status),
                anvil_ctx_get_error(ctx));
    }
    st_sema_result_destroy(&sema);
    return ok;
}

static void compiled_destroy(compiled_t *compiled)
{
    free(compiled->assembly);
    st_lower_result_destroy(&compiled->result);
    memset(compiled, 0, sizeof(*compiled));
}

static bool function_has_op(const anvil_func_t *function, anvil_op_t op)
{
    if (!function) return false;
    for (const anvil_block_t *block = function->blocks; block;
         block = block->next) {
        for (const anvil_instr_t *instruction = block->first; instruction;
             instruction = instruction->next) {
            if (instruction->op == op) return true;
        }
    }
    return false;
}

static bool first_site_has_lexical_owner(const compiled_t *compiled,
                                         uint32_t owner)
{
    const anvil_global_t *global = compiled->result.module
        ? compiled->result.module->globals : NULL;
    const anvil_value_t *initializer = global && global->value
        ? global->value->data.global.init : NULL;
    return initializer != NULL
        && initializer->kind == ANVIL_VAL_CONST_STRUCT
        && initializer->data.aggregate.num_elements == 5u
        && initializer->data.aggregate.elements[1] != NULL
        && initializer->data.aggregate.elements[1]->kind == ANVIL_VAL_CONST_INT
        && initializer->data.aggregate.elements[1]->data.u == owner;
}

static bool write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok;
}

static void execute_x86_nlr_methods(const compiled_t *branch_return,
                                    const compiled_t *both_return);

static void execute_x86_methods(compiled_t *methods, size_t count)
{
#if defined(__x86_64__) && !defined(_WIN32)
    long pid = (long)getpid();
    char paths[EXECUTED_METHOD_COUNT][160];
    char harness_path[160], executable_path[160], command[1800];
    const char *harness =
        "#include \"st_dispatch.h\"\n"
        "#include <stdint.h>\n"
        "extern uint64_t st_HelloApplication_run(StFrame *);\n"
        "extern uint64_t st_Object_yourself(StFrame *);\n"
        "extern uint64_t st_True_not(StFrame *);\n"
        "extern uint64_t st_False_not(StFrame *);\n"
        "extern uint64_t st_True_and(StFrame *);\n"
        "extern uint64_t st_Probe_assignments(StFrame *);\n"
        "extern uint64_t st_Probe_character(StFrame *);\n"
        "extern uint64_t st_Probe_branch(StFrame *);\n"
        "extern uint64_t st_Probe_branchFalse(StFrame *);\n"
        "extern uint64_t st_Probe_branchReturn(StFrame *);\n"
        "extern uint64_t st_Probe_bothReturn(StFrame *);\n"
        "extern uint64_t st_Probe_implicit(StFrame *);\n"
        "extern uint64_t st_Probe_nilLiteral(StFrame *);\n"
        "extern uint64_t st_Probe_minSmallInteger(StFrame *);\n"
        "extern uint64_t st_Probe_maxSmallInteger(StFrame *);\n"
        "int main(void) {\n"
        "  uint64_t argv[1] = { 0x2000 };\n"
        "  StFrame f = { .receiver = 0x1000, .argv = argv, .argc = 1 };\n"
        "  if (st_HelloApplication_run(&f) != 19) return 1;\n"
        "  if (st_Object_yourself(&f) != 0x1000) return 2;\n"
        "  if (st_True_not(&f) != 11 || st_False_not(&f) != 19) return 3;\n"
        "  if (st_True_and(&f) != 0x2000) return 4;\n"
        "  if (st_Probe_assignments(&f) != 337) return 5;\n"
        "  if (st_Probe_character(&f) != 522) return 6;\n"
        "  if (st_Probe_branch(&f) != 329) return 7;\n"
        "  if (st_Probe_branchFalse(&f) != 57) return 8;\n"
        "  /* NLR methods are exercised below with their real control ABI. */\n"
        "  if (st_Probe_implicit(&f) != 0x1000) return 11;\n"
        "  if (st_Probe_nilLiteral(&f) != 3) return 12;\n"
        "  if (st_Probe_minSmallInteger(&f) != UINT64_C(0x8000000000000001)) return 13;\n"
        "  if (st_Probe_maxSmallInteger(&f) != UINT64_C(0x7ffffffffffffff9)) return 14;\n"
        "  return 0;\n"
        "}\n";
    CHECK(count == EXECUTED_METHOD_COUNT);
    bool ok = count == EXECUTED_METHOD_COUNT;
    for (size_t index = 0u; index < count; index++) {
        snprintf(paths[index], sizeof(paths[index]),
                 "/tmp/anvil-st-lower-%ld-%zu.s", pid, index);
        ok = write_text(paths[index], methods[index].assembly) && ok;
    }
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-st-lower-%ld.c", pid);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-st-lower-%ld", pid);
    ok = write_text(harness_path, harness) && ok;
    CHECK(ok);
    if (ok) {
        int offset = snprintf(command, sizeof(command),
            "/usr/bin/cc -std=c11 -no-pie -Iinclude -I../../include "
            "-Isamples/smalltalk/include ");
        for (size_t index = 0u; index < count && offset > 0
                && (size_t)offset < sizeof(command); index++) {
            offset += snprintf(command + offset, sizeof(command) - (size_t)offset,
                               "%s ", paths[index]);
        }
        if (offset > 0 && (size_t)offset < sizeof(command)) {
            snprintf(command + offset, sizeof(command) - (size_t)offset,
                     "%s samples/smalltalk/src/runtime/value.c "
                     "samples/smalltalk/src/runtime/runtime.c "
                     "samples/smalltalk/src/runtime/lookup.c "
                     "samples/smalltalk/src/runtime/send_bridge.c "
                     "samples/smalltalk/src/runtime/control/control.c "
                     "samples/smalltalk/src/runtime/control/control_bridge.c -o %s -pthread",
                     harness_path, executable_path);
            CHECK(system(command) == 0);
            CHECK(system(executable_path) == 0);
        } else {
            CHECK(false);
        }
    }
    for (size_t index = 0u; index < count; index++) unlink(paths[index]);
    unlink(harness_path);
    unlink(executable_path);
    if (count > 10u)
        execute_x86_nlr_methods(&methods[9], &methods[10]);
#else
    (void)methods;
    (void)count;
#endif
}

static void execute_x86_nlr_methods(const compiled_t *branch_return,
                                    const compiled_t *both_return)
{
#if defined(__x86_64__) && !defined(_WIN32)
    long pid = (long)getpid();
    char first_path[160], second_path[160], harness_path[160];
    char executable_path[160], command[1800], harness[9000];
    const st_lower_root_map_t *map = branch_return->result.root_maps;
    CHECK(branch_return->result.root_map_count == 1u);
    CHECK(both_return->result.root_map_count == 1u);
    CHECK(branch_return->result.required_root_capacity == 2u);
    CHECK(map && map->safepoint_id == 1u && map->root_count == 2u
          && map->bitmap_word_count == 1u && map->live_root_bitmap
          && map->live_root_bitmap[0] == UINT64_C(3));
    if (!map || !map->live_root_bitmap) return;
    snprintf(first_path, sizeof(first_path),
             "/tmp/anvil-st-nlr-%ld-first.s", pid);
    snprintf(second_path, sizeof(second_path),
             "/tmp/anvil-st-nlr-%ld-second.s", pid);
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-st-nlr-%ld.c", pid);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-st-nlr-%ld", pid);
    int length = snprintf(harness, sizeof(harness),
        "#include \"st_control_bridge.h\"\n"
        "#include <signal.h>\n#include <stdint.h>\n#include <string.h>\n#include <sys/wait.h>\n#include <unistd.h>\n"
        "extern uint64_t st_Probe_branchReturn(StFrame*);\n"
        "extern uint64_t st_Probe_bothReturn(StFrame*);\n"
        "static uint64_t bits=UINT64_C(0x%llx);\n"
        "static st_root_map_t maps[1]={{1,%u,1,&bits}};\n"
        "static st_unwind_region_t unwind[1]={{0,1,0,ST_UNWIND_NON_LOCAL_RETURN,0}};\n"
        "static StMethodDescriptor md={ST_METHOD_ABI_VERSION,77,1,0,%u,%u,1,0,0,0,0,maps,1,unwind,1};\n"
        "int main(void){const char*n[7]={\"Object\",\"Nil\",\"False\",\"True\",\"SmallInteger\",\"Character\","
        "\"Class\"};StClassDescriptor cs[7];StShapeDescriptor ss[7];const StClassDescriptor*cp[7];const "
        "StShapeDescriptor*sp[7];for(uint32_t i=0;i<7;i++){cs[i]=(StClassDescriptor){i+1,(i==0||i==6)?0:1,7,i+1,"
        "i==6?ST_CLASS_METACLASS:0,n[i],strlen(n[i]),0,0};ss[i]=(StShapeDescriptor){i+1,i+1,8,24,0,"
        "ST_INDEXED_NONE,0,0};cp[i]=&cs[i];sp[i]=&ss[i];}st_runtime_descriptors_t d={cp,7,sp,7};"
        "if(st_runtime_descriptors_validate(&d)!=ST_RUNTIME_OK)return 1;st_lookup_context_t l={0};"
        "if(st_lookup_context_init(&l,&d,(st_lookup_allocator_t){0})!=ST_LOOKUP_FOUND)return 2;uint32_t ids[5]={2,"
        "3,4,5,6};st_aot_thread_t t={0};st_control_thread_t c={0};if(st_control_thread_init(&c,&t,"
        "(st_control_allocator_t){0})!=ST_CONTROL_OK)return 3;if(!st_aot_thread_init(&t,&l,ids,0,&c,0,0,0,0,"
        "0))return 4;uint64_t roots[%u]={0};StFrame f={.thread=&t,.method=&md,.receiver=st_value_true(),"
        ".roots=roots,.root_count=%u};if(st_Probe_branchReturn(&f)!=((UINT64_C(41)<<3)|1)||f.home)return 5;"
        "memset(roots,0,sizeof(roots));if(st_Probe_bothReturn(&f)!=((UINT64_C(41)<<3)|1)||f.home)return 6;"
        "st_aot_thread_destroy(&t);if(st_control_thread_destroy(&c)!=ST_CONTROL_OK)return 7;"
        "st_lookup_context_destroy(&l);return 0;}\n",
        (unsigned long long)map->live_root_bitmap[0], map->root_count,
        branch_return->result.required_root_capacity,
        branch_return->result.method_flags,
        branch_return->result.required_root_capacity,
        branch_return->result.required_root_capacity);
    bool ok = length > 0 && (size_t)length < sizeof(harness)
        && write_text(first_path, branch_return->assembly)
        && write_text(second_path, both_return->assembly)
        && write_text(harness_path, harness);
    CHECK(ok);
    if (ok) {
        snprintf(command, sizeof(command),
            "/usr/bin/cc -std=c11 -no-pie -Iinclude -Isamples/smalltalk/include "
            "%s %s %s samples/smalltalk/src/runtime/value.c "
            "samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/runtime/lookup.c "
            "samples/smalltalk/src/runtime/send_bridge.c "
            "samples/smalltalk/src/runtime/control/control.c "
            "samples/smalltalk/src/runtime/control/control_bridge.c -o %s -pthread",
            first_path, second_path, harness_path, executable_path);
        CHECK(system(command) == 0);
        int execution_status = system(executable_path);
        if (execution_status != 0)
            fprintf(stderr, "primitive executable status=%d exit=%d\n",
                    execution_status,
                    WIFEXITED(execution_status)
                        ? WEXITSTATUS(execution_status) : -1);
        CHECK(execution_status == 0);
    }
    unlink(first_path);
    unlink(second_path);
    unlink(harness_path);
    unlink(executable_path);
#else
    (void)branch_return;
    (void)both_return;
#endif
}

static void execute_x86_general_send(const compiled_t *caller,
                                     const compiled_t *nlr_callee,
                                     st_selector_id_t selector_id)
{
#if defined(__x86_64__) && !defined(_WIN32)
    long pid = (long)getpid();
    char caller_path[160], callee_path[160], harness_path[160];
    char executable_path[160], command[1400];
    char harness[9000];
    CHECK(caller->result.root_map_count == 2u);
    CHECK(caller->result.required_root_capacity == 4u);
    const st_lower_root_map_t *map = caller->result.root_maps;
    CHECK(map != NULL && map->safepoint_id == 1u);
    CHECK(map != NULL && map->root_count == 4u);
    CHECK(map != NULL && map->bitmap_word_count == 1u);
    CHECK(map != NULL && map->live_root_bitmap != NULL
          && map->live_root_bitmap[0] == UINT64_C(15));
    CHECK(nlr_callee->result.required_root_capacity == 3u);
    CHECK(nlr_callee->result.root_map_count == 1u);
    const st_lower_root_map_t *nlr_map = nlr_callee->result.root_maps;
    CHECK(nlr_map != NULL && nlr_map->safepoint_id == 1u
          && nlr_map->root_count == 3u
          && nlr_map->bitmap_word_count == 1u
          && nlr_map->live_root_bitmap != NULL
          && nlr_map->live_root_bitmap[0] == UINT64_C(7));
    CHECK((nlr_callee->result.method_flags
           & (ST_METHOD_CAN_UNWIND | ST_METHOD_HAS_NON_LOCAL_RETURN))
          == (ST_METHOD_CAN_UNWIND | ST_METHOD_HAS_NON_LOCAL_RETURN));
    if (map == NULL || map->live_root_bitmap == NULL || nlr_map == NULL
            || nlr_map->live_root_bitmap == NULL) return;
    snprintf(caller_path, sizeof(caller_path),
             "/tmp/anvil-st-send-%ld-caller.s", pid);
    snprintf(callee_path, sizeof(callee_path),
             "/tmp/anvil-st-send-%ld-callee.s", pid);
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-st-send-%ld.c", pid);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-st-send-%ld", pid);
    int length = snprintf(harness, sizeof(harness),
        "#include \"st_control_bridge.h\"\n"
        "#include <stdint.h>\n#include <string.h>\n"
        "extern uint64_t st_Probe_general(StFrame *);\n"
        "extern uint64_t st_Probe_nlrSend(StFrame *);\n"
        "static int zero_seen;static uint64_t zero_leaf(StFrame*f){if(!f||f->argc!=1||f->roots||f->root_count) "
        "return 0;zero_seen=1;return st_value_false();}\n"
        "static uint64_t bits[1]={UINT64_C(0x%llx)},nlr_bits[1]={UINT64_C(0x%llx)};\n"
        "static st_root_map_t maps[2]={{%u,%u,%zu,bits},{2,%u,%zu,bits}};\n"
        "static st_root_map_t nlr_maps[1]={{%u,%u,%zu,nlr_bits}};\n"
        "static st_unwind_region_t nlr_unwind[1]={{0,1,0,ST_UNWIND_NON_LOCAL_RETURN,0}};\n"
        "static StMethodDescriptor caller={%u,99,1,0,%u,%u,1,0,0,0,0,maps,2,0,0};\n"
        "static StMethodDescriptor leaf={%u,%u,4,1,0,0,1};\n"
        "static StMethodDescriptor nlr={%u,%u,4,1,%u,%u,1,0,0,0,0,nlr_maps,1,nlr_unwind,1};\n"
        "static StMethodBinding binding={&leaf,zero_leaf,1};\n"
        "static StMethodBinding nlr_binding={&nlr,st_Probe_nlrSend,2};\n"
        "static StMethodEntry entry; static st_method_slot_t slot={%u,&entry};\n"
        "static StClassDescriptor cs[7]; static StShapeDescriptor ss[7];\n"
        "static const StClassDescriptor *cp[7]; static const StShapeDescriptor *sp[7];\n"
        "static st_value_t fail(void*u,StFrame*f,const st_send_site_t*s,st_value_t r,const st_value_t*a,uint32_t "
        "n,st_aot_send_status_t e){(void)u;(void)f;(void)s;(void)r;(void)a;(void)n;(void)e;return st_value_nil();}"
        "\n"
        "int main(void){const char*n[7]={\"Object\",\"Nil\",\"False\",\"True\",\"SmallInteger\",\"Character\",\"Class\"};"
        "for(uint32_t i=0;i<7;i++){cs[i]=(StClassDescriptor){i+1,(i==0||i==6)?0:1,7,i+1,i==6?ST_CLASS_METACLASS:0,"
        "n[i],strlen(n[i]),0,0};ss[i]=(StShapeDescriptor){i+1,i+1,8,24,0,ST_INDEXED_NONE,0,0};cp[i]=&cs[i];"
        "sp[i]=&ss[i];}"
        "if(!st_method_entry_init(&entry,&binding))return 1;cs[3].method_slots=&slot;cs[3].method_slot_count=1;"
        "st_runtime_descriptors_t d={cp,7,sp,7};if(st_runtime_descriptors_validate(&d)!=ST_RUNTIME_OK)return 2;"
        "st_lookup_context_t l={0};if(st_lookup_context_init(&l,&d,(st_lookup_allocator_t){0})!=ST_LOOKUP_FOUND)return 3;"
        "uint32_t ids[5]={2,3,4,5,6};st_aot_thread_t t={0};st_control_thread_t c={0};if(st_control_thread_init(&c,"
        "&t,(st_control_allocator_t){0})!=ST_CONTROL_OK)return 4;if(!st_aot_thread_init(&t,&l,ids,0,&c,0,0,0,fail,"
        "0))return 4;"
        "uint64_t roots[%u]={0};StFrame f={.thread=&t,.method=&caller,.receiver=UINT64_C(0x1000),.roots=roots,.root_count=%u};"
        "uint64_t r=st_Probe_general(&f);if(r!=st_value_false()||!zero_seen)return 5;if(f.safepoint_id!=0)return "
        "6;if(roots[0]!=UINT64_C(0x1000)||roots[1]!=st_value_nil()||roots[2]!=st_value_nil()||"
        "roots[3]!=st_value_false())return 7;"
        "if(st_lookup_publish_binding(&l,&entry,&nlr_binding,0)!=ST_LOOKUP_FOUND)return 8;memset(roots,0,"
        "sizeof(roots));r=st_Probe_general(&f);if(r!=((UINT64_C(41)<<3)|1))return 9;"
        "if(f.safepoint_id||roots[3]!=r)return 10;"
        "st_aot_thread_destroy(&t);if(st_control_thread_destroy(&c)!=ST_CONTROL_OK)return 13;st_lookup_context_destroy(&l);return 0;}\n",
        (unsigned long long)map->live_root_bitmap[0],
        (unsigned long long)nlr_map->live_root_bitmap[0], map->safepoint_id,
        map->root_count, map->bitmap_word_count, map->root_count,
        map->bitmap_word_count, nlr_map->safepoint_id,
        nlr_map->root_count, nlr_map->bitmap_word_count,
        ST_METHOD_ABI_VERSION,
        caller->result.required_root_capacity, caller->result.method_flags,
        ST_METHOD_ABI_VERSION, selector_id,
        ST_METHOD_ABI_VERSION, selector_id,
        nlr_callee->result.required_root_capacity,
        nlr_callee->result.method_flags,
        selector_id, caller->result.required_root_capacity,
        caller->result.required_root_capacity);
    bool ok = length > 0 && (size_t)length < sizeof(harness)
        && write_text(caller_path, caller->assembly)
        && write_text(callee_path, nlr_callee->assembly)
        && write_text(harness_path, harness);
    CHECK(ok);
    if (ok) {
        snprintf(command, sizeof(command),
            "/usr/bin/cc -std=c11 -no-pie -Iinclude -Isamples/smalltalk/include "
            "%s %s %s samples/smalltalk/src/runtime/value.c "
            "samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/runtime/lookup.c "
            "samples/smalltalk/src/runtime/send_bridge.c "
            "samples/smalltalk/src/runtime/control/control.c "
            "samples/smalltalk/src/runtime/control/control_bridge.c -o %s -pthread",
            caller_path, callee_path, harness_path, executable_path);
        CHECK(system(command) == 0);
        CHECK(system(executable_path) == 0);
    }
    unlink(caller_path);
    unlink(callee_path);
    unlink(harness_path);
    unlink(executable_path);
#else
    (void)caller;
    (void)nlr_callee;
    (void)selector_id;
#endif
}

static bool write_closure_block_metadata(
    FILE *file, const st_lower_block_artifact_t *block, size_t ordinal)
{
    bool ok = true;
    if (block->capture_count != 0u) {
        ok = fprintf(file,
            "static st_aot_capture_descriptor_t block_captures_%zu[%zu]={",
            ordinal, block->capture_count) > 0;
        for (size_t capture = 0u; ok && capture < block->capture_count;
             capture++) {
            ok = fprintf(file, "%s{%u,%u}", capture ? "," : "",
                block->captures[capture].binding_id,
                block->captures[capture].kind) > 0;
        }
        ok = ok && fprintf(file, "};\n") > 0;
    }
    if (block->root_map_count != 0u) {
        ok = ok && fprintf(file,
            "static uint64_t block_bits_%zu=UINT64_C(0x%llx);\n"
            "static st_root_map_t block_maps_%zu[%zu]={",
            ordinal,
            (unsigned long long)block->root_maps[0].live_root_bitmap[0],
            ordinal, block->root_map_count) > 0;
        for (size_t map_index = 0u; ok
                && map_index < block->root_map_count; map_index++) {
            const st_lower_root_map_t *map = &block->root_maps[map_index];
            ok = fprintf(file, "%s{%u,%u,1,&block_bits_%zu}",
                         map_index ? "," : "", map->safepoint_id,
                         map->root_count, ordinal) > 0;
        }
        ok = ok && fprintf(file, "};\n") > 0;
    }
    bool block_nlr = (block->method_flags
                      & ST_METHOD_HAS_NON_LOCAL_RETURN) != 0u;
    if (block_nlr)
        ok = ok && fprintf(file,
            "static st_unwind_region_t block_unwind_%zu={0,1,0,ST_UNWIND_NON_LOCAL_RETURN,0};\n",
            ordinal) > 0;
    ok = ok && fprintf(file,
        "const StMethodDescriptor %.*s={ST_METHOD_ABI_VERSION,%zu,2,%u,%u,%u,1,0,0,0,0,",
        (int)block->method_descriptor_symbol.length,
        block->method_descriptor_symbol.bytes, 200u + ordinal,
        block->arity, block->required_root_capacity,
        block->method_flags) > 0;
    if (block->root_map_count != 0u)
        ok = ok && fprintf(file, "block_maps_%zu,%zu,", ordinal,
                           block->root_map_count) > 0;
    else
        ok = ok && fprintf(file, "0,0,") > 0;
    if (block_nlr)
        ok = ok && fprintf(file, "&block_unwind_%zu,1};\n", ordinal) > 0;
    else
        ok = ok && fprintf(file, "0,0};\n") > 0;
    if (block->capture_count != 0u)
        ok = ok && fprintf(file,
            "const st_aot_block_descriptor_t %.*s={%u,%u,%zu,%u,%.*s,&%.*s,block_captures_%zu,%zu};\n",
            (int)block->descriptor_symbol.length,
            block->descriptor_symbol.bytes,
            ST_AOT_BLOCK_ABI_VERSION, block->arity,
            block->capture_count, block->flags,
            (int)block->code_symbol.length, block->code_symbol.bytes,
            (int)block->method_descriptor_symbol.length,
            block->method_descriptor_symbol.bytes, ordinal,
            block->capture_count) > 0;
    else
        ok = ok && fprintf(file,
            "const st_aot_block_descriptor_t %.*s={%u,%u,0,%u,%.*s,&%.*s,0,0};\n",
            (int)block->descriptor_symbol.length,
            block->descriptor_symbol.bytes,
            ST_AOT_BLOCK_ABI_VERSION, block->arity, block->flags,
            (int)block->code_symbol.length, block->code_symbol.bytes,
            (int)block->method_descriptor_symbol.length,
            block->method_descriptor_symbol.bytes) > 0;
    return ok;
}

static bool write_closure_harness(const char *path,
                                  const compiled_t methods[CLOSURE_METHOD_COUNT],
                                  const compiled_t *while_true_method,
                                  st_selector_id_t yourself_selector,
                                  st_selector_id_t while_true_selector)
{
    static const char *const factories[CLOSURE_METHOD_COUNT] = {
        "st_Probe_closureMake0", "st_Probe_closureMake1",
        "st_Probe_closureCall0", "st_Probe_closureCall1",
        "st_Probe_closureNlr", "st_Probe_closureCapture",
        "st_Probe_closureSelf", "st_Probe_closureReturned",
        "st_Probe_closureArity2", "st_Probe_closureArity3",
        "st_Probe_closureNestedCall", "st_Probe_closureCellSiblings",
        "st_Probe_closureSend", "st_Probe_closureWhileLoop"
    };
    static const uint32_t factory_arities[CLOSURE_METHOD_COUNT] = {
        0u, 0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u
    };
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fprintf(file,
        "#include \"st_closure_bridge.h\"\n"
        "#include \"st_block_primitive_bridge.h\"\n"
        "#include \"st_control_bridge.h\"\n"
        "#include <signal.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n#include "
        "<sys/wait.h>\n#include <unistd.h>\n"
        "extern uint64_t st_Block_whileTrue(StFrame*);\n") > 0;
    size_t artifact_ordinal = 0u;
    for (size_t index = 0u; ok && index < CLOSURE_METHOD_COUNT; index++) {
        const st_lower_result_t *result = &methods[index].result;
        ok = result->blocks && result->block_count != 0u
            && fprintf(file, "extern uint64_t %s(StFrame*);\n",
                       factories[index]) > 0;
        for (size_t block_index = 0u; ok
                && block_index < result->block_count; block_index++) {
            const st_lower_block_artifact_t *block =
                &result->blocks[block_index];
            ok = fprintf(file, "extern uint64_t %.*s(StFrame*);\n",
                         (int)block->code_symbol.length,
                         block->code_symbol.bytes) > 0;
        }
        if (!ok) break;
        uint64_t factory_bitmap = methods[index].result.root_maps
            ? methods[index].result.root_maps[0].live_root_bitmap[0] : 0u;
        ok = fprintf(file,
            "static uint64_t factory_bits_%zu=UINT64_C(0x%llx);\n"
            "static st_root_map_t factory_maps_%zu[%zu]={",
            index, (unsigned long long)factory_bitmap, index,
            methods[index].result.root_map_count) > 0;
        for (size_t map_index = 0u; ok
                && map_index < methods[index].result.root_map_count;
             map_index++) {
            const st_lower_root_map_t *map =
                &methods[index].result.root_maps[map_index];
            ok = fprintf(file, "%s{%u,%u,1,&factory_bits_%zu}",
                         map_index ? "," : "", map->safepoint_id,
                         map->root_count, index) > 0;
        }
        ok = ok && fprintf(file, "};\n") > 0;
        bool factory_nlr = (methods[index].result.method_flags
                            & ST_METHOD_HAS_NON_LOCAL_RETURN) != 0u;
        if (factory_nlr)
            ok = fprintf(file,
                "static st_unwind_region_t factory_unwind_%zu={0,1,0,ST_UNWIND_NON_LOCAL_RETURN,0};\n",
                index) > 0;
        if (factory_nlr)
            ok = ok && fprintf(file,
                "static StMethodDescriptor factory_md_%zu={ST_METHOD_ABI_VERSION,%zu,1,%u,%u,%u,1,0,0,0,0,"
                "factory_maps_%zu,%zu,&factory_unwind_%zu,1};\n",
                index, 100u + index, factory_arities[index],
                methods[index].result.required_root_capacity,
                methods[index].result.method_flags, index,
                methods[index].result.root_map_count, index) > 0;
        else
            ok = ok && fprintf(file,
                "static StMethodDescriptor factory_md_%zu={ST_METHOD_ABI_VERSION,%zu,1,%u,%u,%u,1,0,0,0,0,factory_maps_%zu,%zu,0,0};\n",
                index, 100u + index, factory_arities[index],
                methods[index].result.required_root_capacity,
                methods[index].result.method_flags, index,
                methods[index].result.root_map_count) > 0;

        for (size_t block_index = 0u; ok
                && block_index < result->block_count; block_index++) {
            ok = write_closure_block_metadata(
                file, &result->blocks[block_index], artifact_ordinal++);
        }
    }
    for (size_t map_index = 0u;
         ok && map_index < while_true_method->result.root_map_count;
         map_index++) {
        const st_lower_root_map_t *map =
            &while_true_method->result.root_maps[map_index];
        ok = fprintf(file,
            "static uint64_t while_bits_%zu[%zu]={",
            map_index, map->bitmap_word_count) > 0;
        for (size_t word = 0u;
             ok && word < map->bitmap_word_count; word++) {
            ok = fprintf(file, "%sUINT64_C(0x%llx)",
                         word == 0u ? "" : ",",
                         (unsigned long long)map->live_root_bitmap[word]) > 0;
        }
        ok = ok && fprintf(file, "};\n") > 0;
    }
    ok = ok && fprintf(file, "static st_root_map_t while_maps[%zu]={",
                       while_true_method->result.root_map_count) > 0;
    for (size_t map_index = 0u;
         ok && map_index < while_true_method->result.root_map_count;
         map_index++) {
        const st_lower_root_map_t *map =
            &while_true_method->result.root_maps[map_index];
        ok = fprintf(file, "%s{%u,%u,%zu,while_bits_%zu}",
                     map_index == 0u ? "" : ",", map->safepoint_id,
                     map->root_count, map->bitmap_word_count, map_index) > 0;
    }
    ok = ok && fprintf(file,
        "};\nstatic StMethodDescriptor while_md={ST_METHOD_ABI_VERSION,%u,2,1,%u,%u,1,0,0,0,0,while_maps,%zu,0,0};\n",
        while_true_selector,
        while_true_method->result.required_root_capacity,
        while_true_method->result.method_flags,
        while_true_method->result.root_map_count) > 0;
    if (!ok) { fclose(file); return false; }
    ok = fprintf(file, "static const st_aot_block_descriptor_t*all_blocks[%u]={",
                 CLOSURE_BLOCK_COUNT) > 0;
    artifact_ordinal = 0u;
    for (size_t index = 0u; ok && index < CLOSURE_METHOD_COUNT; index++) {
        for (size_t block_index = 0u; ok
                && block_index < methods[index].result.block_count;
             block_index++) {
            const st_lower_block_artifact_t *block =
                &methods[index].result.blocks[block_index];
            ok = fprintf(file, "%s&%.*s", artifact_ordinal ? "," : "",
                         (int)block->descriptor_symbol.length,
                         block->descriptor_symbol.bytes) > 0;
            artifact_ordinal++;
        }
    }
    ok = ok && fprintf(file,
        "};\n"
        "static uint64_t si(int64_t n){uint64_t v=0;if(!st_value_from_small_integer(n,&v))_exit(90);return v;}\n"
        "typedef uint64_t(*Code)(StFrame*);\n"
        "static uint64_t run(Code code,const StMethodDescriptor*md,uint64_t recv,uint32_t argc,uint64_t*argv,"
        "st_aot_thread_t*t){uint64_t roots[32];if(md->frame_root_capacity>32)_exit(91);for(uint32_t i=0;"
        "i<md->frame_root_capacity;i++)roots[i]=st_value_nil();StFrame f={.thread=t,.method=md,.receiver=recv,"
        ".argv=argv,.roots=md->frame_root_capacity?roots:0,.argc=argc,.root_count=md->frame_root_capacity};return "
        "code(&f);}\n"
        "static StMethodDescriptor plain={ST_METHOD_ABI_VERSION,999,1,0,0,0,1};\n"
        "static uint64_t invoke(st_aot_thread_t*t,uint64_t closure,uint32_t argc,uint64_t*argv,"
        "st_aot_closure_target_t*out){StFrame caller={.thread=t,.method=&plain,.receiver=st_value_true()};"
        "st_aot_closure_target_t target;if(st_aot_closure_resolve(&caller,closure,argc,"
        "&target)!=ST_AOT_CLOSURE_OK)_exit(92);uint64_t roots[8];if(target.frame_root_capacity>8)_exit(93);"
        "for(uint32_t i=0;i<target.frame_root_capacity;i++)roots[i]=st_value_nil();if(target.frame_root_capacity){"
        "roots[0]=closure;for(uint32_t i=0;i<argc;i++)roots[i+1]=argv[i];}StFrame child={.thread=t,"
        ".caller=&caller,.method=target.method,.home=target.home,.receiver=closure,.argv=argv,"
        ".roots=target.frame_root_capacity?roots:0,.argc=argc,.root_count=target.frame_root_capacity};"
        "if(out)*out=target;return target.code(&child);}\n"
        "static uint64_t unwindable_yourself(StFrame*f){return f->receiver;}\n"
        "static bool object_class(void*u,uint64_t v,uint32_t*out){st_object_view_t view;if(!out||"
        "st_heap_object_view(u,v,&view)!=ST_HEAP_OK)return false;*out=view.class_descriptor->class_id;return true;}\n") > 0;
    ok = ok && fprintf(file,
        "int main(void){enum{O=1,B=2,CL=3,N=4,F=5,T=6,I=7,C=8,M=9,Z=9};const char*n[Z]={\"Object\",\"Block\",\"ClosureCell\",\"Nil\","
        "\"False\",\"True\",\"SmallInteger\",\"Character\",\"Metaclass\"};uint64_t bb=0,cb=1;StClassDescriptor cs[Z];"
        "StShapeDescriptor ss[Z];const StClassDescriptor*cp[Z];const StShapeDescriptor*sp[Z];for(uint32_t i=0;i<Z;"
        "i++){cs[i]=(StClassDescriptor){i+1,(i==0||i==8)?0:1,9,i+1,i==8?ST_CLASS_METACLASS:0,n[i],strlen(n[i]),0,"
        "0};ss[i]=(StShapeDescriptor){i+1,i+1,8,24,0,ST_INDEXED_NONE,0,0};cp[i]=&cs[i];sp[i]=&ss[i];}"
        "ss[B-1]=(StShapeDescriptor){B,B,8,56,4,ST_INDEXED_VALUES,&bb,1};"
        "ss[CL-1]=(StShapeDescriptor){CL,CL,8,32,1,ST_INDEXED_NONE,&cb,1};"
        "StMethodDescriptor yd={ST_METHOD_ABI_VERSION,%u,T,0,0,ST_METHOD_CAN_UNWIND,1};"
        "StMethodBinding yb={&yd,unwindable_yourself,1};StMethodEntry ye;if(!st_method_entry_init(&ye,&yb))return 29;"
        "st_method_slot_t ys={%u,&ye};cs[T-1].method_slots=&ys;cs[T-1].method_slot_count=1;"
        "StMethodBinding wb={&while_md,st_Block_whileTrue,1};StMethodEntry we;if(!st_method_entry_init(&we,&wb))return 36;"
        "st_method_slot_t ws={%u,&we};cs[B-1].method_slots=&ws;cs[B-1].method_slot_count=1;"
        "st_runtime_descriptors_t d={cp,Z,sp,Z};"
        "if(st_runtime_descriptors_validate(&d)!=ST_RUNTIME_OK)return 1;st_heap_t h={0};if(st_heap_init(&h,&d,"
        "(st_runtime_allocator_t){0})!=ST_HEAP_OK)return 2;st_aot_closure_context_t closures={0};"
        "st_aot_closure_options_t co={.heap=&h,.closure_class_id=B,.closure_shape_id=B,.cell_class_id=CL,.cell_shape_id=CL,.descriptors=all_blocks,"
        ".descriptor_count=%u};if(st_aot_closure_context_init(&closures,&co)!=ST_AOT_CLOSURE_OK)return 3;"
        "st_lookup_context_t lookup={0};if(st_lookup_context_init(&lookup,&d,(st_lookup_allocator_t){0}"
        ")!=ST_LOOKUP_FOUND)return 4;st_aot_thread_t t={0};st_control_thread_t control={0};"
        "if(st_control_thread_init(&control,&t,(st_control_allocator_t){0})!=ST_CONTROL_OK)return 5;uint32_t "
        "ids[5]={N,F,T,I,C};if(!st_aot_thread_init(&t,&lookup,ids,0,&control,&closures,object_class,&h,0,0))return 6;uint64_t "
        "a[1],cl;st_heap_collection_stats_t gs;cl=run(st_Probe_closureMake0,&factory_md_0,st_value_true(),0,0,&t);"
        "if(invoke(&t,cl,0,0,0)!=si(41))return 7;if(st_heap_collect(&h,0,0,0,&gs)!=ST_HEAP_OK)return 8;"
        "cl=run(st_Probe_closureMake1,&factory_md_1,st_value_true(),0,0,&t);a[0]=si(47);if(invoke(&t,cl,1,a,"
        "0)!=a[0])return 9;if(st_heap_collect(&h,0,0,0,&gs)!=ST_HEAP_OK)return 10;if(run(st_Probe_closureCall0,"
        "&factory_md_2,st_value_true(),0,0,&t)!=si(42))return 11;if(run(st_Probe_closureCall1,&factory_md_3,"
        "st_value_true(),0,0,&t)!=si(43))return 12;if(run(st_Probe_closureNlr,&factory_md_4,st_value_true(),0,0,"
        "&t)!=si(44))return 13;if(st_control_live_token_count(&control)==0)return 14;if(st_heap_collect(&h,0,0,0,"
        "&gs)!=ST_HEAP_OK||st_control_live_token_count(&control)!=0)return 15;uint64_t captured;"
        "if(st_heap_allocate(&h,O,O,0,0,0,&captured)!=ST_HEAP_OK)return 16;a[0]=captured;"
        "cl=run(st_Probe_closureCapture,&factory_md_5,st_value_true(),1,a,&t);uint64_t keep[1]={cl};"
        "if(st_heap_collect(&h,0,keep,1,&gs)!=ST_HEAP_OK||!st_heap_contains(&h,captured))return 17;if(invoke(&t,"
        "cl,0,0,0)!=captured)return 18;if(st_heap_collect(&h,0,0,0,&gs)!=ST_HEAP_OK||st_heap_contains(&h,"
        "captured))return 19;cl=run(st_Probe_closureSelf,&factory_md_6,st_value_true(),0,0,&t);if(invoke(&t,cl,0,"
        "0,0)!=st_value_true())return 20;if(st_heap_collect(&h,0,0,0,&gs)!=ST_HEAP_OK)return 21;"
        "cl=run(st_Probe_closureReturned,&factory_md_7,st_value_true(),0,0,&t);st_aot_closure_target_t rt={0};"
        "StFrame cf={.thread=&t,.method=&plain,.receiver=st_value_true()};if(st_aot_closure_resolve(&cf,cl,0,"
        "&rt)!=ST_AOT_CLOSURE_OK||!rt.home)return 22;if(st_aot_control_non_local_return(&cf,rt.home,"
        "si(45))!=ST_CONTROL_ERR_BLOCK_RETURNED)return 23;pid_t p=fork();if(p<0)return 24;if(p==0){"
        "(void)invoke(&t,cl,0,0,0);_exit(99);}int s=0;if(waitpid(p,&s,"
        "0)!=p||!WIFSIGNALED(s)||WTERMSIG(s)!=SIGABRT)return 25;if(st_heap_collect(&h,0,0,0,"
        "&gs)!=ST_HEAP_OK||st_control_live_token_count(&control)!=0)return 26;"
        "if(run(st_Probe_closureArity2,&factory_md_8,st_value_true(),0,0,&t)!=si(42))return 30;"
        "if(run(st_Probe_closureArity3,&factory_md_9,st_value_true(),0,0,&t)!=si(43))return 31;"
        "a[0]=si(55);if(run(st_Probe_closureNestedCall,&factory_md_10,st_value_true(),1,a,&t)!=a[0])return 32;"
        "if(run(st_Probe_closureCellSiblings,&factory_md_11,st_value_true(),0,0,&t)!=si(2))return 33;"
        "if(run(st_Probe_closureSend,&factory_md_12,st_value_true(),0,0,&t)!=st_value_true())return 34;"
        "if(run(st_Probe_closureWhileLoop,&factory_md_13,st_value_true(),0,0,&t)!=si(3))return 37;"
        "if(st_heap_collect(&h,0,0,0,&gs)!=ST_HEAP_OK)return 35;st_aot_thread_destroy(&t);"
        "if(st_aot_closure_context_destroy(&closures)!=ST_AOT_CLOSURE_OK)return 27;"
        "if(st_control_thread_destroy(&control)!=ST_CONTROL_OK)return 28;st_lookup_context_destroy(&lookup);"
        "st_heap_destroy(&h);return 0;}\n",
        yourself_selector, yourself_selector, while_true_selector,
        CLOSURE_BLOCK_COUNT) > 0;
    return fclose(file) == 0 && ok;
}

static void execute_x86_closures(
    const compiled_t methods[CLOSURE_METHOD_COUNT],
    const compiled_t *while_true_method,
    st_selector_id_t yourself_selector,
    st_selector_id_t while_true_selector)
{
#if defined(__x86_64__) && !defined(_WIN32)
    long pid = (long)getpid();
    char paths[CLOSURE_METHOD_COUNT][160];
    char while_true_path[160];
    char harness_path[160], executable_path[160], command[6000];
    bool ok = true;
    for (size_t index = 0u; index < CLOSURE_METHOD_COUNT; index++) {
        snprintf(paths[index], sizeof(paths[index]),
                 "/tmp/anvil-st-closure-%ld-%zu.s", pid, index);
        ok = write_text(paths[index], methods[index].assembly) && ok;
    }
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-st-closure-%ld.c", pid);
    snprintf(while_true_path, sizeof(while_true_path),
             "/tmp/anvil-st-closure-%ld-while.s", pid);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-st-closure-%ld", pid);
    ok = write_text(while_true_path, while_true_method->assembly) && ok;
    ok = write_closure_harness(
        harness_path, methods, while_true_method,
        yourself_selector, while_true_selector) && ok;
    CHECK(ok);
    if (ok) {
        int length = snprintf(command, sizeof(command),
            "/usr/bin/cc -std=c11 -no-pie -Iinclude "
            "-Isamples/smalltalk/include ");
        for (size_t index = 0u; length > 0
                && (size_t)length < sizeof(command)
                && index < CLOSURE_METHOD_COUNT; index++)
            length += snprintf(command + length,
                sizeof(command) - (size_t)length, "%s ", paths[index]);
        if (length > 0 && (size_t)length < sizeof(command))
            length += snprintf(command + length,
                sizeof(command) - (size_t)length,
                "%s %s samples/smalltalk/src/runtime/value.c "
                "samples/smalltalk/src/runtime/runtime.c "
                "samples/smalltalk/src/runtime/lookup.c "
                "samples/smalltalk/src/runtime/heap.c "
                "samples/smalltalk/src/runtime/image_runtime.c "
                "samples/smalltalk/src/runtime/control/control.c "
                "samples/smalltalk/src/runtime/control/control_roots.c "
                "samples/smalltalk/src/runtime/send_bridge.c "
                "samples/smalltalk/src/runtime/control/control_bridge.c "
                "samples/smalltalk/src/runtime/closure_bridge.c "
                "samples/smalltalk/src/runtime/primitives/block_primitives.c "
                "samples/smalltalk/src/runtime/primitives/block_primitive_bridge.c "
                "-o %s -pthread", while_true_path, harness_path,
                executable_path);
        CHECK(length > 0 && (size_t)length < sizeof(command));
        if (length > 0 && (size_t)length < sizeof(command)) {
            CHECK(system(command) == 0);
            int execution_status = system(executable_path);
            if (execution_status != 0)
                fprintf(stderr, "closure executable status=%d exit=%d\n",
                        execution_status,
                        WIFEXITED(execution_status)
                            ? WEXITSTATUS(execution_status) : -1);
            CHECK(execution_status == 0);
        }
    }
    for (size_t index = 0u; index < CLOSURE_METHOD_COUNT; index++)
        unlink(paths[index]);
    unlink(while_true_path);
    unlink(harness_path);
    unlink(executable_path);
#else
    (void)methods;
    (void)while_true_method;
    (void)yourself_selector;
    (void)while_true_selector;
#endif
}

static void execute_x86_primitives(const compiled_t methods[5])
{
#if defined(__x86_64__) && !defined(_WIN32)
    long pid = (long)getpid();
    char paths[5][160], harness_path[160], executable_path[160];
    char command[2200];
    const char *harness =
        "#include \"st_send_bridge.h\"\n#include \"st_value.h\"\n"
        "#include <signal.h>\n#include <stdint.h>\n#include <string.h>\n#include <sys/wait.h>\n#include <unistd.h>\n"
        "extern uint64_t st_Object_identity_primitive(StFrame*);\n"
        "extern uint64_t st_Character_value_primitive(StFrame*);\n"
        "extern uint64_t st_Probe_primitiveAdd(StFrame*);\n"
        "extern uint64_t st_Probe_primitiveDivide(StFrame*);\n"
        "extern uint64_t st_SmallInteger_negated_primitive(StFrame*);\n"
        "static uint64_t si(int64_t n){uint64_t v=0;if(!st_value_from_small_integer(n,&v))_exit(90);return v;}\n"
        "static uint64_t fallback_leaf(StFrame*f){if(!f||f->argc!=2)return 0;return si(123);}\n"
        "int main(void){uint64_t a[1];StFrame f={.argv=a,.argc=1};"
        "f.receiver=si(42);a[0]=si(42);if(st_Object_identity_primitive(&f)!=st_value_true())return 1;"
        "f.argc=0;f.argv=0;f.receiver=((uint64_t)'A'<<3)|2;if(st_Character_value_primitive(&f)!=si(65))return 2;"
        "f.argc=1;f.argv=a;f.receiver=si(3);a[0]=si(5);if(st_Probe_primitiveAdd(&f)!=si(8))return 3;"
        "f.receiver=si(1152921504606846975LL);a[0]=si(1);if(st_Probe_primitiveAdd(&f)!=si(7))return 4;"
        "f.receiver=st_value_true();a[0]=si(1);if(st_Probe_primitiveAdd(&f)!=si(7))return 5;"
        "f.receiver=si(8);a[0]=si(2);if(st_Probe_primitiveDivide(&f)!=si(4))return 6;"
        "a[0]=si(0);if(st_Probe_primitiveDivide(&f)!=si(99))return 7;"
        "f.receiver=st_value_true();a[0]=si(2);if(st_Probe_primitiveDivide(&f)!=si(99))return 8;"
        "const char*n[7]={\"Object\",\"Nil\",\"False\",\"True\",\"SmallInteger\",\"Character\",\"Class\"};"
        "StClassDescriptor cs[7];StShapeDescriptor ss[7];const StClassDescriptor*cp[7];const "
        "StShapeDescriptor*sp[7];for(uint32_t i=0;i<7;i++){cs[i]=(StClassDescriptor){i+1,(i==0||i==6)?0:1,7,i+1,"
        "i==6?ST_CLASS_METACLASS:0,n[i],strlen(n[i]),0,0};ss[i]=(StShapeDescriptor){i+1,i+1,8,24,0,"
        "ST_INDEXED_NONE,0,0};cp[i]=&cs[i];sp[i]=&ss[i];}StMethodDescriptor fd={ST_METHOD_ABI_VERSION,4,5,2,0,0,1}"
        ";StMethodBinding fb={&fd,fallback_leaf,1};StMethodEntry fe;if(!st_method_entry_init(&fe,&fb))return 9;"
        "st_method_slot_t fs={4,&fe};cs[4].method_slots=&fs;cs[4].method_slot_count=1;st_runtime_descriptors_t d={"
        "cp,7,sp,7};if(st_runtime_descriptors_validate(&d)!=ST_RUNTIME_OK)return 10;st_lookup_context_t l={0};"
        "if(st_lookup_context_init(&l,&d,(st_lookup_allocator_t){0})!=ST_LOOKUP_FOUND)return 11;uint32_t ids[5]={"
        "2,3,4,5,6};st_aot_thread_t t={0};st_control_thread_t c={0};if(st_control_thread_init(&c,&t,"
        "(st_control_allocator_t){0})!=ST_CONTROL_OK)return 12;if(!st_aot_thread_init(&t,&l,ids,0,&c,0,0,0,0,"
        "0))return 12;uint64_t bm=31;st_root_map_t rm[2]={{1,5,1,&bm},{2,5,1,&bm}};StMethodDescriptor md={"
        "ST_METHOD_ABI_VERSION,77,5,0,5,ST_METHOD_CAN_UNWIND,1,0,0,0,0,rm,2,0,0};uint64_t roots[5]={0};StFrame "
        "nf={.thread=&t,.method=&md,.receiver=si(7),.roots=roots,.root_count=5};"
        "if(st_SmallInteger_negated_primitive(&nf)!=si(-7))return 13;nf.receiver=si(-1152921504606846976LL);"
        "if(st_SmallInteger_negated_primitive(&nf)!=si(123))return 14;nf.receiver=st_value_true();"
        "if(st_SmallInteger_negated_primitive(&nf)!=si(123))return 15;st_aot_thread_destroy(&t);"
        "if(st_control_thread_destroy(&c)!=ST_CONTROL_OK)return 18;st_lookup_context_destroy(&l);"
        "pid_t p=fork();if(p<0)return 16;if(p==0){StFrame bad={.receiver=st_value_true()};"
        "(void)st_Character_value_primitive(&bad);_exit(91);}int s=0;if(waitpid(p,&s,"
        "0)!=p||!WIFSIGNALED(s)||WTERMSIG(s)!=SIGABRT)return 17;return 0;}\n";
    bool ok = true;
    CHECK(methods[4].result.required_root_capacity == 5u);
    CHECK(methods[4].result.root_map_count == 2u);
    CHECK(methods[4].result.root_maps != NULL
          && methods[4].result.root_maps[0].live_root_bitmap[0]
             == UINT64_C(31));
    for (size_t index = 0u; index < 5u; index++) {
        snprintf(paths[index], sizeof(paths[index]),
                 "/tmp/anvil-st-primitive-%ld-%zu.s", pid, index);
        ok = write_text(paths[index], methods[index].assembly) && ok;
    }
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-st-primitive-%ld.c", pid);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-st-primitive-%ld", pid);
    ok = write_text(harness_path, harness) && ok;
    CHECK(ok);
    if (ok) {
        snprintf(command, sizeof(command),
            "/usr/bin/cc -std=c11 -no-pie -Iinclude -Isamples/smalltalk/include "
            "%s %s %s %s %s %s samples/smalltalk/src/runtime/value.c "
            "samples/smalltalk/src/runtime/primitives/core_primitives.c "
            "samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/runtime/lookup.c "
            "samples/smalltalk/src/runtime/send_bridge.c "
            "samples/smalltalk/src/runtime/primitives/primitive_bridge.c "
            "samples/smalltalk/src/runtime/control/control.c "
            "samples/smalltalk/src/runtime/control/control_bridge.c -o %s -pthread",
            paths[0], paths[1], paths[2], paths[3], paths[4], harness_path,
            executable_path);
        CHECK(system(command) == 0);
        int execution_status = system(executable_path);
        if (execution_status != 0)
            fprintf(stderr, "primitive executable status=%d exit=%d\n",
                    execution_status,
                    WIFEXITED(execution_status)
                        ? WEXITSTATUS(execution_status) : -1);
        CHECK(execution_status == 0);
    }
    for (size_t index = 0u; index < 5u; index++) unlink(paths[index]);
    unlink(harness_path);
    unlink(executable_path);
#else
    (void)methods;
#endif
}

static void execute_x86_block_primitives(const compiled_t methods[3])
{
#if defined(__x86_64__) && !defined(_WIN32)
    long pid = (long)getpid();
    char assembly_paths[3][160];
    char executable_path[160];
    char command[4096];
    bool ok = true;
    for (size_t index = 0u; index < 3u; ++index) {
        snprintf(
            assembly_paths[index], sizeof(assembly_paths[index]),
            "/tmp/anvil-st-block-primitive-%ld-%zu.s", pid, index);
        ok = write_text(assembly_paths[index], methods[index].assembly) && ok;
    }
    snprintf(
        executable_path, sizeof(executable_path),
        "/tmp/anvil-st-block-primitive-%ld", pid);
    CHECK(ok);
    if (ok) {
        snprintf(
            command, sizeof(command),
            "/usr/bin/cc -std=c11 -no-pie -Iinclude "
            "-Isamples/smalltalk/include %s %s %s "
            "samples/smalltalk/tests/block_primitives_aot_harness.c "
            "samples/smalltalk/src/compiler/primitive.c "
            "samples/smalltalk/src/runtime/value.c "
            "samples/smalltalk/src/runtime/runtime.c "
            "samples/smalltalk/src/runtime/lookup.c "
            "samples/smalltalk/src/runtime/send_bridge.c "
            "samples/smalltalk/src/runtime/control/control.c "
            "samples/smalltalk/src/runtime/control/control_roots.c "
            "samples/smalltalk/src/runtime/control/control_bridge.c "
            "samples/smalltalk/src/runtime/heap.c "
            "samples/smalltalk/src/runtime/closure_bridge.c "
            "samples/smalltalk/src/runtime/primitives/block_primitives.c "
            "samples/smalltalk/src/runtime/primitives/"
            "block_primitive_bridge.c -o %s -pthread",
            assembly_paths[0], assembly_paths[1], assembly_paths[2],
            executable_path);
        CHECK(system(command) == 0);
        int execution_status = system(executable_path);
        if (execution_status != 0) {
            fprintf(
                stderr, "block primitive AOT status=%d exit=%d\n",
                execution_status,
                WIFEXITED(execution_status)
                    ? WEXITSTATUS(execution_status) : -1);
        }
        CHECK(execution_status == 0);
    }
    for (size_t index = 0u; index < 3u; ++index)
        unlink(assembly_paths[index]);
    unlink(executable_path);
#else
    (void)methods;
#endif
}

static void execute_x86_heap_primitives(const compiled_t methods[11])
{
#if defined(__x86_64__) && !defined(_WIN32)
    long pid = (long)getpid();
    char paths[11][160], harness_path[160], executable_path[160];
    char command[3000];
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
    const char *harness =
        "#include \"st_heap_primitive_bridge.h\"\n#include <signal.h>\n#include <stdint.h>\n#include "
        "<stdlib.h>\n#include <string.h>\n#include <sys/wait.h>\n#include <unistd.h>\n"
        "typedef uint64_t(*fn)(StFrame*);extern uint64_t st_Probe_heapSize(StFrame*),st_Probe_heapAt(StFrame*),"
        "st_Probe_heapAtPut(StFrame*),st_Probe_heapIvarAt(StFrame*),st_Probe_heapIvarAtPut(StFrame*),"
        "st_Probe_heapNew(StFrame*),st_Probe_heapNewSize(StFrame*),st_Probe_heapClass(StFrame*),"
        "st_Probe_heapHash(StFrame*),st_Probe_heapStringEquals(StFrame*),st_String_hash_primitive(StFrame*);"
        "static st_aot_thread_t*t;static uint64_t si(int64_t n){uint64_t v=0;if(!st_value_from_small_integer(n,&v))_exit(90);return v;}"
        "static uint64_t call(fn f,uint32_t n,uint64_t r,uint64_t*a){uint64_t bits=(UINT64_C(1)<<(n+1))-1,"
        "roots[3]={0};st_root_map_t m={1,n+1,1,&bits};StMethodDescriptor d={ST_METHOD_ABI_VERSION,99,1,n,n+1,0,1,"
        "0,0,0,0,&m,1,0,0};StFrame x={.thread=t,.method=&d,.receiver=r,.argv=a,.roots=roots,.argc=n,"
        ".root_count=n+1};return f(&x);}"
        "typedef struct{int fail;}AF;static void*ha(void*u,size_t a,size_t s){AF*f=u;if(f->fail)return 0;return "
        "aligned_alloc(a,s);}static void hf(void*u,void*p,size_t a,size_t s){(void)u;(void)a;(void)s;free(p);}"
        "int main(void){enum{O=1,A=2,M=3,I=4,C=5,N=6,F=7,T=8,B=9,W=10,Z=10};const char*n[Z]={\"Object\",\"Array\","
        "\"Metaclass\",\"SmallInteger\",\"Character\",\"Nil\",\"False\",\"True\",\"NarrowString\",\"WideString\"};"
        "uint64_t ob=1,ab=5;StClassDescriptor cs[Z];StShapeDescriptor ss[Z];const StClassDescriptor*cp[Z];const "
        "StShapeDescriptor*sp[Z];st_heap_indexed_access_t ia[Z]={0};for(uint32_t i=0;i<Z;i++){"
        "cs[i]=(StClassDescriptor){i+1,(i==0||i==2)?0:1,3,i+1,i==2?ST_CLASS_METACLASS:0,n[i],strlen(n[i]),0,0};"
        "ss[i]=(StShapeDescriptor){i+1,i+1,8,24,0,ST_INDEXED_NONE,0,0};cp[i]=&cs[i];sp[i]=&ss[i];}"
        "ss[O-1].minimum_allocation_size=32;ss[O-1].fixed_word_count=1;ss[O-1].fixed_pointer_bitmap=&ob;"
        "ss[O-1].fixed_pointer_bitmap_word_count=1;ss[A-1].minimum_allocation_size=48;ss[A-1].fixed_word_count=3;"
        "ss[A-1].indexed_format=ST_INDEXED_VALUES;ss[A-1].fixed_pointer_bitmap=&ab;"
        "ss[A-1].fixed_pointer_bitmap_word_count=1;ia[A-1]=ST_HEAP_INDEXED_ACCESS_VALUES;"
        "ss[B-1].indexed_format=ST_INDEXED_UINT8;ss[W-1].indexed_format=ST_INDEXED_UINT32;"
        "ia[B-1]=ia[W-1]=ST_HEAP_INDEXED_ACCESS_CHARACTER;st_runtime_descriptors_t d={cp,Z,sp,Z};"
        "if(st_runtime_descriptors_validate(&d)!=ST_RUNTIME_OK)return 1;AF af={0};st_heap_t h={0};"
        "if(st_heap_init(&h,&d,(st_runtime_allocator_t){ha,hf,&af})!=ST_HEAP_OK)return 2;uint64_t co[Z];"
        "for(uint32_t i=0;i<Z;i++)if(st_heap_allocate(&h,M,M,0,0,ST_HEADER_PINNED,&co[i])!=ST_HEAP_OK)return 3;"
        "st_heap_primitive_options_t o={.heap=&h,.immediate_classes={I,C,N,F,T},.class_objects=co,"
        ".class_object_count=Z,.indexed_access=ia,.indexed_access_count=Z};st_heap_primitive_context_t pc={0};"
        "if(st_heap_primitive_context_init(&pc,&o)!=ST_HEAP_PRIMITIVE_OK)return 4;st_lookup_context_t l={0};"
        "if(st_lookup_context_init(&l,&d,(st_lookup_allocator_t){0})!=ST_LOOKUP_FOUND)return 5;uint32_t ids[5]={N,"
        "F,T,I,C};st_aot_thread_t th={0};if(!st_aot_thread_init(&th,&l,ids,&pc,0,0,0,0,0,0))return 6;t=&th;"
        "uint64_t arg[2];uint64_t child=call(st_Probe_heapNew,0,co[O-1],0);if(!st_heap_contains(&h,child))return "
        "7;arg[0]=si(1);uint64_t array=call(st_Probe_heapNewSize,1,co[A-1],arg);if(!st_heap_contains(&h,"
        "array))return 8;st_object_view_t v={0};if(st_heap_object_view(&h,array,&v)!=ST_HEAP_OK)return 9;"
        "st_gc_generation_t g;uint8_t age;if(!st_object_header_survive(&v.object->header,1,&g,"
        "&age)||!st_object_header_survive(&v.object->header,1,&g,&age))return 10;arg[0]=si(1);arg[1]=child;"
        "if(call(st_Probe_heapAtPut,2,array,arg)!=child)return 11;"
        "if(!(st_object_header_flags(st_object_header_load(&v.object->header))&ST_HEADER_REMEMBERED))return 12;"
        "size_t cn=0;const uint64_t*cr=st_heap_primitive_class_roots(&pc,&cn);uint64_t*gr=malloc((cn+1)*8);"
        "memcpy(gr,cr,cn*8);gr[cn]=array;st_heap_collection_stats_t gs;st_heap_status_t hs=st_heap_collect(&h,0,"
        "gr,cn+1,&gs);if(hs!=ST_HEAP_OK)return 30+hs;free(gr);arg[0]=si(1);if(call(st_Probe_heapAt,1,array,"
        "arg)!=child)return 14;if(call(st_Probe_heapSize,0,array,0)!=si(1))return 15;if(call(st_Probe_heapClass,0,"
        "child,0)!=co[O-1])return 16;arg[0]=si(1);arg[1]=st_value_true();if(call(st_Probe_heapIvarAtPut,2,child,"
        "arg)!=st_value_true())return 17;if(call(st_Probe_heapIvarAt,1,child,arg)!=st_value_true())return 18;"
        "uint64_t oh=call(st_Probe_heapHash,0,child,0);if(oh!=call(st_Probe_heapHash,0,child,0))return 40;"
        "arg[0]=si(2);uint64_t bs=call(st_Probe_heapNewSize,1,co[B-1],arg),ws=call(st_Probe_heapNewSize,1,co[W-1],"
        "arg);for(uint32_t j=1;j<=2;j++){arg[0]=si(j);arg[1]=((uint64_t)(64+j)<<3)|2;if(call(st_Probe_heapAtPut,2,"
        "bs,arg)!=arg[1]||call(st_Probe_heapAtPut,2,ws,arg)!=arg[1])return 41;}arg[0]=ws;"
        "if(call(st_Probe_heapStringEquals,1,bs,arg)!=st_value_true())return 42;uint64_t "
        "bh=call(st_String_hash_primitive,0,bs,0),wh=call(st_String_hash_primitive,0,ws,0);if(bh!=wh)return 43;"
        "arg[0]=si(2);arg[1]=((uint64_t)'Z'<<3)|2;if(call(st_Probe_heapAtPut,2,ws,arg)!=arg[1])return 44;"
        "arg[0]=ws;if(call(st_Probe_heapStringEquals,1,bs,arg)!=st_value_false()||call(st_String_hash_primitive,0,"
        "ws,0)==bh)return 45;arg[0]=UINT64_C(0x1000);if(call(st_Probe_heapStringEquals,1,bs,"
        "arg)!=st_value_false())return 46;st_aot_thread_t noheap={0};if(!st_aot_thread_init(&noheap,&l,ids,0,0,0,"
        "0,0,0,0))return 19;t=&noheap;arg[0]=si(1);if(call(st_Probe_heapAt,1,array,arg)!=si(81))return 20;"
        "st_aot_thread_destroy(&noheap);t=&th;af.fail=1;if(call(st_Probe_heapNewSize,1,co[A-1],"
        "arg)!=si(79))return 21;pid_t p=fork();if(p<0)return 22;if(p==0){(void)call(st_Probe_heapNew,0,co[O-1],0);"
        "_exit(91);}int s=0;if(waitpid(p,&s,0)!=p||!WIFSIGNALED(s)||WTERMSIG(s)!=SIGABRT)return 23;af.fail=0;"
        "st_aot_thread_destroy(&th);st_lookup_context_destroy(&l);st_heap_primitive_context_destroy(&pc);"
        "st_heap_destroy(&h);return 0;}";
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    bool ok = true;
    for (size_t i = 0u; i < 11u; i++) {
        snprintf(paths[i], sizeof(paths[i]),
                 "/tmp/anvil-st-heap-%ld-%zu.s", pid, i);
        ok = write_text(paths[i], methods[i].assembly) && ok;
    }
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-st-heap-%ld.c", pid);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-st-heap-%ld", pid);
    ok = write_text(harness_path, harness) && ok;
    CHECK(ok);
    if (ok) {
        int n = snprintf(command, sizeof(command),
            "/usr/bin/cc -std=c11 -no-pie -Iinclude "
            "-Isamples/smalltalk/include ");
        for (size_t i = 0u;
             i < 11u && n > 0 && (size_t)n < sizeof(command); i++) {
            n += snprintf(command + n, sizeof(command) - (size_t)n,
                          "%s ", paths[i]);
        }
        if (n > 0 && (size_t)n < sizeof(command)) {
            snprintf(command + n, sizeof(command) - (size_t)n,
                "%s samples/smalltalk/src/runtime/value.c "
                "samples/smalltalk/src/runtime/runtime.c "
                "samples/smalltalk/src/runtime/heap.c "
                "samples/smalltalk/src/runtime/control/control.c "
                "samples/smalltalk/src/runtime/control/control_roots.c "
                "samples/smalltalk/src/compiler/primitive.c "
                "samples/smalltalk/src/runtime/primitives/core_primitives.c "
                "samples/smalltalk/src/runtime/primitives/heap_primitives.c "
                "samples/smalltalk/src/runtime/lookup.c "
                "samples/smalltalk/src/runtime/send_bridge.c "
                "samples/smalltalk/src/runtime/primitives/"
                "heap_primitive_bridge.c -o %s -pthread",
                harness_path, executable_path);
        }
        CHECK(n > 0 && (size_t)n < sizeof(command)
              && system(command) == 0);
        int run_status = system(executable_path);
        if (run_status != 0)
            fprintf(stderr, "heap harness status=%d\n", run_status);
        CHECK(run_status == 0);
    }
    for (size_t i = 0u; i < 11u; i++) unlink(paths[i]);
    unlink(harness_path);
    unlink(executable_path);
#else
    (void)methods;
#endif
}

static void execute_x86_instance_variables(const compiled_t methods[3])
{
#if defined(__x86_64__) && !defined(_WIN32)
    long pid = (long)getpid();
    char paths[3][160];
    char harness_path[160];
    char executable_path[160];
    char command[2600];
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
    const char *harness =
        "#include \"st_heap_primitive_bridge.h\"\n"
        "#include <signal.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <sys/wait.h>\n#include <unistd.h>\n"
        "typedef uint64_t(*fn)(StFrame*);"
        "extern uint64_t st_Probe_ivarRead(StFrame*),st_Probe_ivarWrite(StFrame*),"
        "st_Probe_ivarSecond(StFrame*);static st_aot_thread_t*t;"
        "static uint64_t call(fn f,uint32_t n,uint64_t r,uint64_t*a){"
        "StMethodDescriptor d={.abi_version=ST_METHOD_ABI_VERSION,.selector_id=99,.owner_class_id=1,"
        ".arity=n,.frame_root_capacity=0,.code_size=1};StFrame x={.thread=t,.method=&d,.receiver=r,"
        ".argv=a,.argc=n};return f(&x);}"
        "int main(void){enum{O=1,M=2,I=3,C=4,N=5,F=6,T=7,Z=7};"
        "const char*n[Z]={\"Object\",\"Metaclass\",\"SmallInteger\",\"Character\",\"Nil\",\"False\",\"True\"};"
        "uint64_t object_bitmap=3;StClassDescriptor cs[Z];StShapeDescriptor ss[Z];"
        "const StClassDescriptor*cp[Z];const StShapeDescriptor*sp[Z];st_heap_indexed_access_t ia[Z]={0};"
        "for(uint32_t i=0;i<Z;i++){cs[i]=(StClassDescriptor){i+1,(i==0||i==1)?0:1,2,i+1,"
        "i==1?ST_CLASS_METACLASS:0,n[i],strlen(n[i]),0,0};ss[i]=(StShapeDescriptor){i+1,i+1,8,24,0,"
        "ST_INDEXED_NONE,0,0};cp[i]=&cs[i];sp[i]=&ss[i];}ss[O-1].minimum_allocation_size=40;"
        "ss[O-1].fixed_word_count=2;ss[O-1].fixed_pointer_bitmap=&object_bitmap;"
        "ss[O-1].fixed_pointer_bitmap_word_count=1;st_runtime_descriptors_t d={cp,Z,sp,Z};"
        "if(st_runtime_descriptors_validate(&d)!=ST_RUNTIME_OK)return 1;st_heap_t h={0};"
        "if(st_heap_init(&h,&d,(st_runtime_allocator_t){0})!=ST_HEAP_OK)return 2;uint64_t co[Z];"
        "for(uint32_t i=0;i<Z;i++)if(st_heap_allocate(&h,M,M,0,0,ST_HEADER_PINNED,&co[i])!=ST_HEAP_OK)return 3;"
        "st_heap_primitive_context_t pc={0};st_heap_primitive_options_t po={.heap=&h,"
        ".immediate_classes={I,C,N,F,T},.class_objects=co,.class_object_count=Z,.indexed_access=ia,"
        ".indexed_access_count=Z};if(st_heap_primitive_context_init(&pc,&po)!=ST_HEAP_PRIMITIVE_OK)return 4;"
        "st_lookup_context_t l={0};if(st_lookup_context_init(&l,&d,(st_lookup_allocator_t){0})"
        "!=ST_LOOKUP_FOUND)return 5;uint32_t ids[5]={N,F,T,I,C};st_aot_thread_t th={0};"
        "if(!st_aot_thread_init(&th,&l,ids,&pc,0,0,0,0,0,0))return 6;t=&th;uint64_t receiver,child;"
        "if(st_heap_allocate(&h,O,O,0,0,0,&receiver)!=ST_HEAP_OK||st_heap_allocate(&h,O,O,0,0,0,&child)"
        "!=ST_HEAP_OK)return 7;st_object_view_t v;if(st_heap_object_view(&h,receiver,&v)!=ST_HEAP_OK)return 8;"
        "st_gc_generation_t g;uint8_t age;if(!st_object_header_survive(&v.object->header,1,&g,&age)"
        "||!st_object_header_survive(&v.object->header,1,&g,&age))return 9;uint64_t a[1]={child};"
        "if(call(st_Probe_ivarWrite,1,receiver,a)!=child||call(st_Probe_ivarRead,0,receiver,0)!=child)return 10;"
        "if(!(st_object_header_flags(st_object_header_load(&v.object->header))&ST_HEADER_REMEMBERED))return 11;"
        "a[0]=st_value_true();if(call(st_Probe_ivarSecond,1,receiver,a)!=st_value_true())return 12;"
        "st_value_t loaded=ST_VALUE_INVALID;if(st_heap_fixed_reference_load(&h,receiver,1,&loaded)!=ST_HEAP_OK"
        "||loaded!=st_value_true())return 13;pid_t p=fork();if(p<0)return 14;if(p==0){"
        "(void)call(st_Probe_ivarRead,0,st_value_true(),0);_exit(91);}int s=0;if(waitpid(p,&s,0)!=p"
        "||!WIFSIGNALED(s)||WTERMSIG(s)!=SIGABRT)return 15;st_aot_thread_destroy(&th);"
        "st_lookup_context_destroy(&l);st_heap_primitive_context_destroy(&pc);st_heap_destroy(&h);return 0;}";
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    bool ok = true;
    for (size_t index = 0u; index < 3u; index++) {
        snprintf(paths[index], sizeof(paths[index]),
                 "/tmp/anvil-st-ivar-%ld-%zu.s", pid, index);
        ok = write_text(paths[index], methods[index].assembly) && ok;
    }
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-st-ivar-%ld.c", pid);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-st-ivar-%ld", pid);
    ok = write_text(harness_path, harness) && ok;
    CHECK(ok);
    int length = snprintf(
        command, sizeof(command),
        "/usr/bin/cc -std=c11 -no-pie -Iinclude "
        "-Isamples/smalltalk/include %s %s %s %s "
        "samples/smalltalk/src/runtime/value.c "
        "samples/smalltalk/src/runtime/runtime.c "
        "samples/smalltalk/src/runtime/heap.c "
        "samples/smalltalk/src/runtime/control/control.c "
        "samples/smalltalk/src/runtime/control/control_roots.c "
        "samples/smalltalk/src/runtime/primitives/heap_primitives.c "
        "samples/smalltalk/src/runtime/lookup.c "
        "samples/smalltalk/src/runtime/send_bridge.c "
        "samples/smalltalk/src/runtime/primitives/heap_primitive_bridge.c "
        "-o %s -pthread",
        paths[0], paths[1], paths[2], harness_path, executable_path);
    CHECK(length > 0 && (size_t)length < sizeof(command));
    if (ok && length > 0 && (size_t)length < sizeof(command)) {
        CHECK(system(command) == 0);
        int status = system(executable_path);
        if (status != 0) {
            fprintf(stderr, "instance-variable harness status=%d\n", status);
        }
        CHECK(status == 0);
    }
    for (size_t index = 0u; index < 3u; index++) {
        unlink(paths[index]);
    }
    unlink(harness_path);
    unlink(executable_path);
#else
    (void)methods;
#endif
}

static bool cross_assemble(const char *assembly)
{
    if (access("/usr/bin/clang", X_OK) != 0) return true;
    char source[] = "/tmp/anvil-st-lower-arm64-XXXXXX.s";
    int fd = mkstemps(source, 2);
    if (fd < 0) return false;
    FILE *file = fdopen(fd, "wb");
    bool ok = file && fputs(assembly, file) >= 0;
    if (file) ok = fclose(file) == 0 && ok;
    else close(fd);
    char object[192], command[640];
    snprintf(object, sizeof(object), "%s.o", source);
    snprintf(command, sizeof(command),
             "/usr/bin/clang --target=aarch64-linux-gnu -c %s -o %s",
             source, object);
    if (ok) ok = system(command) == 0;
    unlink(source);
    unlink(object);
    return ok;
}

static bool native_assemble(const char *assembly)
{
#if defined(__x86_64__) && !defined(_WIN32)
    char source[] = "/tmp/anvil-st-lower-x86-64-XXXXXX.s";
    int fd = mkstemps(source, 2);
    if (fd < 0) return false;

    FILE *file = fdopen(fd, "wb");
    bool ok = file != NULL && fputs(assembly, file) >= 0;
    if (file != NULL) {
        ok = fclose(file) == 0 && ok;
    } else {
        close(fd);
    }

    char object[192];
    char command[640];
    snprintf(object, sizeof(object), "%s.o", source);
    int length = snprintf(command, sizeof(command),
                          "/usr/bin/cc -c %s -o %s", source, object);
    if (ok) {
        ok = length > 0 && (size_t)length < sizeof(command)
          && system(command) == 0;
    }
    unlink(source);
    unlink(object);
    return ok;
#else
    (void)assembly;
    return true;
#endif
}

static void execute_x86_image_and_stream_lowering(
    const compiled_t *global_code, const compiled_t *literal_code,
    const compiled_t *stream_code)
{
#if defined(__x86_64__) && !defined(_WIN32)
    static const char harness[] =
        "#include \"st_image_runtime.h\"\n"
        "#include \"st_lookup.h\"\n"
        "#include \"st_send_bridge.h\"\n"
        "#include \"st_stream_primitive_bridge.h\"\n"
        "#include <stdint.h>\n#include <string.h>\n#include <unistd.h>\n"
        "extern uint64_t st_Probe_globalRead(StFrame*);\n"
        "extern uint64_t st_Probe_twoStrings(StFrame*);\n"
        "extern uint64_t st_Probe_streamWrite(StFrame*);\n"
        "static uint64_t si(int64_t x){uint64_t v=0;if(!st_value_from_small_integer(x,&v))_exit(90);return v;}\n"
        "int main(void){enum{N=8,MC=7,S=8};"
        "const char*n[N]={\"Nil\",\"False\",\"True\",\"SmallInteger\",\"Character\",\"Object\",\"Metaclass\",\"String\"};"
        "StClassDescriptor c[N];StShapeDescriptor s[N];const StClassDescriptor*cp[N];const StShapeDescriptor*sp[N];"
        "for(uint32_t i=0;i<N;i++){uint32_t id=i+1;c[i]=(StClassDescriptor){.class_id=id,"
        ".superclass_id=(id==6||id==MC)?0:6,.metaclass_id=MC,.default_shape_id=id,"
        ".flags=id==MC?ST_CLASS_METACLASS:0,.name=n[i],.name_length=strlen(n[i])};s[i]=(StShapeDescriptor){"
        ".shape_id=id,.class_id=id,.allocation_alignment=8,.minimum_allocation_size=24,"
        ".indexed_format=id==S?ST_INDEXED_UINT8:ST_INDEXED_NONE};cp[i]=&c[i];sp[i]=&s[i];}"
        "st_runtime_descriptors_t d={cp,N,sp,N};if(st_runtime_descriptors_validate(&d)!=ST_RUNTIME_OK)return 1;"
        "st_heap_t heap={0};if(st_heap_init(&heap,&d,(st_runtime_allocator_t){0})!=ST_HEAP_OK)return 2;"
        "st_image_runtime_entry_t ge[8],le[2];for(uint32_t i=0;i<8;i++)ge[i]=(st_image_runtime_entry_t){i+1,"
        "ST_VALUE_INVALID};for(uint32_t i=0;i<2;i++)le[i]=(st_image_runtime_entry_t){i+1,ST_VALUE_INVALID};"
        "ge[7].value=st_value_true();"
        "st_image_runtime_t image={0};st_image_runtime_options_t io={.descriptors=&d,.borrowed_heap=&heap,"
        ".globals=ge,.global_count=8,.literals=le,.literal_count=2,.string_layout={S,S}};"
        "if(st_image_runtime_init(&image,&io)!=ST_IMAGE_RUNTIME_OK)return 3;"
        "uint64_t first=0,second=0;if(st_image_runtime_bootstrap_string_literal(&image,0,\"first\",5,"
        "&first)!=ST_IMAGE_RUNTIME_OK||st_image_runtime_bootstrap_string_literal(&image,1,\"second\",6,"
        "&second)!=ST_IMAGE_RUNTIME_OK)return 4;"
        "st_lookup_context_t lookup={0};if(st_lookup_context_init(&lookup,&d,(st_lookup_allocator_t){0}"
        ")!=ST_LOOKUP_FOUND)return 5;uint32_t ids[5]={1,2,3,4,5};st_aot_thread_t thread={0};"
        "if(!st_aot_thread_init(&thread,&lookup,ids,0,0,0,0,0,0,0)||!st_aot_thread_image_attach(&thread,"
        "&image))return 6;"
        "StMethodDescriptor gmd={.abi_version=ST_METHOD_ABI_VERSION,.selector_id=1,.owner_class_id=6,.arity=0};"
        "StFrame gf={.thread=&thread,.method=&gmd,.receiver=st_value_true()};"
        "if(st_Probe_globalRead(&gf)!=st_value_true())return 7;"
        "StMethodDescriptor lmd={.abi_version=ST_METHOD_ABI_VERSION,.selector_id=2,.owner_class_id=6,.arity=0};"
        "StFrame lf={.thread=&thread,.method=&lmd,.receiver=st_value_true()};uint64_t lv=st_Probe_twoStrings(&lf);"
        "if(lv!=second)return 8;st_object_view_t view={0};if(st_heap_object_view(&heap,lv,"
        "&view)!=ST_HEAP_OK||view.indexed_length!=6||memcmp(view.indexed_elements,\"second\",6))return 9;"
        "st_stream_primitive_context_t streams={0};if(st_stream_primitive_context_init(&streams,"
        "&(st_stream_primitive_options_t){.heap=&heap,.string_class_id=S,.string_shape_id=S}"
        ")!=ST_STREAM_PRIMITIVE_OK||!st_aot_thread_streams_attach(&thread,&streams))return 10;"
        "uint64_t bm=15,args[3],roots[4]={st_value_true(),0,0,0};st_root_map_t rm={1,4,1,&bm};StMethodDescriptor "
        "smd={.abi_version=ST_METHOD_ABI_VERSION,.selector_id=3,.owner_class_id=MC,.arity=3,"
        ".frame_root_capacity=4,.root_maps=&rm,.root_map_count=1};int p[2];if(pipe(p))return 11;args[0]=si(p[1]);"
        "args[1]=si(6);args[2]=second;StFrame sf={.thread=&thread,.method=&smd,.receiver=st_value_true(),"
        ".argv=args,.roots=roots,.argc=3,.root_count=4};if(st_Probe_streamWrite(&sf)!=si(6))return 12;"
        "if(close(p[1]))return 13;char out[6];if(read(p[0],out,6)!=6||memcmp(out,\"second\","
        "6)||close(p[0]))return 14;"
        "if(!st_aot_thread_streams_detach(&thread,&streams))return 15;sf.safepoint_id=0;if(st_Probe_streamWrite(&sf)!=si(77))return 16;"
        "st_stream_primitive_context_destroy(&streams);if(!st_aot_thread_image_detach(&thread,&image))return 17;"
        "st_aot_thread_destroy(&thread);st_lookup_context_destroy(&lookup);st_image_runtime_destroy(&image);"
        "st_heap_destroy(&heap);return 0;}\n";
    pid_t pid = getpid();
    char global_path[160], literal_path[160], stream_path[160];
    char harness_path[160], executable_path[160], command[2048];
    snprintf(global_path, sizeof(global_path),
             "/tmp/anvil-st-image-%ld-global.s", (long)pid);
    snprintf(literal_path, sizeof(literal_path),
             "/tmp/anvil-st-image-%ld-literal.s", (long)pid);
    snprintf(stream_path, sizeof(stream_path),
             "/tmp/anvil-st-image-%ld-stream.s", (long)pid);
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-st-image-%ld.c", (long)pid);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-st-image-%ld", (long)pid);
    bool ok = write_text(global_path, global_code->assembly)
        && write_text(literal_path, literal_code->assembly)
        && write_text(stream_path, stream_code->assembly)
        && write_text(harness_path, harness);
    if (ok) {
        int amount = snprintf(
            command, sizeof(command),
            "/usr/bin/cc -std=c11 -no-pie -Iinclude -Isamples/smalltalk/include %s %s %s %s "
            "samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c "
            "samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/lookup.c "
            "samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c "
            "samples/smalltalk/src/runtime/send_bridge.c samples/smalltalk/src/runtime/image_runtime.c "
            "samples/smalltalk/src/runtime/primitives/stream_primitives.c "
            "samples/smalltalk/src/runtime/primitives/stream_primitive_bridge.c -o %s -pthread",
            global_path, literal_path, stream_path, harness_path,
            executable_path);
        ok = amount > 0 && (size_t)amount < sizeof(command)
          && system(command) == 0;
    }
    int status = ok ? system(executable_path) : -1;
    if (status != 0)
        fprintf(stderr, "image/stream lowering harness status=%d\n", status);
    CHECK(ok && status == 0);
    unlink(global_path);
    unlink(literal_path);
    unlink(stream_path);
    unlink(harness_path);
    unlink(executable_path);
#else
    (void)global_code;
    (void)literal_code;
    (void)stream_code;
#endif
}

static void execute_x86_cascade(const compiled_t *cascade,
                                st_selector_id_t selector_id)
{
#if defined(__x86_64__) && !defined(_WIN32)
    const st_lower_root_map_t *maps = cascade->result.root_maps;
    CHECK(cascade->result.required_root_capacity == 4u
          && cascade->result.root_map_count == 3u && maps != NULL);
    if (maps == NULL) return;
    for (size_t index = 0u; index < 3u; index++)
        CHECK(maps[index].safepoint_id == index + 1u
              && maps[index].root_count == 4u
              && maps[index].bitmap_word_count == 1u
              && maps[index].live_root_bitmap != NULL
              && maps[index].live_root_bitmap[0] == UINT64_C(15));
    long pid = (long)getpid();
    char assembly_path[160], harness_path[160], executable_path[160];
    char command[1400], harness[6000];
    snprintf(assembly_path, sizeof(assembly_path),
             "/tmp/anvil-st-cascade-%ld.s", pid);
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-st-cascade-%ld.c", pid);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-st-cascade-%ld", pid);
    int length = snprintf(harness, sizeof(harness),
        "#include \"st_control_bridge.h\"\n#include <stdint.h>\n#include <string.h>\n"
        "extern uint64_t st_Probe_cascade(StFrame*);\n"
        "static uint64_t leaf(StFrame*f){if(!f||f->receiver!=st_value_true())return 0;return st_value_false();}\n"
        "static uint64_t fail(void*u,StFrame*f,const st_send_site_t*s,uint64_t r,const uint64_t*a,uint32_t n,"
        "st_aot_send_status_t e){(void)u;(void)f;(void)s;(void)r;(void)a;(void)n;(void)e;return st_value_nil();}\n"
        "int main(void){const char*n[7]={\"Object\",\"Nil\",\"False\",\"True\",\"SmallInteger\",\"Character\","
        "\"Class\"};StClassDescriptor c[7];StShapeDescriptor sh[7];const StClassDescriptor*cp[7];const "
        "StShapeDescriptor*sp[7];for(uint32_t i=0;i<7;i++){c[i]=(StClassDescriptor){i+1,(i==0||i==6)?0:1,7,i+1,"
        "i==6?ST_CLASS_METACLASS:0,n[i],strlen(n[i]),0,0};sh[i]=(StShapeDescriptor){i+1,i+1,8,24,0,"
        "ST_INDEXED_NONE,0,0};cp[i]=&c[i];sp[i]=&sh[i];}"
        "StMethodDescriptor ld={.abi_version=ST_METHOD_ABI_VERSION,.selector_id=%u,.owner_class_id=4,.arity=0};"
        "StMethodBinding lb={&ld,leaf,1};StMethodEntry e;if(!st_method_entry_init(&e,&lb))return 1;"
        "st_method_slot_t slot={%u,&e};c[3].method_slots=&slot;c[3].method_slot_count=1;st_runtime_descriptors_t "
        "d={cp,7,sp,7};if(st_runtime_descriptors_validate(&d)!=ST_RUNTIME_OK)return 2;st_lookup_context_t l={0};"
        "if(st_lookup_context_init(&l,&d,(st_lookup_allocator_t){0})!=ST_LOOKUP_FOUND)return 3;uint32_t ids[5]={2,"
        "3,4,5,6};st_aot_thread_t t={0};st_control_thread_t control={0};if(st_control_thread_init(&control,&t,"
        "(st_control_allocator_t){0})!=ST_CONTROL_OK||!st_aot_thread_init(&t,&l,ids,0,&control,0,0,0,fail,"
        "0))return 4;uint64_t bits=15,roots[4]={0};st_root_map_t rm[3]={{1,4,1,&bits},{2,4,1,&bits},{3,4,1,&bits}}"
        ";StMethodDescriptor md={.abi_version=ST_METHOD_ABI_VERSION,.selector_id=88,.owner_class_id=1,.arity=0,"
        ".frame_root_capacity=4,.flags=ST_METHOD_CAN_UNWIND,.root_maps=rm,.root_map_count=3};StFrame f={"
        ".thread=&t,.method=&md,.receiver=st_value_true(),.roots=roots,.root_count=4};uint64_t "
        "r=st_Probe_cascade(&f);if(r!=st_value_false()||f.safepoint_id!=0||roots[0]!=st_value_true()||"
        "roots[1]!=st_value_nil()||roots[2]!=st_value_nil()||roots[3]!=st_value_false())return 5;"
        "st_aot_thread_destroy(&t);"
        "if(st_control_thread_destroy(&control)!=ST_CONTROL_OK)return 6;st_lookup_context_destroy(&l);return 0;}\n",
        selector_id, selector_id);
    bool ok = length > 0 && (size_t)length < sizeof(harness)
        && write_text(assembly_path, cascade->assembly)
        && write_text(harness_path, harness);
    if (ok) {
        int amount = snprintf(command, sizeof(command),
            "/usr/bin/cc -std=c11 -no-pie -Iinclude -Isamples/smalltalk/include %s %s "
            "samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c "
            "samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c "
            "samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_bridge.c -o %s -pthread",
            assembly_path, harness_path, executable_path);
        ok = amount > 0 && (size_t)amount < sizeof(command)
          && system(command) == 0;
    }
    int status = ok ? system(executable_path) : -1;
    if (status != 0) fprintf(stderr, "cascade harness status=%d\n", status);
    CHECK(ok && status == 0);
    unlink(assembly_path);
    unlink(harness_path);
    unlink(executable_path);
#else
    (void)cascade;
    (void)selector_id;
#endif
}

enum { NUMERIC_AOT_METHOD_COUNT = 9 };

static bool write_numeric_aot_harness(
    const char *path,
    const compiled_t methods[NUMERIC_AOT_METHOD_COUNT])
{
    static const char *const symbols[NUMERIC_AOT_METHOD_COUNT] = {
        "st_Probe_numericBinary",
        "st_Probe_numericCompare",
        "st_Probe_numericShift",
        "st_Probe_numericLargeFloat",
        "st_Probe_numericSmallFloat",
        "st_Probe_numericHash",
        "st_Probe_floatAdd",
        "st_Probe_floatRounded",
        "st_Probe_floatHash"
    };
    FILE *file = fopen(path, "wb");
    bool ok;

    if (file == NULL) return false;
    ok = fprintf(
        file,
        "#include \"st_integer_primitives.h\"\n"
        "#include \"st_send_bridge.h\"\n"
        "#include <stdint.h>\n"
        "#include <string.h>\n") > 0;
    for (size_t index = 0u;
         ok && index < NUMERIC_AOT_METHOD_COUNT; index++) {
        ok = fprintf(
            file, "extern uint64_t %s(StFrame *frame);\n",
            symbols[index]) > 0;
    }

    for (size_t method_index = 0u;
         ok && method_index < NUMERIC_AOT_METHOD_COUNT; method_index++) {
        const st_lower_result_t *result = &methods[method_index].result;
        for (size_t map_index = 0u;
             ok && map_index < result->root_map_count; map_index++) {
            const st_lower_root_map_t *map = &result->root_maps[map_index];
            ok = fprintf(
                file,
                "static const uint64_t numeric_bits_%zu_%zu[%zu] = {",
                method_index, map_index, map->bitmap_word_count) > 0;
            for (size_t word = 0u;
                 ok && word < map->bitmap_word_count; word++) {
                ok = fprintf(
                    file, "%sUINT64_C(0x%llx)", word == 0u ? "" : ", ",
                    (unsigned long long)map->live_root_bitmap[word]) > 0;
            }
            ok = ok && fprintf(file, "};\n") > 0;
        }
        ok = ok && fprintf(
            file,
            "static const st_root_map_t numeric_maps_%zu[%zu] = {\n",
            method_index, result->root_map_count) > 0;
        for (size_t map_index = 0u;
             ok && map_index < result->root_map_count; map_index++) {
            const st_lower_root_map_t *map = &result->root_maps[map_index];
            ok = fprintf(
                file,
                "    {%u, %u, %zu, numeric_bits_%zu_%zu},\n",
                map->safepoint_id, map->root_count,
                map->bitmap_word_count, method_index, map_index) > 0;
        }
        ok = ok && fprintf(
            file,
            "};\n"
            "static const StMethodDescriptor numeric_method_%zu = {\n"
            "    .abi_version = ST_METHOD_ABI_VERSION,\n"
            "    .selector_id = %zu,\n"
            "    .owner_class_id = 1,\n"
            "    .arity = %u,\n"
            "    .frame_root_capacity = %u,\n"
            "    .flags = %u,\n"
            "    .code_size = 1,\n"
            "    .root_maps = numeric_maps_%zu,\n"
            "    .root_map_count = %zu\n"
            "};\n",
            method_index, 500u + method_index,
            method_index == 0u ? 2u
                : method_index < 3u || method_index == 6u ? 1u : 0u,
            result->required_root_capacity, result->method_flags,
            method_index, result->root_map_count) > 0;
    }

    ok = ok && fprintf(
        file,
        "static uint64_t small_integer(int64_t integer)\n"
        "{\n"
        "    uint64_t value = 0;\n"
        "    if (!st_value_from_small_integer(integer, &value)) return 0;\n"
        "    return value;\n"
        "}\n"
        "\n"
        "typedef uint64_t (*numeric_code_t)(StFrame *frame);\n"
        "\n"
        "static uint64_t run_numeric(\n"
        "    numeric_code_t code, const StMethodDescriptor *method,\n"
        "    st_aot_thread_t *thread, uint64_t receiver,\n"
        "    uint64_t *arguments, uint32_t argument_count)\n"
        "{\n"
        "    uint64_t roots[32];\n"
        "    if (method->frame_root_capacity > 32) return 0;\n"
        "    for (uint32_t index = 0; index < method->frame_root_capacity; index++)\n"
        "        roots[index] = st_value_nil();\n"
        "    StFrame frame = {\n"
        "        .thread = thread,\n"
        "        .method = method,\n"
        "        .receiver = receiver,\n"
        "        .argv = arguments,\n"
        "        .roots = method->frame_root_capacity != 0 ? roots : NULL,\n"
        "        .argc = argument_count,\n"
        "        .root_count = method->frame_root_capacity\n"
        "    };\n"
        "    return code(&frame);\n"
        "}\n") > 0;

    ok = ok && fprintf(
        file,
        "int main(void)\n"
        "{\n"
        "    enum {\n"
        "        OBJECT = 1, NIL, FALSE_VALUE, TRUE_VALUE, SMALL_INTEGER,\n"
        "        CHARACTER, NUMBER, FLOAT_VALUE, BOXED_FLOAT64, INTEGER,\n"
        "        LARGE_POSITIVE, LARGE_NEGATIVE, METACLASS, CLASS_COUNT\n"
        "    };\n"
        "    const char *names[CLASS_COUNT - 1] = {\n"
        "        \"Object\", \"Nil\", \"False\", \"True\", \"SmallInteger\",\n"
        "        \"Character\", \"Number\", \"Float\", \"BoxedFloat64\",\n"
        "        \"Integer\", \"LargePositiveInteger\",\n"
        "        \"LargeNegativeInteger\", \"Metaclass\"\n"
        "    };\n"
        "    uint32_t supers[CLASS_COUNT - 1] = {\n"
        "        0, OBJECT, OBJECT, OBJECT, INTEGER, OBJECT, OBJECT, NUMBER,\n"
        "        FLOAT_VALUE, NUMBER, INTEGER, INTEGER, 0\n"
        "    };\n"
        "    uint64_t raw_bitmap = 0;\n"
        "    StClassDescriptor classes[CLASS_COUNT - 1];\n"
        "    StShapeDescriptor shapes[CLASS_COUNT - 1];\n"
        "    const StClassDescriptor *class_pointers[CLASS_COUNT - 1];\n"
        "    const StShapeDescriptor *shape_pointers[CLASS_COUNT - 1];\n"
        "    for (uint32_t id = 1; id < CLASS_COUNT; id++) {\n"
        "        size_t index = id - 1;\n"
        "        classes[index] = (StClassDescriptor) {\n"
        "            id, supers[index], METACLASS, id,\n"
        "            id == METACLASS ? ST_CLASS_METACLASS : 0,\n"
        "            names[index], strlen(names[index]), NULL, 0\n"
        "        };\n"
        "        shapes[index] = (StShapeDescriptor) {\n"
        "            id, id, 8, 24, 0, ST_INDEXED_NONE, NULL, 0\n"
        "        };\n"
        "        class_pointers[index] = &classes[index];\n"
        "        shape_pointers[index] = &shapes[index];\n"
        "    }\n"
        "    uint32_t raw_classes[3] = {BOXED_FLOAT64, LARGE_POSITIVE, LARGE_NEGATIVE};\n"
        "    for (size_t index = 0; index < 3; index++) {\n"
        "        StShapeDescriptor *shape = &shapes[raw_classes[index] - 1];\n"
        "        shape->minimum_allocation_size = 32;\n"
        "        shape->fixed_word_count = 1;\n"
        "        shape->fixed_pointer_bitmap = &raw_bitmap;\n"
        "        shape->fixed_pointer_bitmap_word_count = 1;\n"
        "    }\n"
        "    shapes[LARGE_POSITIVE - 1].indexed_format = ST_INDEXED_UINT32;\n"
        "    shapes[LARGE_NEGATIVE - 1].indexed_format = ST_INDEXED_UINT32;\n"
        "    st_runtime_descriptors_t descriptors = {\n"
        "        class_pointers, CLASS_COUNT - 1,\n"
        "        shape_pointers, CLASS_COUNT - 1\n"
        "    };\n"
        "    if (st_runtime_descriptors_validate(&descriptors) != ST_RUNTIME_OK) return 1;\n"
        "    st_heap_t heap = {0};\n"
        "    if (st_heap_init(&heap, &descriptors, (st_runtime_allocator_t) {0})\n"
        "            != ST_HEAP_OK) return 2;\n"
        "    st_float_primitive_context_t floats = {0};\n"
        "    if (st_float_primitive_context_init(\n"
        "            &floats, &(st_float_primitive_options_t) {\n"
        "                .heap = &heap,\n"
        "                .boxed_float64_class_id = BOXED_FLOAT64,\n"
        "                .boxed_float64_shape_id = BOXED_FLOAT64\n"
        "            }) != ST_FLOAT_PRIMITIVE_OK) return 3;\n"
        "    st_numeric_context_t numeric = {0};\n"
        "    if (st_numeric_context_init(\n"
        "            &numeric, &(st_numeric_options_t) {\n"
        "                .heap = &heap,\n"
        "                .large_positive_class_id = LARGE_POSITIVE,\n"
        "                .large_positive_shape_id = LARGE_POSITIVE,\n"
        "                .large_negative_class_id = LARGE_NEGATIVE,\n"
        "                .large_negative_shape_id = LARGE_NEGATIVE,\n"
        "                .float_primitives = &floats\n"
        "            }) != ST_INTEGER_PRIMITIVE_OK) return 4;\n"
        "    st_lookup_context_t lookup = {0};\n"
        "    if (st_lookup_context_init(\n"
        "            &lookup, &descriptors, (st_lookup_allocator_t) {0})\n"
        "            != ST_LOOKUP_FOUND) return 5;\n"
        "    st_aot_thread_t thread = {0};\n"
        "    st_control_thread_t control = {0};\n"
        "    if (st_control_thread_init(\n"
        "            &control, &thread, (st_control_allocator_t) {0})\n"
        "            != ST_CONTROL_OK) return 6;\n"
        "    uint32_t immediate[5] = {\n"
        "        NIL, FALSE_VALUE, TRUE_VALUE, SMALL_INTEGER, CHARACTER\n"
        "    };\n"
        "    if (!st_aot_thread_init(\n"
        "            &thread, &lookup, immediate, NULL, &control, NULL,\n"
        "            NULL, NULL, NULL, NULL)) return 7;\n"
        "    if (!st_aot_thread_numeric_attach(&thread, &numeric)) return 8;\n"
        "    uint64_t arguments[2] = {small_integer(1), small_integer(1)};\n"
        "    uint64_t promoted = run_numeric(\n"
        "        st_Probe_numericBinary, &numeric_method_0, &thread,\n"
        "        small_integer(1152921504606846975LL), arguments, 2);\n"
        "    uint32_t storage[2];\n"
        "    st_integer_view_t view;\n"
        "    if (st_integer_view(&numeric, promoted, storage, &view)\n"
        "            != ST_INTEGER_PRIMITIVE_OK\n"
        "            || view.negative || view.limb_count != 2\n"
        "            || view.limbs[0] != 0 || view.limbs[1] != 0x10000000)\n"
        "        return 9;\n"
        "    arguments[0] = small_integer(1152921504606846975LL);\n"
        "    if (run_numeric(\n"
        "            st_Probe_numericCompare, &numeric_method_1, &thread,\n"
        "            promoted, arguments, 1) != small_integer(1)) return 10;\n"
        "    arguments[0] = small_integer(1);\n"
        "    uint64_t shifted = run_numeric(\n"
        "        st_Probe_numericShift, &numeric_method_2, &thread,\n"
        "        promoted, arguments, 1);\n"
        "    uint32_t shifted_expected[2] = {0, 0x20000000};\n"
        "    if (st_integer_view(&numeric, shifted, storage, &view)\n"
        "            != ST_INTEGER_PRIMITIVE_OK\n"
        "            || memcmp(view.limbs, shifted_expected, sizeof(shifted_expected)) != 0)\n"
        "        return 11;\n"
        "    uint64_t boxed = run_numeric(\n"
        "        st_Probe_numericLargeFloat, &numeric_method_3, &thread,\n"
        "        promoted, NULL, 0);\n"
        "    uint64_t bits = 0;\n"
        "    if (st_float_primitive_unbox_bits(&floats, boxed, &bits)\n"
        "            != ST_FLOAT_PRIMITIVE_OK\n"
        "            || bits != UINT64_C(0x43b0000000000000)) return 12;\n"
        "    boxed = run_numeric(\n"
        "        st_Probe_numericSmallFloat, &numeric_method_4, &thread,\n"
        "        small_integer(-7), NULL, 0);\n"
        "    if (st_float_primitive_unbox_bits(&floats, boxed, &bits)\n"
        "            != ST_FLOAT_PRIMITIVE_OK\n"
        "            || bits != UINT64_C(0xc01c000000000000)) return 13;\n"
        "    uint64_t expected_hash = 0;\n"
        "    if (st_integer_hash(&numeric, promoted, &expected_hash)\n"
        "            != ST_INTEGER_PRIMITIVE_OK) return 14;\n"
        "    if (run_numeric(\n"
        "            st_Probe_numericHash, &numeric_method_5, &thread,\n"
        "            promoted, NULL, 0) != expected_hash) return 15;\n"
        "    uint64_t float_one = 0;\n"
        "    uint64_t float_two = 0;\n"
        "    if (st_float_primitive_box_bits(\n"
        "            &floats, UINT64_C(0x3ff0000000000000), &float_one)\n"
        "            != ST_FLOAT_PRIMITIVE_OK\n"
        "            || st_float_primitive_box_bits(\n"
        "                &floats, UINT64_C(0x4000000000000000), &float_two)\n"
        "                != ST_FLOAT_PRIMITIVE_OK) return 16;\n"
        "    arguments[0] = float_two;\n"
        "    boxed = run_numeric(\n"
        "        st_Probe_floatAdd, &numeric_method_6, &thread,\n"
        "        float_one, arguments, 1);\n"
        "    if (st_float_primitive_unbox_bits(&floats, boxed, &bits)\n"
        "            != ST_FLOAT_PRIMITIVE_OK\n"
        "            || bits != UINT64_C(0x4008000000000000)) return 17;\n"
        "    uint64_t float_large = 0;\n"
        "    if (st_float_primitive_box_bits(\n"
        "            &floats, UINT64_C(0x43b0000000000000), &float_large)\n"
        "            != ST_FLOAT_PRIMITIVE_OK) return 18;\n"
        "    uint64_t converted = run_numeric(\n"
        "        st_Probe_floatRounded, &numeric_method_7, &thread,\n"
        "        float_large, NULL, 0);\n"
        "    if (st_integer_view(&numeric, converted, storage, &view)\n"
        "            != ST_INTEGER_PRIMITIVE_OK\n"
        "            || view.negative || view.limb_count != 2\n"
        "            || view.limbs[0] != 0\n"
        "            || view.limbs[1] != 0x10000000) return 19;\n"
        "    uint64_t positive_zero = 0;\n"
        "    uint64_t negative_zero = 0;\n"
        "    if (st_float_primitive_box_bits(&floats, 0, &positive_zero)\n"
        "            != ST_FLOAT_PRIMITIVE_OK\n"
        "            || st_float_primitive_box_bits(\n"
        "                &floats, UINT64_C(0x8000000000000000),\n"
        "                &negative_zero) != ST_FLOAT_PRIMITIVE_OK) return 20;\n"
        "    uint64_t positive_hash = run_numeric(\n"
        "        st_Probe_floatHash, &numeric_method_8, &thread,\n"
        "        positive_zero, NULL, 0);\n"
        "    uint64_t negative_hash = run_numeric(\n"
        "        st_Probe_floatHash, &numeric_method_8, &thread,\n"
        "        negative_zero, NULL, 0);\n"
        "    if (positive_hash != negative_hash) return 21;\n"
        "    if (!st_aot_thread_numeric_detach(&thread, &numeric)) return 22;\n"
        "    st_aot_thread_destroy(&thread);\n"
        "    if (st_control_thread_destroy(&control) != ST_CONTROL_OK) return 23;\n"
        "    st_lookup_context_destroy(&lookup);\n"
        "    st_numeric_context_destroy(&numeric);\n"
        "    st_float_primitive_context_destroy(&floats);\n"
        "    st_heap_destroy(&heap);\n"
        "    return 0;\n"
        "}\n") > 0;
    return fclose(file) == 0 && ok;
}

static void execute_x86_numeric_primitives(
    const compiled_t methods[NUMERIC_AOT_METHOD_COUNT])
{
#if defined(__x86_64__) && !defined(_WIN32)
    long pid = (long)getpid();
    char assembly_paths[NUMERIC_AOT_METHOD_COUNT][160];
    char harness_path[160];
    char executable_path[160];
    char command[5000];
    bool ok = true;
    int length;

    for (size_t index = 0u; index < NUMERIC_AOT_METHOD_COUNT; index++) {
        snprintf(
            assembly_paths[index], sizeof(assembly_paths[index]),
            "/tmp/anvil-st-numeric-%ld-%zu.s", pid, index);
        ok = write_text(assembly_paths[index], methods[index].assembly) && ok;
    }
    snprintf(
        harness_path, sizeof(harness_path),
        "/tmp/anvil-st-numeric-%ld.c", pid);
    snprintf(
        executable_path, sizeof(executable_path),
        "/tmp/anvil-st-numeric-%ld", pid);
    ok = write_numeric_aot_harness(harness_path, methods) && ok;

    length = snprintf(
        command, sizeof(command),
        "/usr/bin/cc -std=c11 -no-pie -Iinclude "
        "-Isamples/smalltalk/include ");
    for (size_t index = 0u;
         length > 0 && (size_t)length < sizeof(command)
             && index < NUMERIC_AOT_METHOD_COUNT; index++) {
        length += snprintf(
            command + length, sizeof(command) - (size_t)length,
            "%s ", assembly_paths[index]);
    }
    if (length > 0 && (size_t)length < sizeof(command)) {
        length += snprintf(
            command + length, sizeof(command) - (size_t)length,
            "%s "
            "samples/smalltalk/src/runtime/value.c "
            "samples/smalltalk/src/runtime/runtime.c "
            "samples/smalltalk/src/runtime/lookup.c "
            "samples/smalltalk/src/runtime/send_bridge.c "
            "samples/smalltalk/src/runtime/control/control.c "
            "samples/smalltalk/src/runtime/control/control_roots.c "
            "samples/smalltalk/src/runtime/control/control_bridge.c "
            "samples/smalltalk/src/runtime/heap.c "
            "samples/smalltalk/src/runtime/primitives/float_primitives.c "
            "samples/smalltalk/src/runtime/primitives/"
            "float_primitive_bridge.c "
            "samples/smalltalk/src/runtime/primitives/integer_primitives.c "
            "samples/smalltalk/src/runtime/primitives/primitive_bridge.c "
            "samples/smalltalk/src/runtime/primitives/"
            "integer_primitive_bridge.c "
            "-o %s -pthread -lm",
            harness_path, executable_path);
    }
    CHECK(ok && length > 0 && (size_t)length < sizeof(command));
    if (ok && length > 0 && (size_t)length < sizeof(command)) {
        CHECK(system(command) == 0);
        int status = system(executable_path);
        if (status != 0)
            fprintf(stderr, "numeric AOT harness status=%d\n", status);
        CHECK(status == 0);
    }
    for (size_t index = 0u; index < NUMERIC_AOT_METHOD_COUNT; index++)
        unlink(assembly_paths[index]);
    unlink(harness_path);
    unlink(executable_path);
#else
    (void)methods;
#endif
}

typedef struct {
    size_t successes_before_failure;
    size_t calls;
    size_t live;
} failing_allocator_t;

static void *failing_allocate(void *user, size_t size)
{
    failing_allocator_t *state = user;
    if (state->calls++ == state->successes_before_failure) return NULL;
    void *memory = malloc(size);
    if (memory) state->live++;
    return memory;
}

static void failing_deallocate(void *user, void *pointer)
{
    failing_allocator_t *state = user;
    if (pointer) state->live--;
    free(pointer);
}

static void test_ir_fault_transactions(
    const st_class_graph_result_t *graph,
    const st_class_graph_method_t *method,
    const char *symbol,
    const st_selector_table_t *selectors)
{
    bool reached_success = false;

    CHECK(method != NULL);
    for (size_t fail_after = 0u;
         method != NULL && fail_after < 4096u; fail_after++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        st_sema_result_t sema;
        st_lower_result_t result;
        st_lower_options_t options = {
            .symbol_name = symbol,
            .linkage = ANVIL_LINK_EXTERNAL,
            .selectors = selectors
        };

        CHECK(ctx != NULL);
        if (ctx == NULL) {
            break;
        }
        CHECK(analyze(graph, method, &sema));
        st_lower_result_init(&result);
        anvil_test_fail_alloc_after(ctx, fail_after);
        st_lower_status_t status = st_lower_method(
            &result, ctx, graph, method->id, &sema, &options);
        anvil_test_disable_alloc_fail(ctx);
        if (status == ST_LOWER_OK) {
            reached_success = true;
            CHECK(result.module != NULL && result.function != NULL);
        } else {
            CHECK(status == ST_LOWER_ERR_OUT_OF_MEMORY);
            CHECK(result.module == NULL && result.function == NULL);
        }
        st_lower_result_destroy(&result);
        st_sema_result_destroy(&sema);
        anvil_ctx_destroy(ctx);
        if (reached_success) {
            break;
        }
    }
    if (!reached_success) {
        fprintf(stderr, "IR fault sweep never reached success for %s\n",
                symbol);
    }
    CHECK(reached_success);
}

static void test_diagnostics_and_transactions(
    const st_class_graph_result_t *graph,
    const st_selector_table_t *selectors)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    const st_class_graph_method_t *unsupported_method = find_method(
        graph, "Probe", "unsupported");
    const st_class_graph_method_t *huge_method = find_method(
        graph, "Probe", "huge");
    const st_class_graph_method_t *primitive_method = find_method(
        graph, "Probe", "primitive");
    const st_class_graph_method_t *assignment_method = find_method(
        graph, "Probe", "assignments");
    const st_class_graph_method_t *ivar_method = find_method(
        graph, "Probe", "ivarWrite:");
    const st_class_graph_method_t *ivar_closure_method = find_method(
        graph, "Probe", "ivarClosureRead");
    const st_class_graph_method_t *while_method = find_method(
        graph, "Probe", "closureWhileLoop");
    st_lower_options_t options = {
        .symbol_name = "st_negative",
        .linkage = ANVIL_LINK_EXTERNAL
    };
    const st_class_graph_method_t *cases[] = {
        unsupported_method, huge_method, primitive_method
    };
    st_lower_diagnostic_code_t codes[] = {
        ST_LOWER_DIAG_UNSUPPORTED_SEND,
        ST_LOWER_DIAG_LITERAL_OUT_OF_RANGE,
        ST_LOWER_DIAG_UNSUPPORTED_PRIMITIVE
    };
    CHECK(ctx != NULL);
    for (size_t index = 0u; ctx && index < 3u; index++) {
        st_sema_result_t sema;
        st_lower_result_t result;
        CHECK(analyze(graph, cases[index], &sema));
        st_lower_result_init(&result);
        CHECK(st_lower_method(&result, ctx, graph, cases[index]->id,
                              &sema, &options) == ST_LOWER_ERR_UNSUPPORTED);
        CHECK(result.module == NULL && result.function == NULL);
        CHECK(result.diagnostic.code == codes[index]);
        CHECK(result.diagnostic.has_span);
        st_lower_result_destroy(&result);
        st_sema_result_destroy(&sema);
    }

    if (ctx && assignment_method) {
        st_sema_result_t sema;
        st_lower_result_t result;
        failing_allocator_t allocator = {0u, 0u, 0u};
        CHECK(analyze(graph, assignment_method, &sema));
        st_lower_result_init(&result);
        options.symbol_name = "bad\n.globl injected";
        CHECK(st_lower_method(&result, ctx, graph, assignment_method->id,
                              &sema, &options) ==
              ST_LOWER_ERR_INVALID_ARGUMENT);
        CHECK(result.module == NULL && result.function == NULL);
        CHECK(result.diagnostic.code == ST_LOWER_DIAG_INVALID_INPUT);
        st_lower_result_destroy(&result);

        options.symbol_name = "st_negative";
        options.allocator.allocate = failing_allocate;
        options.allocator.deallocate = failing_deallocate;
        options.allocator.user = &allocator;
        st_lower_result_init(&result);
        CHECK(st_lower_method(&result, ctx, graph, assignment_method->id,
                              &sema, &options) == ST_LOWER_ERR_OUT_OF_MEMORY);
        CHECK(result.module == NULL && result.function == NULL);
        CHECK(allocator.live == 0u);
        st_lower_result_destroy(&result);
        st_sema_result_destroy(&sema);
    }
    anvil_ctx_destroy(ctx);

    test_ir_fault_transactions(
        graph, assignment_method, "st_assignment_fault_sweep", NULL);
    test_ir_fault_transactions(
        graph, ivar_method, "st_ivar_fault_sweep", NULL);
    test_ir_fault_transactions(
        graph, ivar_closure_method, "st_ivar_closure_fault_sweep", NULL);
    test_ir_fault_transactions(
        graph, while_method, "st_while_true_fault_sweep", selectors);
}

static void test_primitive_fault_transactions(
    const st_class_graph_result_t *graph,
    const st_primitive_result_t *primitives,
    const st_selector_table_t *selectors)
{
    const st_class_graph_method_t *method = find_method(
        graph, "Probe", "primitiveAdd:");
    const st_primitive_binding_t *binding = find_primitive_binding(
        primitives, method ? method->node : NULL);
    bool reached_success = false;
    CHECK(method != NULL && binding != NULL);
    for (size_t fail_after = 0u;
         method && binding && fail_after < 768u; fail_after++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        st_sema_result_t sema;
        st_lower_result_t result;
        st_lower_options_t options = {
            .symbol_name = "st_primitive_fault_sweep",
            .linkage = ANVIL_LINK_EXTERNAL,
            .selectors = selectors,
            .primitive_binding = binding
        };
        CHECK(ctx != NULL);
        if (!ctx) break;
        CHECK(analyze(graph, method, &sema));
        st_lower_result_init(&result);
        anvil_test_fail_alloc_after(ctx, fail_after);
        st_lower_status_t status = st_lower_method(
            &result, ctx, graph, method->id, &sema, &options);
        anvil_test_disable_alloc_fail(ctx);
        if (status == ST_LOWER_OK) {
            reached_success = true;
            CHECK(result.module != NULL && result.function != NULL);
        } else {
            CHECK(status == ST_LOWER_ERR_OUT_OF_MEMORY);
            CHECK(result.module == NULL && result.function == NULL);
        }
        st_lower_result_destroy(&result);
        st_sema_result_destroy(&sema);
        anvil_ctx_destroy(ctx);
        if (reached_success) break;
    }
    CHECK(reached_success);
}

static void test_closure_fault_transactions(
    const st_class_graph_result_t *graph,
    const st_selector_table_t *selectors)
{
    const st_class_graph_method_t *method = find_method(
        graph, "Probe", "closureCapture:");
    bool reached_success = false;
    CHECK(method != NULL);
    for (size_t fail_after = 0u; method && fail_after < 32u; fail_after++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        st_sema_result_t sema;
        st_lower_result_t result;
        failing_allocator_t allocator = { fail_after, 0u, 0u };
        st_lower_options_t options = {
            .symbol_name = "st_closure_host_fault_sweep",
            .linkage = ANVIL_LINK_EXTERNAL,
            .selectors = selectors,
            .allocator = {
                failing_allocate, failing_deallocate, &allocator
            }
        };
        CHECK(ctx != NULL && analyze(graph, method, &sema));
        if (!ctx) break;
        st_lower_result_init(&result);
        st_lower_status_t status = st_lower_method(
            &result, ctx, graph, method->id, &sema, &options);
        if (status == ST_LOWER_OK) {
            reached_success = true;
            CHECK(result.module != NULL && result.block_count == 1u);
        } else {
            CHECK(status == ST_LOWER_ERR_OUT_OF_MEMORY);
            CHECK(result.module == NULL && result.function == NULL
                  && result.blocks == NULL && result.block_count == 0u);
        }
        st_lower_result_destroy(&result);
        CHECK(allocator.live == 0u);
        st_sema_result_destroy(&sema);
        anvil_ctx_destroy(ctx);
        if (reached_success) break;
    }
    CHECK(reached_success);

    reached_success = false;
    for (size_t fail_after = 0u; method && fail_after < 1024u; fail_after++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        st_sema_result_t sema;
        st_lower_result_t result;
        st_lower_options_t options = {
            .symbol_name = "st_closure_ir_fault_sweep",
            .linkage = ANVIL_LINK_EXTERNAL,
            .selectors = selectors
        };
        CHECK(ctx != NULL && analyze(graph, method, &sema));
        if (!ctx) break;
        st_lower_result_init(&result);
        anvil_test_fail_alloc_after(ctx, fail_after);
        st_lower_status_t status = st_lower_method(
            &result, ctx, graph, method->id, &sema, &options);
        anvil_test_disable_alloc_fail(ctx);
        if (status == ST_LOWER_OK) {
            reached_success = true;
            CHECK(result.module != NULL && result.block_count == 1u);
        } else {
            CHECK(status == ST_LOWER_ERR_OUT_OF_MEMORY);
            CHECK(result.module == NULL && result.function == NULL
                  && result.blocks == NULL && result.block_count == 0u);
        }
        st_lower_result_destroy(&result);
        st_sema_result_destroy(&sema);
        anvil_ctx_destroy(ctx);
        if (reached_success) break;
    }
    CHECK(reached_success);
}

static void test_literal_fault_transactions(
    const st_class_graph_result_t *graph)
{
    const st_class_graph_method_t *method = find_method(
        graph, "Probe", "twoStrings");
    bool reached_success = false;
    CHECK(method != NULL);
    for (size_t fail_after = 0u; method && fail_after < 32u; fail_after++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        st_sema_result_t sema;
        st_lower_result_t result;
        failing_allocator_t allocator = { fail_after, 0u, 0u };
        st_lower_options_t options = {
            .symbol_name = "st_literal_host_fault_sweep",
            .linkage = ANVIL_LINK_EXTERNAL,
            .literal_base_index = 13u,
            .allocator = {
                failing_allocate, failing_deallocate, &allocator
            }
        };
        CHECK(ctx != NULL && analyze(graph, method, &sema));
        if (!ctx) break;
        st_lower_result_init(&result);
        st_lower_status_t status = st_lower_method(
            &result, ctx, graph, method->id, &sema, &options);
        if (status == ST_LOWER_OK) {
            reached_success = true;
            CHECK(result.module != NULL
                  && result.string_literal_count == 2u);
        } else {
            CHECK(status == ST_LOWER_ERR_OUT_OF_MEMORY);
            CHECK(result.module == NULL && result.function == NULL
                  && result.string_literals == NULL
                  && result.string_literal_count == 0u);
        }
        st_lower_result_destroy(&result);
        CHECK(allocator.live == 0u);
        st_sema_result_destroy(&sema);
        anvil_ctx_destroy(ctx);
        if (reached_success) break;
    }
    CHECK(reached_success);
}

int main(void)
{
    fixture_t fixtures[FIXTURE_COUNT];
    const st_ast_unit_t *units[FIXTURE_COUNT];
    const char *paths[] = {
        "samples/smalltalk/st-image/Kernel/Object.st",
        "samples/smalltalk/st-image/Kernel/Boolean.st",
        "samples/smalltalk/st-image/Kernel/True.st",
        "samples/smalltalk/st-image/Kernel/False.st",
        "samples/smalltalk/tests/fixtures/HelloApplication.st",
        "samples/smalltalk/st-image/Kernel/Behavior.st",
        "samples/smalltalk/st-image/Execution/Block.st",
        "samples/smalltalk/st-image/Exceptions/Exception.st",
        "samples/smalltalk/st-image/Exceptions/Error.st",
        "samples/smalltalk/st-image/Magnitudes/Magnitude.st",
        "samples/smalltalk/st-image/Magnitudes/Number.st",
        "samples/smalltalk/st-image/Magnitudes/Integer.st",
        "samples/smalltalk/st-image/Magnitudes/SmallInteger.st",
        "samples/smalltalk/st-image/Magnitudes/LargeInteger.st",
        "samples/smalltalk/st-image/Magnitudes/Character.st"
        ,"samples/smalltalk/st-image/Collections/Collection.st"
        ,"samples/smalltalk/st-image/Collections/SequenceableCollection.st"
        ,"samples/smalltalk/st-image/Collections/ArrayedCollection.st"
        ,"samples/smalltalk/st-image/Collections/Array.st"
        ,"samples/smalltalk/st-image/Collections/String.st"
    };
    const char *probe_source =
        "Probe := Object [ | first second | "
        "assignments [ | a | a := 41. a := 42. ^a ] "
        "character [ ^$A ] "
        "branch [ ^true ifTrue: [ 41 ] ifFalse: [ 7 ] ] "
        "branchFalse [ ^false ifTrue: [ 41 ] ifFalse: [ 7 ] ] "
        "branchReturn [ true ifTrue: [ ^41 ]. ^7 ] "
        "bothReturn [ true ifTrue: [ ^41 ] ifFalse: [ ^7 ] ] "
        "implicit [ 99 ] "
        "nilLiteral [ ^nil ] "
        "minSmallInteger [ ^-1152921504606846976 ] "
        "maxSmallInteger [ ^1152921504606846975 ] "
        "general [ ^true & false ] "
        "nlrSend: ignored [ true ifTrue: [ ^41 ]. ^7 ] "
        "closureMake0 [ ^[ 41 ] ] "
        "closureMake1 [ ^[ :x | x ] ] "
        "closureCall0 [ ^[ 42 ] value ] "
        "closureCall1 [ ^[ :x | x ] value: 43 ] "
        "closureNlr [ ^[ ^44 ] value ] "
        "closureCapture: x [ ^[ x ] ] "
        "closureSelf [ ^[ self ] ] "
        "closureReturned [ ^[ ^45 ] ] "
        "closureCell [ | x b | b := [ x ]. x := 2. ^b ] "
        "closureNested: x [ ^[ [ x ] ] ] "
        "closureArity2 [ ^[ :x :y | y ] value: 41 value: 42 ] "
        "closureArity3 [ ^[ :x :y :z | z ] value: 41 value: 42 value: 43 ] "
        "closureNestedCall: x [ ^[ [ x ] value ] value ] "
        "closureCellSiblings [ | x | x := 1. [ x := 2 ] value. ^[ x ] value ] "
        "closureSend [ ^[ self yourself ] value ] "
        "closureWhileLoop [ | continue result | continue := true. result := 0. "
        "[ continue ] whileTrue: [ result := 3. continue := false ]. ^result ] "
        "dynamicValue: block [ ^block value ] "
        "selfSend [ ^self yourself ] "
        "nestedSend [ ^self largeOp: self yourself with: self yourself ] "
        "superSend [ ^super yourself ] "
        "cascade [ ^self yourself; yourself ] "
        "globalRead [ ^Transcript ] "
        "stringLiteral [ ^'hello lowering' ] "
        "twoStrings [ 'first'. ^'second' ] "
        "class streamWrite: descriptor next: count from: string [ "
        "<primitive: StreamWritePrimitive> ^77 ] "
        "asSymbolRuntime [ <primitive: StringAsSymbolPrimitive> ^87 ] "
        "blockValue0 [ <primitive: BlockValuePrimitive> ^90 ] "
        "blockValue1: argument [ <primitive: BlockValuePrimitive1> ^91 ] "
        "blockValue2: first with: second [ <primitive: BlockValuePrimitive2> ^92 ] "
        "blockValue3: first with: second with: third [ <primitive: BlockValuePrimitive3> ^93 ] "
        "blockValueArguments: arguments [ <primitive: BlockValueArgsPrimitive> ^94 ] "
        "blockWhileTrue: body [ <primitive: BlockWhileTruePrimitive> ^95 ] "
        "primitiveAdd: value [ <primitive: IntAddPrimitive> ^7 ] "
        "primitiveDivide: value [ <primitive: IntFloorDivPrimitive> ^99 ] "
        "numericBinary: opcode with: value [ "
        "<primitive: LargeIntBinaryPrimitive> ^90 ] "
        "numericCompare: value [ "
        "<primitive: LargeIntComparePrimitive> ^91 ] "
        "numericShift: count [ "
        "<primitive: LargeIntShiftPrimitive> ^92 ] "
        "numericLargeFloat [ "
        "<primitive: LargeIntAsFloatPrimitive> ^93 ] "
        "numericSmallFloat [ "
        "<primitive: IntAsFloatPrimitive> ^94 ] "
        "numericHash [ <primitive: IntegerHashPrimitive> ] "
        "floatAdd: value [ <primitive: FloatAddPrimitive> ^96 ] "
        "floatRounded [ <primitive: FloatRoundedPrimitive> ^97 ] "
        "floatHash [ <primitive: FloatHashPrimitive> ] "
        "heapSize [ <primitive: SizePrimitive> ^77 ] "
        "heapAt: index [ <primitive: AtPrimitive> ^81 ] "
        "heapAt: index put: value [ <primitive: AtPutPrimitive> ^82 ] "
        "heapInstVarAt: index [ <primitive: InstVarAtPrimitive> ^83 ] "
        "heapInstVarAt: index put: value [ <primitive: InstVarAtPutPrimitive> ^84 ] "
        "heapNew [ <primitive: BehaviorNewPrimitive> ^78 ] "
        "heapNew: size [ <primitive: BehaviorNewSizePrimitive> ^79 ] "
        "heapClass [ <primitive: ClassPrimitive> ^80 ] "
        "heapHash [ <primitive: HashPrimitive> ^85 ] "
        "heapStringEquals: other [ <primitive: ArrayEqualsPrimitive> ^false ] "
        "heapStringHash [ <primitive: StringHashPrimitive> ^86 ] "
        "ivarRead [ ^first ] "
        "ivarWrite: value [ first := value. ^first ] "
        "ivarSecond: value [ second := value. ^second ] "
        "ivarClosureRead [ ^[ first ] value ] "
        "unsupported [ ^self foo ] "
        "huge [ ^1152921504606846976 ] "
        "primitive [ <primitive: Foo> ] ]";
    memset(fixtures, 0, sizeof(fixtures));
    bool initialized = true;
    size_t initialized_count = 0u;
    for (size_t index = 0u; index < ARRAY_COUNT(paths); index++) {
        if (!fixture_init_file(&fixtures[index], paths[index])) {
            initialized = false;
            break;
        }
        initialized_count++;
    }
    if (initialized && fixture_init_text(&fixtures[FIXTURE_COUNT - 1u], "Probe.st",
                                          probe_source)) {
        initialized_count++;
    } else {
        initialized = false;
    }
    CHECK(initialized);
    if (!initialized) goto done;
    for (size_t index = 0u; index < FIXTURE_COUNT; index++)
        units[index] = &fixtures[index].unit;

    st_class_graph_result_t graph;
    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, units, FIXTURE_COUNT, NULL)
          == ST_CLASS_GRAPH_OK);
    CHECK(st_class_graph_succeeded(&graph));
    if (!st_class_graph_succeeded(&graph)) {
        st_class_graph_result_destroy(&graph);
        goto done;
    }
    st_primitive_catalog_t primitive_catalog = {0};
    st_primitive_result_t primitive_bindings;
    st_primitive_result_init(&primitive_bindings);
    CHECK(st_primitive_catalog_init(&primitive_catalog,
                                    (st_primitive_allocator_t){0}));
    size_t core_spec_count = 0u;
    const st_primitive_spec_t *core_specs = st_core_primitive_specs(
        &core_spec_count);
    for (size_t index = 0u; index < core_spec_count; index++)
        CHECK(st_primitive_catalog_register(
                  &primitive_catalog, &core_specs[index], NULL)
              == ST_PRIMITIVE_OK);
    size_t heap_spec_count = 0u;
    const st_primitive_spec_t *heap_specs = st_heap_primitive_specs(
        &heap_spec_count);
    for (size_t index = 0u; index < heap_spec_count; index++)
        CHECK(st_primitive_catalog_register(
                  &primitive_catalog, &heap_specs[index], NULL)
              == ST_PRIMITIVE_OK);
    size_t stream_spec_count = 0u;
    const st_primitive_spec_t *stream_specs = st_stream_primitive_specs(
        &stream_spec_count);
    for (size_t index = 0u; index < stream_spec_count; index++)
        CHECK(st_primitive_catalog_register(
                  &primitive_catalog, &stream_specs[index], NULL)
              == ST_PRIMITIVE_OK);
    size_t string_spec_count = 0u;
    const st_primitive_spec_t *string_specs = st_string_primitive_specs(
        &string_spec_count);
    for (size_t index = 0u; index < string_spec_count; index++)
        CHECK(st_primitive_catalog_register(
                  &primitive_catalog, &string_specs[index], NULL)
              == ST_PRIMITIVE_OK);
    size_t block_spec_count = 0u;
    const st_primitive_spec_t *block_specs = st_block_primitive_specs(
        &block_spec_count);
    for (size_t index = 0u; index < block_spec_count; ++index)
        CHECK(st_primitive_catalog_register(
                  &primitive_catalog, &block_specs[index], NULL)
              == ST_PRIMITIVE_OK);
    size_t integer_spec_count = 0u;
    const st_primitive_spec_t *integer_specs = st_integer_primitive_specs(
        &integer_spec_count);
    for (size_t index = 0u; index < integer_spec_count; ++index)
        CHECK(st_primitive_catalog_register(
                  &primitive_catalog, &integer_specs[index], NULL)
              == ST_PRIMITIVE_OK);
    size_t float_spec_count = 0u;
    const st_primitive_spec_t *float_specs = st_float_primitive_specs(
        &float_spec_count);
    for (size_t index = 0u; index < float_spec_count; ++index)
        CHECK(st_primitive_catalog_register(
                  &primitive_catalog, &float_specs[index], NULL)
              == ST_PRIMITIVE_OK);
    CHECK(st_primitive_resolve(&primitive_bindings, units, FIXTURE_COUNT,
                               &primitive_catalog, NULL) == ST_PRIMITIVE_OK);
    CHECK(primitive_bindings.binding_count == 79u);
    st_selector_table_t send_selectors = {0};
    st_selector_id_t not_id = 0u;
    st_selector_id_t and_id = 0u;
    st_selector_id_t yourself_id = 0u;
    st_selector_id_t large_op_id = 0u;
    st_selector_id_t value_id = 0u;
    st_selector_id_t signal_id = 0u;
    st_selector_id_t while_true_id = 0u;
    CHECK(st_selector_table_init(&send_selectors,
                                 (st_selector_allocator_t){0},
                                 UINT64_C(0x53454e44)));
    CHECK(st_selector_intern(&send_selectors, "not", 3u, &not_id)
          == ST_SELECTOR_OK);
    CHECK(st_selector_intern(&send_selectors, "&", 1u, &and_id)
          == ST_SELECTOR_OK);
    CHECK(st_selector_intern(&send_selectors, "yourself", 8u, &yourself_id)
          == ST_SELECTOR_OK);
    CHECK(st_selector_intern(&send_selectors, "largeOp:with:", 13u,
                             &large_op_id) == ST_SELECTOR_OK);
    CHECK(st_selector_intern(&send_selectors, "value", 5u, &value_id)
          == ST_SELECTOR_OK);
    CHECK(st_selector_intern(&send_selectors, "signal", 6u, &signal_id)
          == ST_SELECTOR_OK);
    CHECK(st_selector_intern(&send_selectors, "whileTrue:", 10u,
                             &while_true_id) == ST_SELECTOR_OK);
    CHECK(st_selector_table_freeze(&send_selectors));

    const char *classes[] = {
        "HelloApplication", "Object", "True", "False", "True",
        "Probe", "Probe", "Probe", "Probe", "Probe", "Probe", "Probe",
        "Probe", "Probe", "Probe"
    };
    const char *selectors[] = {
        "run", "yourself", "not", "not", "&",
        "assignments", "character", "branch", "branchFalse", "branchReturn",
        "bothReturn", "implicit", "nilLiteral", "minSmallInteger",
        "maxSmallInteger"
    };
    const char *symbols[] = {
        "st_HelloApplication_run", "st_Object_yourself", "st_True_not",
        "st_False_not", "st_True_and", "st_Probe_assignments",
        "st_Probe_character", "st_Probe_branch", "st_Probe_branchFalse",
        "st_Probe_branchReturn", "st_Probe_bothReturn",
        "st_Probe_implicit", "st_Probe_nilLiteral",
        "st_Probe_minSmallInteger", "st_Probe_maxSmallInteger"
    };
    _Static_assert(ARRAY_COUNT(classes) == EXECUTED_METHOD_COUNT,
                   "class/method test table is incomplete");
    _Static_assert(ARRAY_COUNT(selectors) == EXECUTED_METHOD_COUNT,
                   "selector test table is incomplete");
    _Static_assert(ARRAY_COUNT(symbols) == EXECUTED_METHOD_COUNT,
                   "symbol test table is incomplete");
    compiled_t compiled[EXECUTED_METHOD_COUNT];
    memset(compiled, 0, sizeof(compiled));
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    CHECK(ctx != NULL);
    size_t compiled_count = 0u;
    if (ctx) {
        for (size_t index = 0u; index < EXECUTED_METHOD_COUNT; index++) {
            if (!compile_method(ctx, &graph, classes[index], selectors[index],
                                symbols[index], NULL, NULL,
                                &compiled[index])) break;
            compiled_count++;
        }
        CHECK(compiled_count == EXECUTED_METHOD_COUNT);
        if (compiled_count == EXECUTED_METHOD_COUNT) {
            CHECK(anvil_type_size(compiled[0].result.frame_type)
                  == sizeof(StFrame));
            CHECK(anvil_type_struct_field_offset(
                      compiled[0].result.frame_type,
                      ST_FRAME_RECEIVER_FIELD) == offsetof(StFrame, receiver));
            CHECK(anvil_type_struct_field_offset(
                      compiled[0].result.frame_type,
                      ST_FRAME_SAFEPOINT_FIELD) == offsetof(StFrame,
                                                             safepoint_id));
            CHECK(anvil_type_func_cc_value(compiled[0].result.method_type)
                  == ANVIL_CC_SYSV);
            CHECK(compiled[7].result.function->num_blocks >= 4u);
            CHECK(function_has_op(compiled[7].result.function, ANVIL_OP_PHI));
            CHECK(compiled[8].result.function->num_blocks >= 4u);
            CHECK(function_has_op(compiled[8].result.function, ANVIL_OP_PHI));
            execute_x86_methods(compiled, compiled_count);
        }
    }
    for (size_t index = 0u; index < compiled_count; index++)
        compiled_destroy(&compiled[index]);

    if (ctx) {
        compiled_t caller;
        compiled_t nlr_callee;
        compiled_t super_send;
        const st_class_graph_method_t *super_method = find_method(
            &graph, "Probe", "superSend");
        CHECK(compile_method(ctx, &graph, "Probe", "general",
                             "st_Probe_general", &send_selectors, NULL,
                             &caller));
        CHECK(compile_method(ctx, &graph, "Probe", "nlrSend:",
                             "st_Probe_nlrSend", &send_selectors,
                             NULL, &nlr_callee));
        CHECK(compile_method(ctx, &graph, "Probe", "superSend",
                             "st_Probe_superSend", &send_selectors,
                             NULL, &super_send));
        if (caller.result.module && nlr_callee.result.module)
            execute_x86_general_send(&caller, &nlr_callee, and_id);
        if (super_send.result.module && super_method) {
            CHECK(super_send.result.send_site_count == 1u);
            CHECK(first_site_has_lexical_owner(&super_send,
                                               super_method->owner));
        }
        compiled_destroy(&caller);
        compiled_destroy(&nlr_callee);
        compiled_destroy(&super_send);

        compiled_t cascade_code;
        CHECK(compile_method(ctx, &graph, "Probe", "cascade",
                             "st_Probe_cascade", &send_selectors, NULL,
                             &cascade_code));
        CHECK(cascade_code.result.module != NULL
              && cascade_code.result.function != NULL);
        CHECK(cascade_code.result.send_site_count == 2u);
        CHECK(cascade_code.result.safepoint_count == 3u
              && cascade_code.result.root_map_count == 3u);
        CHECK(cascade_code.result.required_root_capacity == 4u);
        if (cascade_code.result.module != NULL && cascade_code.assembly != NULL)
            execute_x86_cascade(&cascade_code, yourself_id);
        compiled_destroy(&cascade_code);

        {
            const st_class_graph_method_t *literal_method = find_method(
                &graph, "Probe", "twoStrings");
            st_sema_result_t literal_sema;
            st_lower_result_t literal_result;
            st_lower_options_t literal_options = {
                .symbol_name = "st_Probe_twoStrings",
                .linkage = ANVIL_LINK_EXTERNAL,
                .literal_base_index = 41u
            };
            CHECK(analyze(&graph, literal_method, &literal_sema));
            st_lower_result_init(&literal_result);
            CHECK(st_lower_method(
                      &literal_result, ctx, &graph, literal_method->id,
                      &literal_sema, &literal_options) == ST_LOWER_OK);
            CHECK(literal_result.string_literal_count == 2u
                  && literal_result.string_literals != NULL);
            if (literal_result.string_literal_count == 2u) {
                CHECK(literal_result.string_literals[0].literal_id == 41u);
                CHECK(literal_result.string_literals[0].length == 5u);
                CHECK(memcmp(literal_result.string_literals[0].bytes,
                             "first", 5u) == 0);
                CHECK(literal_result.string_literals[1].literal_id == 42u);
                CHECK(literal_result.string_literals[1].length == 6u);
                CHECK(memcmp(literal_result.string_literals[1].bytes,
                             "second", 6u) == 0);
            }
            char *literal_assembly = NULL;
            size_t literal_assembly_length = 0u;
            CHECK(literal_result.module != NULL
                  && anvil_module_codegen(
                      literal_result.module, &literal_assembly,
                      &literal_assembly_length) == ANVIL_OK);
            CHECK(literal_assembly != NULL
                  && strstr(literal_assembly,
                            "st_image_runtime_literal_load") != NULL);
            free(literal_assembly);
            st_lower_result_t overflow_result;
            st_lower_options_t overflow_options = {
                .symbol_name = "st_Probe_literalOverflow",
                .linkage = ANVIL_LINK_EXTERNAL,
                .literal_base_index = UINT32_MAX
            };
            st_lower_result_init(&overflow_result);
            CHECK(st_lower_method(
                      &overflow_result, ctx, &graph, literal_method->id,
                      &literal_sema, &overflow_options)
                  == ST_LOWER_ERR_OVERFLOW);
            CHECK(overflow_result.module == NULL
                  && overflow_result.string_literals == NULL);
            st_lower_result_destroy(&overflow_result);
            st_sema_result_destroy(&literal_sema);
            CHECK(literal_result.string_literals != NULL
                  && memcmp(literal_result.string_literals[1].bytes,
                            "second", 6u) == 0);
            st_lower_result_destroy(&literal_result);
        }

        {
            const uint32_t transcript_external_id = UINT32_C(0xf0000001);
            const st_class_graph_method_t *global_method = find_method(
                &graph, "Probe", "globalRead");
            st_sema_result_t global_sema;
            st_lower_result_t global_result;
            const st_lower_global_binding_t globals[] = {
                { UINT32_C(0xf0000001), 7u }
            };
            st_lower_options_t global_options = {
                .symbol_name = "st_Probe_globalRead",
                .linkage = ANVIL_LINK_EXTERNAL,
                .globals = globals,
                .global_count = ARRAY_COUNT(globals)
            };
            CHECK(analyze_with_external_global(
                &graph, global_method, "Transcript",
                transcript_external_id, &global_sema));
            st_lower_result_init(&global_result);
            CHECK(st_lower_method(
                      &global_result, ctx, &graph, global_method->id,
                      &global_sema, &global_options) == ST_LOWER_OK);
            char *global_assembly = NULL;
            size_t global_assembly_length = 0u;
            CHECK(global_result.module != NULL
                  && anvil_module_codegen(
                      global_result.module, &global_assembly,
                      &global_assembly_length) == ANVIL_OK);
            CHECK(global_assembly != NULL
                  && strstr(global_assembly,
                            "st_image_runtime_global_load") != NULL
                  && strstr(global_assembly,
                            "st_aot_image_runtime_contract_violation")
                      != NULL);
            free(global_assembly);
            st_lower_result_destroy(&global_result);

            st_lower_result_t missing_global;
            st_lower_options_t missing_options = {
                .symbol_name = "st_Probe_missingGlobal",
                .linkage = ANVIL_LINK_EXTERNAL
            };
            st_lower_result_init(&missing_global);
            CHECK(st_lower_method(
                      &missing_global, ctx, &graph, global_method->id,
                      &global_sema, &missing_options)
                  == ST_LOWER_ERR_UNSUPPORTED);
            CHECK(missing_global.module == NULL
                  && missing_global.diagnostic.code
                    == ST_LOWER_DIAG_UNSUPPORTED_BINDING);
            st_lower_result_destroy(&missing_global);

            const st_lower_global_binding_t duplicate_globals[] = {
                { 1u, 7u }, { 2u, 7u }
            };
            st_lower_options_t duplicate_options = {
                .symbol_name = "st_Probe_duplicateGlobal",
                .linkage = ANVIL_LINK_EXTERNAL,
                .globals = duplicate_globals,
                .global_count = ARRAY_COUNT(duplicate_globals)
            };
            st_lower_result_t duplicate_result;
            st_lower_result_init(&duplicate_result);
            CHECK(st_lower_method(
                      &duplicate_result, ctx, &graph, global_method->id,
                      &global_sema, &duplicate_options)
                  == ST_LOWER_ERR_INVALID_ARGUMENT);
            CHECK(duplicate_result.module == NULL);
            st_lower_result_destroy(&duplicate_result);
            st_sema_result_destroy(&global_sema);
        }

        {
            const uint32_t transcript_external_id = UINT32_C(0xf0000001);
            const st_lower_global_binding_t globals[] = {
                { UINT32_C(0xf0000001), 7u }
            };
            const st_class_graph_method_t *global_method = find_method(
                &graph, "Probe", "globalRead");
            const st_class_graph_method_t *stream_method = find_class_method(
                &graph, "Probe", "streamWrite:next:from:");
            const st_primitive_binding_t *stream_binding =
                find_primitive_binding(
                    &primitive_bindings,
                    stream_method ? stream_method->node : NULL);
            st_sema_result_t global_sema;
            compiled_t global_runtime = {0};
            compiled_t literal_runtime = {0};
            compiled_t stream_runtime = {0};
            st_lower_result_init(&global_runtime.result);
            CHECK(analyze_with_external_global(
                &graph, global_method, "Transcript",
                transcript_external_id, &global_sema));
            st_lower_options_t global_options = {
                .symbol_name = "st_Probe_globalRead",
                .linkage = ANVIL_LINK_EXTERNAL,
                .globals = globals,
                .global_count = ARRAY_COUNT(globals)
            };
            CHECK(st_lower_method(
                      &global_runtime.result, ctx, &graph, global_method->id,
                      &global_sema, &global_options) == ST_LOWER_OK);
            CHECK(global_runtime.result.module != NULL
                  && anvil_module_codegen(
                      global_runtime.result.module, &global_runtime.assembly,
                      &global_runtime.assembly_length) == ANVIL_OK);
            st_sema_result_destroy(&global_sema);
            CHECK(compile_method(
                ctx, &graph, "Probe", "twoStrings", "st_Probe_twoStrings",
                &send_selectors, NULL, &literal_runtime));
            CHECK(compile_method(
                ctx, &graph, "Probe", "streamWrite:next:from:",
                "st_Probe_streamWrite", &send_selectors, stream_binding,
                &stream_runtime));
            if (global_runtime.assembly && literal_runtime.assembly
                    && stream_runtime.assembly)
                execute_x86_image_and_stream_lowering(
                    &global_runtime, &literal_runtime, &stream_runtime);
            compiled_destroy(&global_runtime);
            compiled_destroy(&literal_runtime);
            compiled_destroy(&stream_runtime);
        }

        static const char *const closure_selectors[CLOSURE_METHOD_COUNT] = {
            "closureMake0", "closureMake1", "closureCall0",
            "closureCall1", "closureNlr", "closureCapture:",
            "closureSelf", "closureReturned", "closureArity2",
            "closureArity3", "closureNestedCall:",
            "closureCellSiblings", "closureSend", "closureWhileLoop"
        };
        static const char *const closure_symbols[CLOSURE_METHOD_COUNT] = {
            "st_Probe_closureMake0", "st_Probe_closureMake1",
            "st_Probe_closureCall0", "st_Probe_closureCall1",
            "st_Probe_closureNlr", "st_Probe_closureCapture",
            "st_Probe_closureSelf", "st_Probe_closureReturned",
            "st_Probe_closureArity2", "st_Probe_closureArity3",
            "st_Probe_closureNestedCall",
            "st_Probe_closureCellSiblings", "st_Probe_closureSend",
            "st_Probe_closureWhileLoop"
        };
        compiled_t closure_methods[CLOSURE_METHOD_COUNT];
        memset(closure_methods, 0, sizeof(closure_methods));
        bool closures_compiled = true;
        for (size_t index = 0u; index < CLOSURE_METHOD_COUNT; index++) {
            closures_compiled = compile_method(
                ctx, &graph, "Probe", closure_selectors[index],
                closure_symbols[index], &send_selectors, NULL,
                &closure_methods[index]) && closures_compiled;
            if (closure_methods[index].result.module) {
                const st_lower_block_artifact_t *block =
                    closure_methods[index].result.blocks;
                size_t expected_blocks = index == 10u || index == 11u
                        || index == 13u
                    ? 2u : 1u;
                CHECK(closure_methods[index].result.block_count
                      == expected_blocks);
                CHECK(block != NULL && block->function != NULL);
                CHECK(block != NULL && block->lexical_ordinal == 0u);
                CHECK(block != NULL && block->descriptor_symbol.bytes != NULL
                      && block->descriptor_symbol.length != 0u);
                CHECK(block != NULL
                      && block->method_descriptor_symbol.bytes != NULL
                      && block->method_descriptor_symbol.length != 0u);
                if (index == 0u) {
                    CHECK(block->method_flags == 0u);
                } else if (index == 8u) {
                    CHECK(block->arity == 2u);
                } else if (index == 9u) {
                    CHECK(block->arity == 3u);
                } else if (index == 10u) {
                    CHECK(closure_methods[index].result.blocks[1]
                          .lexical_ordinal == 1u);
                } else if (index == 11u) {
                    const st_lower_block_artifact_t *sibling =
                        &closure_methods[index].result.blocks[1];
                    CHECK((block->flags & ST_AOT_BLOCK_HAS_CELLS) != 0u
                          && (sibling->flags & ST_AOT_BLOCK_HAS_CELLS) != 0u);
                    CHECK(block->capture_count == 1u
                          && sibling->capture_count == 1u
                          && block->captures[0].kind == ST_AOT_CAPTURE_CELL
                          && sibling->captures[0].kind == ST_AOT_CAPTURE_CELL
                          && block->captures[0].binding_id
                             == sibling->captures[0].binding_id);
                } else if (index == 12u) {
                    CHECK((block->method_flags & ST_METHOD_CAN_UNWIND) != 0u
                          && (block->method_flags
                              & ST_METHOD_HAS_NON_LOCAL_RETURN) == 0u
                          && (block->flags & ST_AOT_BLOCK_HAS_HOME) == 0u);
                } else if (index == 13u) {
                    const st_lower_block_artifact_t *body =
                        &closure_methods[index].result.blocks[1];
                    CHECK(closure_methods[index].result.send_site_count == 1u);
                    CHECK(block->capture_count == 1u
                          && block->captures[0].kind == ST_AOT_CAPTURE_CELL);
                    CHECK(body->capture_count == 2u
                          && body->captures[0].kind == ST_AOT_CAPTURE_CELL
                          && body->captures[1].kind == ST_AOT_CAPTURE_CELL);
                }
            }
        }
        st_selector_id_t yourself_selector = 0u;
        CHECK(st_selector_lookup(&send_selectors, "yourself",
                                 sizeof("yourself") - 1u,
                                 &yourself_selector));
        const st_class_graph_method_t *while_true_method = find_method(
            &graph, "Block", "whileTrue:");
        const st_primitive_binding_t *while_true_binding =
            find_primitive_binding(
                &primitive_bindings,
                while_true_method != NULL ? while_true_method->node : NULL);
        compiled_t while_true_code;
        CHECK(while_true_binding != NULL
              && while_true_binding->primitive->implementation_kind
                    == ST_PRIMITIVE_RUNTIME_SYMBOL);
        bool while_true_compiled = compile_method_with_global(
            ctx, &graph, "Block", "whileTrue:", "st_Block_whileTrue",
            &send_selectors, while_true_binding, "Error", 0u,
            &while_true_code);
        CHECK(while_true_compiled);
        if (while_true_compiled) {
            CHECK(strstr(
                while_true_code.assembly,
                "st_aot_block_while_true_primitive_execute") != NULL);
        }
        if (closures_compiled && while_true_compiled)
            execute_x86_closures(
                closure_methods, &while_true_code,
                yourself_selector, while_true_id);
        compiled_destroy(&while_true_code);
        for (size_t index = 0u; index < CLOSURE_METHOD_COUNT; index++)
            compiled_destroy(&closure_methods[index]);

        const char *general_closures[2] = {
            "closureCell", "closureNested:"
        };
        for (size_t index = 0u; index < 2u; index++) {
            const st_class_graph_method_t *method = find_method(
                &graph, "Probe", general_closures[index]);
            st_sema_result_t sema;
            st_lower_result_t rejected;
            st_lower_options_t options = {
                .symbol_name = "st_rejected_closure",
                .linkage = ANVIL_LINK_EXTERNAL,
                .selectors = &send_selectors
            };
            CHECK(analyze(&graph, method, &sema));
            st_lower_result_init(&rejected);
            CHECK(st_lower_method(&rejected, ctx, &graph, method->id,
                                  &sema, &options)
                  == ST_LOWER_OK);
            CHECK(rejected.module != NULL && rejected.function != NULL
                  && rejected.block_count == index + 1u
                  && rejected.blocks != NULL);
            for (size_t block_index = 0u;
                 block_index < rejected.block_count; block_index++)
                CHECK(rejected.blocks[block_index].lexical_ordinal
                      == block_index);
            st_lower_result_destroy(&rejected);
            st_sema_result_destroy(&sema);
        }
        compiled_t dynamic_closure_send;
        CHECK(compile_method(ctx, &graph, "Probe", "dynamicValue:",
                             "st_Probe_dynamicValue", &send_selectors,
                             NULL, &dynamic_closure_send));
        if (dynamic_closure_send.result.module) {
            CHECK(dynamic_closure_send.result.block_count == 0u);
            CHECK(dynamic_closure_send.result.send_site_count == 1u);
        }
        compiled_destroy(&dynamic_closure_send);

        compiled_t nested_send;
        CHECK(compile_method(ctx, &graph, "Probe", "nestedSend",
                             "st_Probe_nestedSend", &send_selectors,
                             NULL, &nested_send));
        if (nested_send.result.module != NULL) {
            CHECK(nested_send.result.send_site_count == 3u);
            CHECK(native_assemble(nested_send.assembly));
        }
        compiled_destroy(&nested_send);

        struct {
            const char *class_name;
            const char *selector;
            const char *symbol;
        } primitive_cases[4] = {
            { "Object", "=", "st_Object_identity_primitive" },
            { "Character", "value", "st_Character_value_primitive" },
            { "Probe", "primitiveAdd:", "st_Probe_primitiveAdd" },
            { "Probe", "primitiveDivide:", "st_Probe_primitiveDivide" }
        };
        compiled_t primitive_methods[5];
        memset(primitive_methods, 0, sizeof(primitive_methods));
        for (size_t index = 0u; index < 4u; index++) {
            const st_class_graph_method_t *method = find_method(
                &graph, primitive_cases[index].class_name,
                primitive_cases[index].selector);
            const st_primitive_binding_t *binding = find_primitive_binding(
                &primitive_bindings, method ? method->node : NULL);
            CHECK(binding != NULL);
            CHECK(compile_method(
                ctx, &graph, primitive_cases[index].class_name,
                primitive_cases[index].selector,
                primitive_cases[index].symbol, &send_selectors, binding,
                &primitive_methods[index]));
            if (primitive_methods[index].result.module) {
                CHECK(primitive_methods[index].result.has_primitive);
                CHECK(primitive_methods[index].result.primitive_intrinsic_id
                      == binding->primitive->intrinsic_id);
                CHECK(primitive_methods[index].result.primitive_failure_policy
                      == binding->primitive->failure_policy);
            }
        }
        {
            const st_class_graph_method_t *identity_method = find_method(
                &graph, "Object", "=");
            const st_class_graph_method_t *character_method = find_method(
                &graph, "Character", "value");
            const st_primitive_binding_t *wrong_binding =
                find_primitive_binding(
                    &primitive_bindings,
                    identity_method ? identity_method->node : NULL);
            st_sema_result_t sema;
            st_lower_result_t invalid;
            st_lower_options_t options = {
                .symbol_name = "st_invalid_primitive_binding",
                .linkage = ANVIL_LINK_EXTERNAL,
                .primitive_binding = wrong_binding
            };
            CHECK(analyze(&graph, character_method, &sema));
            st_lower_result_init(&invalid);
            CHECK(st_lower_method(&invalid, ctx, &graph,
                                  character_method->id, &sema, &options)
                  == ST_LOWER_ERR_INVALID_ARGUMENT);
            CHECK(invalid.module == NULL && invalid.function == NULL);
            CHECK(invalid.diagnostic.code == ST_LOWER_DIAG_INVALID_INPUT);
            st_lower_result_destroy(&invalid);
            st_sema_result_destroy(&sema);
        }
        const st_class_graph_method_t *negated = find_method(
            &graph, "SmallInteger", "negated");
        const st_primitive_binding_t *negated_binding =
            find_primitive_binding(&primitive_bindings,
                                   negated ? negated->node : NULL);
        CHECK(negated_binding != NULL);
        CHECK(compile_method(ctx, &graph, "SmallInteger", "negated",
                             "st_SmallInteger_negated_primitive",
                             &send_selectors, negated_binding,
                             &primitive_methods[4]));
        if (primitive_methods[4].result.module) {
            CHECK(primitive_methods[4].result.has_primitive);
            CHECK(primitive_methods[4].result.send_site_count == 1u);
            CHECK(primitive_methods[4].result.root_map_count == 2u);
        }
        if (primitive_methods[0].result.module
                && primitive_methods[1].result.module
                && primitive_methods[2].result.module
                && primitive_methods[3].result.module
                && primitive_methods[4].result.module)
            execute_x86_primitives(primitive_methods);
        for (size_t index = 0u; index < 5u; index++)
            compiled_destroy(&primitive_methods[index]);

        {
            const st_class_graph_method_t *stream_method = find_class_method(
                &graph, "Probe", "streamWrite:next:from:");
            const st_primitive_binding_t *stream_binding =
                find_primitive_binding(
                    &primitive_bindings,
                    stream_method ? stream_method->node : NULL);
            compiled_t stream_method_code;
            CHECK(stream_binding != NULL
                  && stream_binding->primitive->implementation_kind
                    == ST_PRIMITIVE_RUNTIME_SYMBOL);
            CHECK(compile_method(
                ctx, &graph, "Probe", "streamWrite:next:from:",
                "st_Probe_streamWrite", &send_selectors, stream_binding,
                &stream_method_code));
            if (stream_method_code.result.module != NULL) {
                CHECK(stream_method_code.result.has_primitive);
                CHECK(stream_method_code.result.primitive_intrinsic_id
                      == ST_PRIMITIVE_INVALID_INTRINSIC_ID);
                CHECK(stream_method_code.result.primitive_failure_policy
                      == ST_PRIMITIVE_FALL_THROUGH);
                CHECK(stream_method_code.result.safepoint_count == 1u);
                CHECK(stream_method_code.result.root_map_count == 1u);
                CHECK(stream_method_code.result.required_root_capacity == 4u);
                CHECK(strstr(stream_method_code.assembly,
                             "st_aot_stream_write_primitive_execute")
                      != NULL);
                CHECK(strstr(stream_method_code.assembly,
                             "st_stream_write_primitive_execute") == NULL);
            }
            compiled_destroy(&stream_method_code);
        }

        {
            const st_class_graph_method_t *symbol_method = find_method(
                &graph, "Probe", "asSymbolRuntime");
            const st_primitive_binding_t *symbol_binding =
                find_primitive_binding(
                    &primitive_bindings,
                    symbol_method ? symbol_method->node : NULL);
            compiled_t symbol_method_code;
            CHECK(symbol_binding != NULL
                  && symbol_binding->primitive->implementation_kind
                    == ST_PRIMITIVE_RUNTIME_SYMBOL);
            CHECK(symbol_binding != NULL
                  && symbol_binding->primitive->intrinsic_id
                    == ST_PRIMITIVE_INVALID_INTRINSIC_ID);
            CHECK(compile_method(
                ctx, &graph, "Probe", "asSymbolRuntime",
                "st_Probe_asSymbolRuntime", &send_selectors, symbol_binding,
                &symbol_method_code));
            if (symbol_method_code.result.module != NULL) {
                CHECK(symbol_method_code.result.has_primitive);
                CHECK(symbol_method_code.result.primitive_intrinsic_id
                      == ST_PRIMITIVE_INVALID_INTRINSIC_ID);
                CHECK(symbol_method_code.result.primitive_failure_policy
                      == ST_PRIMITIVE_FALL_THROUGH);
                CHECK(symbol_method_code.result.safepoint_count == 1u);
                CHECK(symbol_method_code.result.root_map_count == 1u);
                CHECK(symbol_method_code.result.required_root_capacity == 1u);
                CHECK(strstr(
                    symbol_method_code.assembly,
                    "st_aot_string_as_symbol_primitive_execute") != NULL);
            }
            compiled_destroy(&symbol_method_code);
        }

        {
            static const char *const selectors[NUMERIC_AOT_METHOD_COUNT] = {
                "numericBinary:with:",
                "numericCompare:",
                "numericShift:",
                "numericLargeFloat",
                "numericSmallFloat",
                "numericHash",
                "floatAdd:",
                "floatRounded",
                "floatHash"
            };
            static const char *const symbols[NUMERIC_AOT_METHOD_COUNT] = {
                "st_Probe_numericBinary",
                "st_Probe_numericCompare",
                "st_Probe_numericShift",
                "st_Probe_numericLargeFloat",
                "st_Probe_numericSmallFloat",
                "st_Probe_numericHash",
                "st_Probe_floatAdd",
                "st_Probe_floatRounded",
                "st_Probe_floatHash"
            };
            static const char *const bridge_symbols[
                NUMERIC_AOT_METHOD_COUNT] = {
                "st_aot_large_integer_binary_primitive_execute",
                "st_aot_large_integer_compare_primitive_execute",
                "st_aot_large_integer_shift_primitive_execute",
                "st_aot_large_integer_as_float_primitive_execute",
                "st_aot_small_integer_as_float_primitive_execute",
                "st_aot_integer_hash_primitive_execute",
                "st_aot_float_add_primitive_execute",
                "st_aot_float_rounded_primitive_execute",
                "st_aot_float_hash_primitive_execute"
            };
            compiled_t numeric_methods[NUMERIC_AOT_METHOD_COUNT];
            bool numeric_ready = true;

            memset(numeric_methods, 0, sizeof(numeric_methods));
            for (size_t index = 0u;
                 index < NUMERIC_AOT_METHOD_COUNT; index++) {
                const st_class_graph_method_t *method = find_method(
                    &graph, "Probe", selectors[index]);
                const st_primitive_binding_t *binding =
                    find_primitive_binding(
                        &primitive_bindings,
                        method != NULL ? method->node : NULL);
                CHECK(binding != NULL
                      && binding->primitive->implementation_kind
                        == ST_PRIMITIVE_RUNTIME_SYMBOL
                      && binding->primitive->intrinsic_id
                        == ST_PRIMITIVE_INVALID_INTRINSIC_ID);
                numeric_ready = compile_method(
                    ctx, &graph, "Probe", selectors[index], symbols[index],
                    &send_selectors, binding, &numeric_methods[index])
                    && numeric_ready;
                if (numeric_methods[index].result.module != NULL) {
                    CHECK(numeric_methods[index].result.has_primitive);
                    CHECK(numeric_methods[index].result.safepoint_count >= 1u);
                    CHECK(strstr(
                        numeric_methods[index].assembly,
                        bridge_symbols[index]) != NULL);
                }
            }
            if (numeric_ready)
                execute_x86_numeric_primitives(numeric_methods);
            for (size_t index = 0u;
                 index < NUMERIC_AOT_METHOD_COUNT; index++)
                compiled_destroy(&numeric_methods[index]);
        }

        {
            static const char *const selectors[3] = {
                "blockValue0", "blockValue1:", "blockWhileTrue:"
            };
            static const char *const symbols[3] = {
                "st_Probe_blockValue0", "st_Probe_blockValue1",
                "st_Probe_blockWhileTrue"
            };
            static const char *const bridge_symbols[3] = {
                "st_aot_block_value_primitive_execute",
                "st_aot_block_value_primitive_1_execute",
                "st_aot_block_while_true_primitive_execute"
            };
            compiled_t block_methods[3];
            memset(block_methods, 0, sizeof(block_methods));
            bool block_methods_ready = true;
            for (size_t index = 0u; index < 3u; ++index) {
                const st_class_graph_method_t *method = find_method(
                    &graph, "Probe", selectors[index]);
                const st_primitive_binding_t *binding =
                    find_primitive_binding(
                        &primitive_bindings,
                        method != NULL ? method->node : NULL);
                CHECK(binding != NULL
                      && binding->primitive->implementation_kind
                        == ST_PRIMITIVE_RUNTIME_SYMBOL
                      && binding->primitive->intrinsic_id
                        == ST_PRIMITIVE_INVALID_INTRINSIC_ID);
                block_methods_ready = compile_method(
                    ctx, &graph, "Probe", selectors[index], symbols[index],
                    &send_selectors, binding, &block_methods[index])
                    && block_methods_ready;
                if (block_methods[index].result.module != NULL) {
                    CHECK(strstr(
                        block_methods[index].assembly,
                        bridge_symbols[index])
                          != NULL);
                    CHECK(block_methods[index].result.safepoint_count >= 1u);
                }
            }
            if (block_methods_ready)
                execute_x86_block_primitives(block_methods);
            for (size_t index = 0u; index < 3u; ++index)
                compiled_destroy(&block_methods[index]);
        }

        static const char *const heap_classes[11] = {
            "Object", "Probe", "Probe", "Probe", "Probe", "Behavior",
            "Probe", "Object", "Object", "Probe", "String"
        };
        static const char *const heap_selectors[11] = {
            "size", "heapAt:", "heapAt:put:", "heapInstVarAt:",
            "heapInstVarAt:put:", "new", "heapNew:", "class", "hash",
            "heapStringEquals:", "hash"
        };
        static const char *const heap_symbols[11] = {
            "st_Probe_heapSize", "st_Probe_heapAt", "st_Probe_heapAtPut",
            "st_Probe_heapIvarAt", "st_Probe_heapIvarAtPut",
            "st_Probe_heapNew", "st_Probe_heapNewSize", "st_Probe_heapClass",
            "st_Probe_heapHash", "st_Probe_heapStringEquals",
            "st_String_hash_primitive"
        };
        compiled_t heap_methods[11];
        memset(heap_methods, 0, sizeof(heap_methods));
        for (size_t index = 0u; index < 11u; index++) {
            const st_class_graph_method_t *method = find_method(
                &graph, heap_classes[index], heap_selectors[index]);
            const st_primitive_binding_t *binding = find_primitive_binding(
                &primitive_bindings, method ? method->node : NULL);
            CHECK(binding != NULL);
            CHECK(compile_method(ctx, &graph, heap_classes[index],
                                 heap_selectors[index],
                                 heap_symbols[index], &send_selectors,
                                 binding, &heap_methods[index]));
            if (heap_methods[index].result.module) {
                CHECK(heap_methods[index].result.safepoint_count == 1u);
                CHECK(heap_methods[index].result.root_map_count == 1u);
                CHECK(heap_methods[index].result.required_root_capacity
                      == method->node->as.method.arguments.count + 1u);
            }
        }
        bool all_heap = true;
        for (size_t index=0u;index<11u;index++)
            all_heap = all_heap && heap_methods[index].result.module != NULL;
        if (all_heap) execute_x86_heap_primitives(heap_methods);
        for (size_t index=0u;index<11u;index++)
            compiled_destroy(&heap_methods[index]);

        {
            static const char *const selectors[3] = {
                "ivarRead", "ivarWrite:", "ivarSecond:"
            };
            static const char *const symbols[3] = {
                "st_Probe_ivarRead",
                "st_Probe_ivarWrite",
                "st_Probe_ivarSecond"
            };
            compiled_t ivar_methods[3];
            bool ivar_ready = true;

            memset(ivar_methods, 0, sizeof(ivar_methods));
            for (size_t index = 0u; index < 3u; index++) {
                ivar_ready = compile_method(
                    ctx, &graph, "Probe", selectors[index], symbols[index],
                    &send_selectors, NULL, &ivar_methods[index])
                    && ivar_ready;
                if (ivar_methods[index].result.module != NULL) {
                    CHECK(strstr(
                        ivar_methods[index].assembly,
                        "st_aot_heap_primitive_execute") != NULL);
                    CHECK(strstr(
                        ivar_methods[index].assembly,
                        "st_aot_heap_primitive_contract_violation") != NULL);
                    CHECK(ivar_methods[index].result.safepoint_count == 0u);
                }
            }
            if (ivar_ready) {
                execute_x86_instance_variables(ivar_methods);
            }
            for (size_t index = 0u; index < 3u; index++) {
                compiled_destroy(&ivar_methods[index]);
            }

            compiled_t closure_ivar;
            CHECK(compile_method(
                ctx, &graph, "Probe", "ivarClosureRead",
                "st_Probe_ivarClosureRead", &send_selectors, NULL,
                &closure_ivar));
            if (closure_ivar.result.module != NULL) {
                CHECK(closure_ivar.result.block_count == 1u);
                CHECK(closure_ivar.result.blocks != NULL
                      && closure_ivar.result.blocks[0].capture_count == 1u
                      && closure_ivar.result.blocks[0].captures[0].kind
                          == ST_AOT_CAPTURE_SELF);
                CHECK(strstr(
                    closure_ivar.assembly,
                    "st_aot_heap_primitive_execute") != NULL);
            }
            compiled_destroy(&closure_ivar);
        }
    }
    anvil_ctx_destroy(ctx);

    ctx = anvil_ctx_create_for_target(ANVIL_ARCH_ARM64);
    CHECK(ctx != NULL);
    if (ctx) {
        compiled_t arm;
        CHECK(compile_method(ctx, &graph, "Probe", "general",
                             "st_Probe_general", &send_selectors, NULL,
                             &arm));
        if (arm.result.module) CHECK(cross_assemble(arm.assembly));
        compiled_destroy(&arm);
        CHECK(compile_method(ctx, &graph, "Probe", "branchReturn",
                             "st_Probe_branchReturn", &send_selectors,
                             NULL, &arm));
        if (arm.result.module) {
            CHECK((arm.result.method_flags
                   & (ST_METHOD_CAN_UNWIND
                      | ST_METHOD_HAS_NON_LOCAL_RETURN))
                  == (ST_METHOD_CAN_UNWIND
                      | ST_METHOD_HAS_NON_LOCAL_RETURN));
            CHECK(cross_assemble(arm.assembly));
        }
        compiled_destroy(&arm);
        CHECK(compile_method(ctx, &graph, "Probe", "closureNlr",
                             "st_Probe_closureNlr", &send_selectors,
                             NULL, &arm));
        if (arm.result.module) {
            CHECK(arm.result.block_count == 1u);
            CHECK(arm.result.blocks != NULL
                  && (arm.result.blocks[0].flags & ST_AOT_BLOCK_HAS_HOME));
            CHECK(cross_assemble(arm.assembly));
        }
        compiled_destroy(&arm);
        static const char *const general_block_selectors[5] = {
            "closureArity3", "closureNestedCall:",
            "closureCellSiblings", "closureSend", "closureWhileLoop"
        };
        static const char *const general_block_symbols[5] = {
            "st_Probe_closureArity3", "st_Probe_closureNestedCall",
            "st_Probe_closureCellSiblings", "st_Probe_closureSend",
            "st_Probe_closureWhileLoop"
        };
        for (size_t index = 0u; index < 5u; index++) {
            CHECK(compile_method(
                ctx, &graph, "Probe", general_block_selectors[index],
                general_block_symbols[index], &send_selectors, NULL, &arm));
            if (arm.result.module) {
                CHECK(arm.result.block_count
                      == (index == 1u || index == 2u || index == 4u
                          ? 2u : 1u));
                CHECK(cross_assemble(arm.assembly));
            }
            compiled_destroy(&arm);
        }
        CHECK(compile_method(
            ctx, &graph, "Probe", "ivarWrite:",
            "st_Probe_ivarWrite_arm64", &send_selectors, NULL, &arm));
        if (arm.result.module != NULL) {
            CHECK(strstr(
                arm.assembly, "st_aot_heap_primitive_execute") != NULL);
            CHECK(cross_assemble(arm.assembly));
        }
        compiled_destroy(&arm);
        CHECK(compile_method(
            ctx, &graph, "Probe", "ivarClosureRead",
            "st_Probe_ivarClosureRead_arm64", &send_selectors, NULL, &arm));
        if (arm.result.module != NULL) {
            CHECK(arm.result.block_count == 1u);
            CHECK(strstr(
                arm.assembly, "st_aot_heap_primitive_execute") != NULL);
            CHECK(cross_assemble(arm.assembly));
        }
        compiled_destroy(&arm);
        CHECK(compile_method(ctx, &graph, "Probe", "cascade",
                             "st_Probe_cascade", &send_selectors,
                             NULL, &arm));
        if (arm.result.module) {
            CHECK(arm.result.send_site_count == 2u);
            CHECK(cross_assemble(arm.assembly));
        }
        compiled_destroy(&arm);
        CHECK(compile_method(ctx, &graph, "Probe", "nestedSend",
                             "st_Probe_nestedSend", &send_selectors,
                             NULL, &arm));
        if (arm.result.module != NULL) {
            CHECK(arm.result.send_site_count == 3u);
            CHECK(cross_assemble(arm.assembly));
        }
        compiled_destroy(&arm);
        CHECK(compile_method(ctx, &graph, "Probe", "stringLiteral",
                             "st_Probe_stringLiteral", &send_selectors,
                             NULL, &arm));
        if (arm.result.module) {
            CHECK(arm.result.string_literal_count == 1u);
            CHECK(cross_assemble(arm.assembly));
        }
        compiled_destroy(&arm);
        const st_class_graph_method_t *primitive_method = find_method(
            &graph, "Probe", "primitiveDivide:");
        const st_primitive_binding_t *binding = find_primitive_binding(
            &primitive_bindings,
            primitive_method ? primitive_method->node : NULL);
        CHECK(compile_method(ctx, &graph, "Probe", "primitiveDivide:",
                             "st_Probe_primitiveDivide", &send_selectors,
                             binding, &arm));
        if (arm.result.module) CHECK(cross_assemble(arm.assembly));
        compiled_destroy(&arm);
        primitive_method = find_method(&graph, "SmallInteger", "negated");
        binding = find_primitive_binding(
            &primitive_bindings,
            primitive_method ? primitive_method->node : NULL);
        CHECK(compile_method(ctx, &graph, "SmallInteger", "negated",
                             "st_SmallInteger_negated_primitive",
                             &send_selectors, binding, &arm));
        if (arm.result.module) CHECK(cross_assemble(arm.assembly));
        compiled_destroy(&arm);
        primitive_method = find_method(&graph, "Probe", "heapAt:put:");
        binding = find_primitive_binding(
            &primitive_bindings,
            primitive_method ? primitive_method->node : NULL);
        CHECK(compile_method(ctx, &graph, "Probe", "heapAt:put:",
                             "st_Probe_heapAtPut", &send_selectors,
                             binding, &arm));
        if (arm.result.module) CHECK(cross_assemble(arm.assembly));
        compiled_destroy(&arm);
        primitive_method = find_method(&graph, "String", "hash");
        binding = find_primitive_binding(
            &primitive_bindings,
            primitive_method ? primitive_method->node : NULL);
        CHECK(compile_method(ctx, &graph, "String", "hash",
                             "st_String_hash_primitive", &send_selectors,
                             binding, &arm));
        if (arm.result.module) CHECK(cross_assemble(arm.assembly));
        compiled_destroy(&arm);
        primitive_method = find_class_method(
            &graph, "Probe", "streamWrite:next:from:");
        binding = find_primitive_binding(
            &primitive_bindings,
            primitive_method ? primitive_method->node : NULL);
        CHECK(compile_method(
            ctx, &graph, "Probe", "streamWrite:next:from:",
            "st_Probe_streamWrite", &send_selectors, binding, &arm));
        if (arm.result.module) CHECK(cross_assemble(arm.assembly));
        compiled_destroy(&arm);
        primitive_method = find_method(&graph, "Probe", "blockValue1:");
        binding = find_primitive_binding(
            &primitive_bindings,
            primitive_method != NULL ? primitive_method->node : NULL);
        CHECK(compile_method(
            ctx, &graph, "Probe", "blockValue1:",
            "st_Probe_blockValue1", &send_selectors, binding, &arm));
        if (arm.result.module) {
            CHECK(strstr(
                arm.assembly,
                "st_aot_block_value_primitive_1_execute") != NULL);
            CHECK(cross_assemble(arm.assembly));
        }
        compiled_destroy(&arm);
        primitive_method = find_method(
            &graph, "Probe", "blockWhileTrue:");
        binding = find_primitive_binding(
            &primitive_bindings,
            primitive_method != NULL ? primitive_method->node : NULL);
        CHECK(compile_method(
            ctx, &graph, "Probe", "blockWhileTrue:",
            "st_Probe_blockWhileTrue", &send_selectors, binding, &arm));
        if (arm.result.module) {
            CHECK(strstr(
                arm.assembly,
                "st_aot_block_while_true_primitive_execute") != NULL);
            CHECK(cross_assemble(arm.assembly));
        }
        compiled_destroy(&arm);

        {
            static const char *const selectors[NUMERIC_AOT_METHOD_COUNT] = {
                "numericBinary:with:",
                "numericCompare:",
                "numericShift:",
                "numericLargeFloat",
                "numericSmallFloat",
                "numericHash",
                "floatAdd:",
                "floatRounded",
                "floatHash"
            };
            static const char *const symbols[NUMERIC_AOT_METHOD_COUNT] = {
                "st_Probe_numericBinary_arm64",
                "st_Probe_numericCompare_arm64",
                "st_Probe_numericShift_arm64",
                "st_Probe_numericLargeFloat_arm64",
                "st_Probe_numericSmallFloat_arm64",
                "st_Probe_numericHash_arm64",
                "st_Probe_floatAdd_arm64",
                "st_Probe_floatRounded_arm64",
                "st_Probe_floatHash_arm64"
            };
            static const char *const bridge_symbols[
                NUMERIC_AOT_METHOD_COUNT] = {
                "st_aot_large_integer_binary_primitive_execute",
                "st_aot_large_integer_compare_primitive_execute",
                "st_aot_large_integer_shift_primitive_execute",
                "st_aot_large_integer_as_float_primitive_execute",
                "st_aot_small_integer_as_float_primitive_execute",
                "st_aot_integer_hash_primitive_execute",
                "st_aot_float_add_primitive_execute",
                "st_aot_float_rounded_primitive_execute",
                "st_aot_float_hash_primitive_execute"
            };
            for (size_t index = 0u;
                 index < NUMERIC_AOT_METHOD_COUNT; index++) {
                const st_class_graph_method_t *method = find_method(
                    &graph, "Probe", selectors[index]);
                const st_primitive_binding_t *numeric_binding =
                    find_primitive_binding(
                        &primitive_bindings,
                        method != NULL ? method->node : NULL);
                CHECK(numeric_binding != NULL);
                CHECK(compile_method(
                    ctx, &graph, "Probe", selectors[index], symbols[index],
                    &send_selectors, numeric_binding, &arm));
                if (arm.result.module != NULL) {
                    CHECK(strstr(
                        arm.assembly, bridge_symbols[index]) != NULL);
                    CHECK(cross_assemble(arm.assembly));
                }
                compiled_destroy(&arm);
            }
        }
        anvil_ctx_destroy(ctx);
    }

    ctx = anvil_ctx_create_for_target(ANVIL_ARCH_ZARCH);
    CHECK(ctx != NULL);
    if (ctx) {
        compiled_t zarch;
        CHECK(compile_method(ctx, &graph, "Probe", "selfSend",
                             "st_Probe_selfSend", &send_selectors,
                             NULL, &zarch));
        if (zarch.result.module) {
            CHECK(zarch.result.send_site_count == 1u);
            CHECK(zarch.assembly != NULL && zarch.assembly_length != 0u);
        }
        compiled_destroy(&zarch);
        anvil_ctx_destroy(ctx);
    }

    test_diagnostics_and_transactions(&graph, &send_selectors);
    test_primitive_fault_transactions(&graph, &primitive_bindings,
                                      &send_selectors);
    test_closure_fault_transactions(&graph, &send_selectors);
    test_literal_fault_transactions(&graph);
    st_selector_table_destroy(&send_selectors);
    st_primitive_result_destroy(&primitive_bindings);
    st_primitive_catalog_destroy(&primitive_catalog);
    st_class_graph_result_destroy(&graph);
done:
    for (size_t index = initialized_count; index > 0u; index--)
        fixture_destroy(&fixtures[index - 1u]);
    if (failures) {
        fprintf(stderr, "%u Smalltalk AOT lowering regression(s) failed\n",
                failures);
        return 1;
    }
    puts("smalltalk AOT lowering: PASS (x86_64 closures/NLR/GC, ARM64 assembly, ZARCH sends, transactional OOM)");
    return 0;
}
