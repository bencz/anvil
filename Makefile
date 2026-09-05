# ANVIL - Makefile
#
# Build the ANVIL library and examples

CC = gcc
AR = ar
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -I./include -g -O2
DEPFLAGS = -MMD -MP
LDFLAGS = -L./lib
ARFLAGS = rcs

# Directories
SRC_DIR = src
BUILD_DIR = build
LIB_DIR = lib
INCLUDE_DIR = include
EXAMPLES_DIR = examples

# Library name
LIB_NAME = libanvil.a
LIB_PATH = $(LIB_DIR)/$(LIB_NAME)

# Host platform implementation (independent of the generated target).
HOST_PLATFORM ?= posix
ifeq ($(OS),Windows_NT)
HOST_PLATFORM = windows
endif

# Source files
CORE_SRCS = \
	$(SRC_DIR)/core/atomic.c \
	$(SRC_DIR)/core/context.c \
	$(SRC_DIR)/core/cpu_table.c \
	$(SRC_DIR)/core/types.c \
	$(SRC_DIR)/core/module.c \
	$(SRC_DIR)/core/function.c \
	$(SRC_DIR)/core/value.c \
	$(SRC_DIR)/core/builder.c \
	$(SRC_DIR)/core/strbuf.c \
	$(SRC_DIR)/core/backend.c \
	$(SRC_DIR)/core/ir_dump.c \
	$(SRC_DIR)/core/verify.c

BACKEND_SRCS = \
	$(SRC_DIR)/backend/common/gnu_data.c \
	$(SRC_DIR)/backend/x86/targets/x86_32.c \
	$(SRC_DIR)/backend/x86/targets/x86_64.c \
	$(SRC_DIR)/backend/x86/x86_32_helpers.c \
	$(SRC_DIR)/backend/x86/x86_64_helpers.c \
	$(SRC_DIR)/backend/x86/x86_32_lower.c \
	$(SRC_DIR)/backend/x86/x86_64_lower.c \
	$(SRC_DIR)/backend/x86/x86_32_legal.c \
	$(SRC_DIR)/backend/x86/x86_64_legal.c \
	$(SRC_DIR)/backend/x86/x86_32_codegen.c \
	$(SRC_DIR)/backend/x86/x86_64_codegen.c \
	$(SRC_DIR)/backend/x86/abi/x86_32_abi.c \
	$(SRC_DIR)/backend/x86/abi/x86_64_abi.c \
	$(SRC_DIR)/backend/x86/abi/x86_64_varargs.c \
	$(SRC_DIR)/backend/x86/asm/gas32.c \
	$(SRC_DIR)/backend/x86/asm/gas64.c \
	$(SRC_DIR)/backend/systemz/systemz_target.c \
	$(SRC_DIR)/backend/systemz/systemz_lower.c \
	$(SRC_DIR)/backend/systemz/systemz_legal.c \
	$(SRC_DIR)/backend/systemz/systemz_codegen.c \
	$(SRC_DIR)/backend/systemz/abi/mvs_arena_31.c \
	$(SRC_DIR)/backend/systemz/abi/mvs_arena_64.c \
	$(SRC_DIR)/backend/systemz/asm/hlasm.c \
	$(SRC_DIR)/backend/systemz/asm/hlasm_data.c \
	$(SRC_DIR)/backend/systemz/asm/hlasm_names.c \
	$(SRC_DIR)/backend/systemz/asm/hlasm_dispatch.c \
	$(SRC_DIR)/backend/systemz/targets/s370.c \
	$(SRC_DIR)/backend/systemz/targets/s370_xa.c \
	$(SRC_DIR)/backend/systemz/targets/s390.c \
	$(SRC_DIR)/backend/systemz/targets/zarch.c \
	$(SRC_DIR)/backend/ppc/ppc_target.c \
	$(SRC_DIR)/backend/ppc/ppc_lower.c \
	$(SRC_DIR)/backend/ppc/ppc_legal.c \
	$(SRC_DIR)/backend/ppc/ppc_emit.c \
	$(SRC_DIR)/backend/ppc/ppc_codegen.c \
	$(SRC_DIR)/backend/ppc/abi/elf32.c \
	$(SRC_DIR)/backend/ppc/abi/elfv1.c \
	$(SRC_DIR)/backend/ppc/abi/elfv2.c \
	$(SRC_DIR)/backend/ppc/targets/ppc32.c \
	$(SRC_DIR)/backend/ppc/targets/ppc64.c \
	$(SRC_DIR)/backend/ppc/targets/ppc64le.c \
	$(SRC_DIR)/backend/arm64/arm64.c \
	$(SRC_DIR)/backend/arm64/arm64_helpers.c \
	$(SRC_DIR)/backend/arm64/arm64_varargs.c \
	$(SRC_DIR)/backend/arm64/arm64_mir.c

ANALYSIS_SRCS = \
	$(SRC_DIR)/analysis/loops.c \
	$(SRC_DIR)/analysis/dominance_frontier.c \
	$(SRC_DIR)/analysis/alias.c \
	$(SRC_DIR)/analysis/cfg.c \
	$(SRC_DIR)/analysis/def_use.c

OPT_SRCS = \
	$(SRC_DIR)/opt/call_order.c \
	$(SRC_DIR)/opt/inline.c \
	$(SRC_DIR)/opt/unroll.c \
	$(SRC_DIR)/opt/vectorize.c \
	$(SRC_DIR)/opt/mem2reg.c \
	$(SRC_DIR)/opt/sroa.c \
	$(SRC_DIR)/opt/licm.c \
	$(SRC_DIR)/opt/loop_utils.c \
	$(SRC_DIR)/opt/sccp.c \
	$(SRC_DIR)/opt/known_bits.c \
	$(SRC_DIR)/opt/opt.c \
	$(SRC_DIR)/opt/const_fold.c \
	$(SRC_DIR)/opt/dce.c \
	$(SRC_DIR)/opt/simplify_cfg.c \
	$(SRC_DIR)/opt/strength_reduce.c \
	$(SRC_DIR)/opt/copy_prop.c \
	$(SRC_DIR)/opt/dead_store.c \
	$(SRC_DIR)/opt/load_elim.c \
	$(SRC_DIR)/opt/cse.c \
	$(SRC_DIR)/opt/ctx_opt.c \
	$(SRC_DIR)/opt/store_load_prop.c

MACHINE_SRCS = \
	$(SRC_DIR)/machine/parallel_copy.c \
	$(SRC_DIR)/machine/liveness.c \
	$(SRC_DIR)/machine/verify_alloc.c \
	$(SRC_DIR)/machine/machine_ir.c \
	$(SRC_DIR)/machine/regalloc.c

PLATFORM_SRCS = \
	$(SRC_DIR)/platform/$(HOST_PLATFORM)/registry.c \
	$(SRC_DIR)/platform/$(HOST_PLATFORM)/stream.c

ALL_SRCS = $(PLATFORM_SRCS) $(CORE_SRCS) $(BACKEND_SRCS) $(ANALYSIS_SRCS) $(OPT_SRCS) $(MACHINE_SRCS)

# Object files
OBJS = $(ALL_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS = $(OBJS:.o=.d)

# Examples (all .c files in examples directory)
EXAMPLES = \
	$(BUILD_DIR)/examples/simple \
	$(BUILD_DIR)/examples/multiarch \
	$(BUILD_DIR)/examples/floating_point \
	$(BUILD_DIR)/examples/control_flow \
	$(BUILD_DIR)/examples/hello_world \
	$(BUILD_DIR)/examples/string_test \
	$(BUILD_DIR)/examples/array_test \
	$(BUILD_DIR)/examples/struct_test \
	$(BUILD_DIR)/examples/optimization_test \
	$(BUILD_DIR)/examples/memory_opt_test \
	$(BUILD_DIR)/examples/cse_test \
	$(BUILD_DIR)/examples/global_test \
	$(BUILD_DIR)/examples/cpu_model_test \
	$(BUILD_DIR)/examples/ir_dump_test

TESTS = \
	$(BUILD_DIR)/tests/global_memory_regression \
	$(BUILD_DIR)/tests/arithmetic_optimization_regression \
	$(BUILD_DIR)/tests/analysis_regression \
	$(BUILD_DIR)/tests/atomic_regression \
	$(BUILD_DIR)/tests/inline_regression \
	$(BUILD_DIR)/tests/unroll_regression \
	$(BUILD_DIR)/tests/interval_split_regression \
	$(BUILD_DIR)/tests/vector_regression \
	$(BUILD_DIR)/tests/loop_preheader_regression \
	$(BUILD_DIR)/tests/abi_plan_regression \
	$(BUILD_DIR)/tests/optimization_pipeline_regression \
	$(BUILD_DIR)/tests/core_arm64_regression \
	$(BUILD_DIR)/tests/ir_verifier_regression \
	$(BUILD_DIR)/tests/optimizer_regression \
	$(BUILD_DIR)/tests/machine_regalloc_regression \
	$(BUILD_DIR)/tests/arm64_mir_lowering_regression \
	$(BUILD_DIR)/tests/x86_64_mir_lowering_regression \
	$(BUILD_DIR)/tests/x86_mir_lowering_regression \
	$(BUILD_DIR)/tests/ppc_mir_lowering_regression \
	$(BUILD_DIR)/tests/backend_dispatch_regression \
	$(BUILD_DIR)/tests/mainframe_mir_lowering_regression \
	$(BUILD_DIR)/tests/fcmp_backend_regression \
	$(BUILD_DIR)/tests/typed_gep_backend_regression \
	$(BUILD_DIR)/tests/call_reloc_core_regression \
	$(BUILD_DIR)/tests/smalltalk_lexer_regression \
	$(BUILD_DIR)/tests/smalltalk_ast_regression \
	$(BUILD_DIR)/tests/smalltalk_parser_regression \
	$(BUILD_DIR)/tests/smalltalk_value_regression \
	$(BUILD_DIR)/tests/smalltalk_selector_regression \
	$(BUILD_DIR)/tests/smalltalk_sema_regression \
	$(BUILD_DIR)/tests/smalltalk_source_bundle_regression \
	$(BUILD_DIR)/tests/smalltalk_class_graph_regression \
	$(BUILD_DIR)/tests/smalltalk_image_layout_regression \
	$(BUILD_DIR)/tests/smalltalk_image_regression \
	$(BUILD_DIR)/tests/smalltalk_examples_regression \
	$(BUILD_DIR)/tests/smalltalk_primitive_regression \
	$(BUILD_DIR)/tests/smalltalk_runtime_regression \
	$(BUILD_DIR)/tests/smalltalk_core_primitives_regression \
	$(BUILD_DIR)/tests/smalltalk_lookup_regression \
	$(BUILD_DIR)/tests/smalltalk_send_bridge_regression \
	$(BUILD_DIR)/tests/smalltalk_image_runtime_regression \
	$(BUILD_DIR)/tests/smalltalk_primitive_bridge_regression \
	$(BUILD_DIR)/tests/smalltalk_heap_regression \
	$(BUILD_DIR)/tests/smalltalk_heap_primitives_regression \
	$(BUILD_DIR)/tests/smalltalk_float_primitives_regression \
	$(BUILD_DIR)/tests/smalltalk_integer_primitives_regression \
	$(BUILD_DIR)/tests/smalltalk_integer_primitive_bridge_regression \
	$(BUILD_DIR)/tests/smalltalk_stream_primitives_regression \
	$(BUILD_DIR)/tests/smalltalk_stream_primitive_bridge_regression \
	$(BUILD_DIR)/tests/smalltalk_string_primitives_regression \
	$(BUILD_DIR)/tests/smalltalk_string_primitive_bridge_regression \
	$(BUILD_DIR)/tests/smalltalk_symbol_intern_regression \
	$(BUILD_DIR)/tests/smalltalk_control_regression \
	$(BUILD_DIR)/tests/smalltalk_control_bridge_regression \
	$(BUILD_DIR)/tests/smalltalk_control_gc_regression \
	$(BUILD_DIR)/tests/smalltalk_closure_bridge_regression \
	$(BUILD_DIR)/tests/smalltalk_block_primitives_regression \
	$(BUILD_DIR)/tests/smalltalk_exception_primitives_regression \
	$(BUILD_DIR)/tests/smalltalk_reflection_primitives_regression \
	$(BUILD_DIR)/tests/smalltalk_reflection_lower_regression \
	$(BUILD_DIR)/tests/smalltalk_exception_lower_regression \
	$(BUILD_DIR)/tests/smalltalk_dnu_lower_regression \
	$(BUILD_DIR)/tests/smalltalk_lower_regression \
	$(BUILD_DIR)/tests/smalltalk_image_emit_regression \
	$(BUILD_DIR)/tests/smalltalk_aot_compile_regression \
	$(BUILD_DIR)/tests/smalltalk_artifact_bundle_regression \
	$(BUILD_DIR)/tests/smalltalk_artifact_materialize_regression \
	$(BUILD_DIR)/tests/smalltalk_application_materialize_regression \
	$(BUILD_DIR)/tests/smalltalk_application_aot_regression \
	$(BUILD_DIR)/tests/smalltalk_aot_toolchain_regression \
	$(BUILD_DIR)/tests/smalltalk_application_samples_regression \
	$(BUILD_DIR)/tests/smalltalk_dispatch_regression

.PHONY: all clean lib examples tests smalltalk-aotc smalltalk-aot-link \
	smalltalk-runtime smalltalk-app test-win64-abi test-fcmp-i1-runtime \
	test-sanitize test-valgrind install examples-runtime test-examples \
	clean-examples-runtime examples-advanced test-examples-advanced \
	clean-examples-advanced

all: lib examples

lib: $(LIB_PATH)

$(LIB_PATH): $(OBJS) Makefile
	@mkdir -p $(LIB_DIR)
	$(RM) $@.tmp
	$(AR) $(ARFLAGS) $@.tmp $(OBJS)
	mv -f $@.tmp $@
	@echo "Built $(LIB_PATH)"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

-include $(DEPS)

examples: lib $(EXAMPLES)

$(BUILD_DIR)/examples/%: $(EXAMPLES_DIR)/%.c $(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -lanvil
	@echo "Built $@"

tests: lib $(TESTS) $(BUILD_DIR)/tests/win64_abi_codegen $(BUILD_DIR)/tests/fcmp_i1_runtime_codegen
	@for test in $(TESTS); do \
		echo "Running $$test"; \
		$$test || exit 1; \
	done
	@BUILD_DIR=$(BUILD_DIR) bash tests/run_win64_abi.sh
	@BUILD_DIR=$(BUILD_DIR) bash tests/run_fcmp_i1_runtime.sh

# Build every object in an isolated tree so sanitizer flags cannot be mixed
# with the normal archive. LeakSanitizer is disabled here because it cannot
# operate under the ptrace sandbox; leak conformance has its own Valgrind gate.
test-sanitize:
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) CC=/usr/bin/clang \
		BUILD_DIR=build/sanitize LIB_DIR=lib/sanitize \
		CFLAGS="-Wall -Wextra -std=c11 -D_GNU_SOURCE -I./include -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined" \
		LDFLAGS="-L./lib/sanitize -fsanitize=address,undefined" tests

test-valgrind: lib $(TESTS)
	@for test in $(TESTS); do \
		echo "Valgrind $$test"; \
		valgrind --quiet --leak-check=full --show-leak-kinds=definite,indirect \
			--errors-for-leak-kinds=definite,indirect --error-exitcode=1 \
			$$test || exit 1; \
	done

test-win64-abi: $(BUILD_DIR)/tests/win64_abi_codegen
	BUILD_DIR=$(BUILD_DIR) bash tests/run_win64_abi.sh

test-fcmp-i1-runtime: $(BUILD_DIR)/tests/fcmp_i1_runtime_codegen
	BUILD_DIR=$(BUILD_DIR) bash tests/run_fcmp_i1_runtime.sh

$(BUILD_DIR)/tests/%: tests/%.c tests/platform/$(HOST_PLATFORM)/host.c tests/platform/host.h $(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< tests/platform/$(HOST_PLATFORM)/host.c -o $@ $(LDFLAGS) -lanvil
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_lexer_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/tests/lexer_test.c \
		samples/smalltalk/include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/tests/lexer_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_ast_regression: \
		samples/smalltalk/src/frontend/ast.c samples/smalltalk/tests/ast_test.c \
		samples/smalltalk/include/st_ast.h samples/smalltalk/include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/ast.c samples/smalltalk/tests/ast_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_parser_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/tests/parser_test.c \
		samples/smalltalk/include/st_lexer.h \
		samples/smalltalk/include/st_ast.h \
		samples/smalltalk/include/st_parser.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/tests/parser_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_value_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/tests/value_test.c \
		samples/smalltalk/include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/tests/value_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_selector_regression: \
		samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/tests/selector_test.c \
		samples/smalltalk/include/st_selector.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/tests/selector_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_sema_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/tests/sema_test.c \
		samples/smalltalk/include/st_lexer.h \
		samples/smalltalk/include/st_ast.h \
		samples/smalltalk/include/st_parser.h \
		samples/smalltalk/include/st_sema.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/tests/sema_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_source_bundle_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/tests/source_bundle_test.c \
		samples/smalltalk/include/st_lexer.h \
		samples/smalltalk/include/st_ast.h \
		samples/smalltalk/include/st_parser.h \
		samples/smalltalk/include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/tests/source_bundle_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_class_graph_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/tests/class_graph_test.c \
		samples/smalltalk/include/st_lexer.h \
		samples/smalltalk/include/st_ast.h \
		samples/smalltalk/include/st_parser.h \
		samples/smalltalk/include/st_sema.h \
		samples/smalltalk/include/st_class_graph.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/tests/class_graph_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_image_layout_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/src/compiler/image_layout.c \
		samples/smalltalk/tests/image_layout_test.c \
		samples/smalltalk/include/st_image_layout.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/src/compiler/image_layout.c \
		samples/smalltalk/tests/image_layout_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_image_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/tests/image_test.c \
		samples/smalltalk/include/st_source_bundle.h \
		samples/smalltalk/include/st_class_graph.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/tests/image_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_examples_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/tests/examples_test.c \
		samples/smalltalk/examples/hello/application.manifest \
		samples/smalltalk/examples/hello/HelloApplication.st \
		samples/smalltalk/examples/closures/application.manifest \
		samples/smalltalk/examples/closures/ClosuresApplication.st \
		samples/smalltalk/include/st_source_bundle.h \
		samples/smalltalk/include/st_class_graph.h \
		samples/smalltalk/include/st_sema.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/tests/examples_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_primitive_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/tests/primitive_test.c \
		samples/smalltalk/include/st_lexer.h \
		samples/smalltalk/include/st_ast.h \
		samples/smalltalk/include/st_parser.h \
		samples/smalltalk/include/st_source_bundle.h \
		samples/smalltalk/include/st_primitive.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/tests/primitive_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_runtime_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/tests/runtime_test.c \
		samples/smalltalk/include/st_value.h \
		samples/smalltalk/include/st_dispatch.h \
		samples/smalltalk/include/st_runtime.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/tests/runtime_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_core_primitives_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/tests/core_primitives_test.c \
		samples/smalltalk/include/st_value.h \
		samples/smalltalk/include/st_primitive.h \
		samples/smalltalk/include/st_core_primitives.h \
		samples/smalltalk/include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/tests/core_primitives_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_lookup_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c \
		samples/smalltalk/tests/lookup_test.c \
		samples/smalltalk/include/st_value.h \
		samples/smalltalk/include/st_runtime.h \
		samples/smalltalk/include/st_lookup.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c \
		samples/smalltalk/tests/lookup_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_send_bridge_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/tests/send_bridge_test.c \
		samples/smalltalk/include/st_value.h \
		samples/smalltalk/include/st_runtime.h \
		samples/smalltalk/include/st_lookup.h \
		samples/smalltalk/include/st_send_bridge.h \
		samples/smalltalk/include/st_control.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/tests/send_bridge_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_image_runtime_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/tests/image_runtime_test.c \
		samples/smalltalk/include/st_image_runtime.h \
		samples/smalltalk/include/st_send_bridge.h \
		samples/smalltalk/include/st_heap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/tests/image_runtime_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_primitive_bridge_regression: \
		samples/smalltalk/src/runtime/primitives/primitive_bridge.c \
		samples/smalltalk/tests/primitive_bridge_test.c \
		samples/smalltalk/include/st_primitive_bridge.h \
		samples/smalltalk/include/st_core_primitives.h \
		samples/smalltalk/include/st_dispatch.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/primitives/primitive_bridge.c \
		samples/smalltalk/tests/primitive_bridge_test.c \
		-o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_heap_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/tests/heap_test.c \
		samples/smalltalk/include/st_value.h \
		samples/smalltalk/include/st_runtime.h \
		samples/smalltalk/include/st_heap.h \
		samples/smalltalk/include/st_control.h \
		samples/smalltalk/include/st_control_roots.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/tests/heap_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_heap_primitives_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/tests/heap_primitives_test.c \
		samples/smalltalk/include/st_heap_primitives.h \
		samples/smalltalk/include/st_source_bundle.h \
		samples/smalltalk/include/st_control_roots.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/tests/heap_primitives_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_float_primitives_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/tests/float_primitives_test.c \
		samples/smalltalk/include/st_float_primitives.h \
		samples/smalltalk/include/st_source_bundle.h \
		samples/smalltalk/include/st_control_roots.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/tests/float_primitives_test.c \
		-o $@ $(LDFLAGS) -pthread -lm
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_integer_primitives_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/integer_primitives.c \
		samples/smalltalk/tests/integer_primitives_test.c \
		samples/smalltalk/tests/integer_differential_oracle.py \
		samples/smalltalk/src/runtime/primitives/float_primitives_internal.h \
		samples/smalltalk/include/st_integer_primitives.h \
		samples/smalltalk/include/st_float_primitives.h \
		samples/smalltalk/include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/integer_primitives.c \
		samples/smalltalk/tests/integer_primitives_test.c \
		-o $@ $(LDFLAGS) -pthread -lm
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_integer_primitive_bridge_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/integer_primitives.c \
		samples/smalltalk/src/runtime/primitives/integer_primitive_bridge.c \
		samples/smalltalk/tests/integer_primitive_bridge_test.c \
		samples/smalltalk/src/runtime/primitives/float_primitives_internal.h \
		samples/smalltalk/include/st_integer_primitives.h \
		samples/smalltalk/include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/integer_primitives.c \
		samples/smalltalk/src/runtime/primitives/integer_primitive_bridge.c \
		samples/smalltalk/tests/integer_primitive_bridge_test.c \
		-o $@ $(LDFLAGS) -pthread -lm
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_stream_primitives_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/tests/stream_primitives_test.c \
		samples/smalltalk/include/st_stream_primitives.h \
		samples/smalltalk/include/st_float_primitives.h \
		samples/smalltalk/include/st_heap_primitives.h \
		samples/smalltalk/include/st_source_bundle.h \
		samples/smalltalk/include/st_control_roots.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/tests/stream_primitives_test.c \
		-o $@ $(LDFLAGS) -pthread -lm
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_stream_primitive_bridge_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitive_bridge.c \
		samples/smalltalk/tests/stream_primitive_bridge_test.c \
		samples/smalltalk/include/st_stream_primitive_bridge.h \
		samples/smalltalk/include/st_stream_primitives.h \
		samples/smalltalk/include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitive_bridge.c \
		samples/smalltalk/tests/stream_primitive_bridge_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_string_primitives_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/tests/string_primitives_test.c \
		samples/smalltalk/include/st_string_primitives.h \
		samples/smalltalk/include/st_stream_primitives.h \
		samples/smalltalk/include/st_float_primitives.h \
		samples/smalltalk/include/st_heap_primitives.h \
		samples/smalltalk/include/st_source_bundle.h \
		samples/smalltalk/include/st_control_roots.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/tests/string_primitives_test.c \
		-o $@ $(LDFLAGS) -pthread -lm
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_string_primitive_bridge_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/src/runtime/primitives/string_primitive_bridge.c \
		samples/smalltalk/tests/string_primitive_bridge_test.c \
		samples/smalltalk/include/st_string_primitive_bridge.h \
		samples/smalltalk/include/st_string_primitives.h \
		samples/smalltalk/include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/src/runtime/primitives/string_primitive_bridge.c \
		samples/smalltalk/tests/string_primitive_bridge_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_symbol_intern_regression: \
		samples/smalltalk/src/frontend/lexer.c \
		samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c \
		samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/src/runtime/primitives/symbol_intern.c \
		samples/smalltalk/tests/symbol_intern_test.c \
		samples/smalltalk/include/st_symbol_intern.h \
		samples/smalltalk/include/st_image_runtime.h \
		samples/smalltalk/include/st_source_bundle.h \
		samples/smalltalk/include/st_string_primitives.h \
		samples/smalltalk/include/st_heap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c \
		samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c \
		samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/src/runtime/primitives/symbol_intern.c \
		samples/smalltalk/tests/symbol_intern_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_control_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/tests/control_test.c \
		samples/smalltalk/include/st_control.h \
		samples/smalltalk/include/st_runtime.h \
		samples/smalltalk/include/st_dispatch.h \
		samples/smalltalk/include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/tests/control_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_control_bridge_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/tests/control_bridge_test.c \
		samples/smalltalk/include/st_value.h \
		samples/smalltalk/include/st_runtime.h \
		samples/smalltalk/include/st_lookup.h \
		samples/smalltalk/include/st_send_bridge.h \
		samples/smalltalk/include/st_control.h \
		samples/smalltalk/include/st_control_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/tests/control_bridge_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_control_gc_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/tests/control_gc_test.c \
		samples/smalltalk/include/st_value.h \
		samples/smalltalk/include/st_runtime.h \
		samples/smalltalk/include/st_lookup.h \
		samples/smalltalk/include/st_send_bridge.h \
		samples/smalltalk/include/st_control.h \
		samples/smalltalk/include/st_control_roots.h \
		samples/smalltalk/include/st_heap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/tests/control_gc_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_closure_bridge_regression: \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/control/control_bridge.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/tests/closure_bridge_test.c \
		samples/smalltalk/include/st_closure_bridge.h \
		samples/smalltalk/include/st_send_bridge.h \
		samples/smalltalk/include/st_heap.h \
		samples/smalltalk/include/st_control.h \
		samples/smalltalk/include/st_control_bridge.h \
		samples/smalltalk/include/st_control_roots.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/control/control_bridge.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/tests/closure_bridge_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_block_primitives_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/control/control_bridge.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/runtime/primitives/block_primitives.c \
		samples/smalltalk/src/runtime/primitives/block_primitive_bridge.c \
		samples/smalltalk/tests/block_primitives_test.c \
		samples/smalltalk/include/st_block_primitives.h \
		samples/smalltalk/include/st_block_primitive_bridge.h \
		samples/smalltalk/include/st_closure_bridge.h \
		samples/smalltalk/include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/control/control_bridge.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/runtime/primitives/block_primitives.c \
		samples/smalltalk/src/runtime/primitives/block_primitive_bridge.c \
		samples/smalltalk/tests/block_primitives_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_exception_primitives_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/runtime/primitives/exception_primitives.c \
		samples/smalltalk/src/runtime/primitives/exception_primitive_bridge.c \
		samples/smalltalk/tests/exception_primitives_test.c \
		samples/smalltalk/include/st_exception_primitives.h \
		samples/smalltalk/include/st_exception_primitive_bridge.h \
		samples/smalltalk/include/st_closure_bridge.h \
		samples/smalltalk/include/st_control.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/runtime/primitives/exception_primitives.c \
		samples/smalltalk/src/runtime/primitives/exception_primitive_bridge.c \
		samples/smalltalk/tests/exception_primitives_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_reflection_primitives_regression: \
		samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c \
		samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/reflection_primitives.c \
		samples/smalltalk/src/runtime/primitives/reflection_primitive_bridge.c \
		samples/smalltalk/tests/reflection_primitives_test.c \
		samples/smalltalk/include/st_heap_primitives.h \
		samples/smalltalk/include/st_reflection_primitives.h \
		samples/smalltalk/include/st_reflection_primitive_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c \
		samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/reflection_primitives.c \
		samples/smalltalk/src/runtime/primitives/reflection_primitive_bridge.c \
		samples/smalltalk/tests/reflection_primitives_test.c \
		-o $@ $(LDFLAGS) -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_reflection_lower_regression: \
		samples/smalltalk/src/frontend/lexer.c \
		samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c \
		samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/compiler/lower.c \
		samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c \
		samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/reflection_primitives.c \
		samples/smalltalk/tests/reflection_lower_test.c \
		samples/smalltalk/tests/reflection_primitives_aot_harness.c \
		samples/smalltalk/include/st_reflection_primitives.h \
		samples/smalltalk/include/st_lower.h $(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c \
		samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c \
		samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c \
		samples/smalltalk/src/compiler/lower.c \
		samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c \
		samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/reflection_primitives.c \
		samples/smalltalk/tests/reflection_lower_test.c \
		-o $@ $(LIB_PATH) $(LDFLAGS) -pthread -lm
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_exception_lower_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/compiler/lower.c \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/exception_primitives.c \
		samples/smalltalk/tests/exception_lower_test.c \
		samples/smalltalk/tests/exception_primitives_aot_harness.c \
		samples/smalltalk/include/st_exception_primitives.h \
		samples/smalltalk/include/st_lower.h $(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/compiler/lower.c \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/exception_primitives.c \
		samples/smalltalk/tests/exception_lower_test.c \
		-o $@ $(LDFLAGS) -lanvil -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_dnu_lower_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/lower.c \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/tests/dnu_lower_test.c \
		samples/smalltalk/tests/dnu_test.c samples/smalltalk/src/runtime/dnu.c \
		samples/smalltalk/include/st_dnu.h samples/smalltalk/include/st_lower.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/lower.c \
		samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/tests/dnu_lower_test.c \
		-o $@ $(LDFLAGS) -lanvil -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_lower_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/src/runtime/primitives/block_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitive_bridge.c \
		samples/smalltalk/src/runtime/primitives/float_primitives_internal.h \
		samples/smalltalk/src/runtime/primitives/integer_primitives.c \
		samples/smalltalk/src/runtime/lookup.c \
		samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/compiler/lower.c \
		samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/runtime/primitives/primitive_bridge.c \
		samples/smalltalk/src/runtime/primitives/heap_primitive_bridge.c \
		samples/smalltalk/tests/lower_test.c \
		samples/smalltalk/include/st_lower.h \
		samples/smalltalk/include/st_class_graph.h \
		samples/smalltalk/include/st_selector.h \
		samples/smalltalk/include/st_primitive.h \
		samples/smalltalk/include/st_core_primitives.h \
		samples/smalltalk/include/st_heap_primitives.h \
		samples/smalltalk/include/st_stream_primitives.h \
		samples/smalltalk/include/st_string_primitives.h \
		samples/smalltalk/include/st_float_primitives.h \
		samples/smalltalk/include/st_float_primitive_bridge.h \
		samples/smalltalk/include/st_integer_primitives.h \
		samples/smalltalk/include/st_primitive_bridge.h \
		samples/smalltalk/include/st_heap_primitive_bridge.h \
		samples/smalltalk/include/st_send_bridge.h \
		samples/smalltalk/include/st_control.h \
		samples/smalltalk/include/st_control_roots.h \
		samples/smalltalk/include/st_control_bridge.h \
		samples/smalltalk/include/st_closure_bridge.h \
		samples/smalltalk/include/st_dispatch.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/src/runtime/primitives/block_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/integer_primitives.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/compiler/lower.c \
		samples/smalltalk/tests/lower_test.c \
		-o $@ $(LDFLAGS) -lanvil -pthread -lm
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_image_emit_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/image_layout.c \
		samples/smalltalk/src/compiler/image_emit.c \
		samples/smalltalk/tests/image_emit_test.c \
		samples/smalltalk/include/st_source_bundle.h \
		samples/smalltalk/include/st_class_graph.h \
		samples/smalltalk/include/st_selector.h \
		samples/smalltalk/include/st_image_emit.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c \
		samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/image_layout.c \
		samples/smalltalk/src/compiler/image_emit.c \
		samples/smalltalk/tests/image_emit_test.c \
		-o $@ $(LDFLAGS) -lanvil
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_aot_compile_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/integer_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/src/runtime/primitives/block_primitives.c \
		samples/smalltalk/src/runtime/primitives/exception_primitives.c \
		samples/smalltalk/src/runtime/primitives/reflection_primitives.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/runtime/control/control_bridge.c \
		samples/smalltalk/src/runtime/primitives/primitive_bridge.c \
		samples/smalltalk/src/compiler/lower.c samples/smalltalk/src/compiler/image_layout.c \
		samples/smalltalk/src/compiler/image_emit.c \
		samples/smalltalk/src/compiler/aot_compile.c \
		samples/smalltalk/tests/aot_compile_test.c \
		samples/smalltalk/include/st_aot_compile.h \
		samples/smalltalk/include/st_lower.h \
		samples/smalltalk/include/st_image_emit.h \
		samples/smalltalk/include/st_control_roots.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/integer_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/src/runtime/primitives/block_primitives.c \
		samples/smalltalk/src/runtime/primitives/exception_primitives.c \
		samples/smalltalk/src/runtime/primitives/reflection_primitives.c \
		samples/smalltalk/src/runtime/lookup.c \
		samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/compiler/lower.c samples/smalltalk/src/compiler/image_layout.c \
		samples/smalltalk/src/compiler/image_emit.c \
		samples/smalltalk/src/compiler/aot_compile.c \
		samples/smalltalk/tests/aot_compile_test.c \
		-o $@ $(LDFLAGS) -lanvil -pthread -lm
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_artifact_bundle_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/compiler/lower.c samples/smalltalk/src/compiler/image_layout.c \
		samples/smalltalk/src/compiler/image_emit.c \
		samples/smalltalk/src/compiler/aot_compile.c \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/tests/artifact_bundle_test.c \
		samples/smalltalk/include/st_artifact_bundle.h \
		samples/smalltalk/include/st_aot_compile.h \
		samples/smalltalk/include/st_lower.h \
		samples/smalltalk/include/st_image_emit.h \
		samples/smalltalk/include/st_control_roots.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/compiler/lower.c samples/smalltalk/src/compiler/image_layout.c \
		samples/smalltalk/src/compiler/image_emit.c \
		samples/smalltalk/src/compiler/aot_compile.c \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/tests/artifact_bundle_test.c \
		-o $@ $(LDFLAGS) -lanvil -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_artifact_materialize_regression: \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/compiler/artifact_materialize.c \
		samples/smalltalk/tests/artifact_materialize_test.c \
		samples/smalltalk/include/st_artifact_bundle.h \
		samples/smalltalk/include/st_artifact_materialize.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/compiler/artifact_materialize.c \
		samples/smalltalk/tests/artifact_materialize_test.c \
		-o $@ $(LDFLAGS) -lanvil -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_application_materialize_regression: \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/compiler/artifact_materialize.c \
		samples/smalltalk/src/compiler/artifact_materialize_internal.h \
		samples/smalltalk/src/compiler/application_materialize.c \
		samples/smalltalk/tests/application_materialize_test.c \
		samples/smalltalk/include/st_artifact_bundle.h \
		samples/smalltalk/include/st_artifact_materialize.h \
		samples/smalltalk/include/st_application_aot.h \
		samples/smalltalk/include/st_application_materialize.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/compiler/artifact_materialize.c \
		samples/smalltalk/src/compiler/application_materialize.c \
		samples/smalltalk/tests/application_materialize_test.c \
		-o $@ $(LDFLAGS) -lanvil -pthread
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_application_aot_regression: \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/integer_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/src/runtime/primitives/block_primitives.c \
		samples/smalltalk/src/runtime/primitives/exception_primitives.c \
		samples/smalltalk/src/runtime/primitives/reflection_primitives.c \
		samples/smalltalk/src/runtime/primitives/product_primitives.c samples/smalltalk/src/runtime/primitives/fiber_catalog.c samples/smalltalk/src/runtime/primitives/socket_catalog.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/compiler/lower.c samples/smalltalk/src/compiler/image_layout.c \
		samples/smalltalk/src/compiler/image_emit.c \
		samples/smalltalk/src/compiler/aot_compile.c \
		samples/smalltalk/src/compiler/application_launch.c \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/product/application_aot.c \
		samples/smalltalk/tests/application_aot_test.c \
		samples/smalltalk/include/st_application_aot.h \
		samples/smalltalk/include/st_application_launch.h \
		samples/smalltalk/include/st_product_primitives.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/frontend/lexer.c samples/smalltalk/src/frontend/ast.c \
		samples/smalltalk/src/frontend/parser.c samples/smalltalk/src/frontend/sema.c \
		samples/smalltalk/src/frontend/source_bundle.c \
		samples/smalltalk/src/frontend/class_graph.c samples/smalltalk/src/frontend/selector.c \
		samples/smalltalk/src/compiler/primitive.c samples/smalltalk/src/runtime/value.c \
		samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c samples/smalltalk/src/runtime/heap.c \
		samples/smalltalk/src/runtime/control/control.c \
		samples/smalltalk/src/runtime/control/control_roots.c \
		samples/smalltalk/src/runtime/primitives/core_primitives.c \
		samples/smalltalk/src/runtime/primitives/heap_primitives.c \
		samples/smalltalk/src/runtime/primitives/float_primitives.c \
		samples/smalltalk/src/runtime/primitives/integer_primitives.c \
		samples/smalltalk/src/runtime/primitives/stream_primitives.c \
		samples/smalltalk/src/runtime/primitives/string_primitives.c \
		samples/smalltalk/src/runtime/primitives/block_primitives.c \
		samples/smalltalk/src/runtime/primitives/exception_primitives.c \
		samples/smalltalk/src/runtime/primitives/reflection_primitives.c \
		samples/smalltalk/src/runtime/primitives/product_primitives.c samples/smalltalk/src/runtime/primitives/fiber_catalog.c samples/smalltalk/src/runtime/primitives/socket_catalog.c \
		samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c \
		samples/smalltalk/src/runtime/image_runtime.c \
		samples/smalltalk/src/runtime/closure_bridge.c \
		samples/smalltalk/src/compiler/lower.c samples/smalltalk/src/compiler/image_layout.c \
		samples/smalltalk/src/compiler/image_emit.c \
		samples/smalltalk/src/compiler/aot_compile.c \
		samples/smalltalk/src/compiler/application_launch.c \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/product/application_aot.c \
		samples/smalltalk/tests/application_aot_test.c \
		-o $@ $(LDFLAGS) -lanvil -pthread -lm
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_aot_toolchain_regression: \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/product/aot_toolchain.c \
		samples/smalltalk/tests/aot_toolchain_test.c \
		samples/smalltalk/include/st_aot_toolchain.h \
		samples/smalltalk/include/st_artifact_bundle.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/product/aot_toolchain.c \
		samples/smalltalk/tests/aot_toolchain_test.c \
		-o $@ $(LDFLAGS) -lanvil
	@echo "Built $@"

$(BUILD_DIR)/tests/smalltalk_application_samples_regression: \
		$(BUILD_DIR)/samples/smalltalk/st-aotc \
		$(BUILD_DIR)/samples/smalltalk/st-aot-link \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/product/aot_toolchain.c \
		samples/smalltalk/src/product/application_startup.c \
		samples/smalltalk/tests/hello_application_test.c \
		samples/smalltalk/examples/support/native_main.c \
		samples/smalltalk/examples/hello/HelloApplication.st \
		samples/smalltalk/examples/hello/application.manifest \
		samples/smalltalk/examples/closures/ClosuresApplication.st \
		samples/smalltalk/examples/closures/application.manifest \
		samples/smalltalk/include/st_aot_toolchain.h \
		samples/smalltalk/include/st_application_startup.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		-DST_AOTC_PATH=\"$(abspath $(BUILD_DIR)/samples/smalltalk/st-aotc)\" \
		-DST_AOT_LINK_PATH=\"$(abspath $(BUILD_DIR)/samples/smalltalk/st-aot-link)\" \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/product/aot_toolchain.c \
		samples/smalltalk/tests/hello_application_test.c \
		-o $@ $(LDFLAGS) -lanvil
	@echo "Built $@"

SMALLTALK_APPLICATION_COMPILER_SRCS = \
	samples/smalltalk/src/frontend/lexer.c \
	samples/smalltalk/src/frontend/ast.c \
	samples/smalltalk/src/frontend/parser.c \
	samples/smalltalk/src/frontend/sema.c \
	samples/smalltalk/src/frontend/source_bundle.c \
	samples/smalltalk/src/frontend/class_graph.c \
	samples/smalltalk/src/frontend/selector.c \
	samples/smalltalk/src/compiler/primitive.c \
	samples/smalltalk/src/runtime/value.c \
	samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/$(HOST_PLATFORM)/runtime.c \
	samples/smalltalk/src/runtime/heap.c \
	samples/smalltalk/src/runtime/control/control.c \
	samples/smalltalk/src/runtime/control/control_roots.c \
	samples/smalltalk/src/runtime/primitives/core_primitives.c \
	samples/smalltalk/src/runtime/primitives/heap_primitives.c \
	samples/smalltalk/src/runtime/primitives/float_primitives.c \
	samples/smalltalk/src/runtime/primitives/integer_primitives.c \
	samples/smalltalk/src/runtime/primitives/stream_primitives.c \
	samples/smalltalk/src/runtime/primitives/string_primitives.c \
	samples/smalltalk/src/runtime/primitives/block_primitives.c \
	samples/smalltalk/src/runtime/primitives/exception_primitives.c \
	samples/smalltalk/src/runtime/primitives/reflection_primitives.c \
	samples/smalltalk/src/runtime/primitives/product_primitives.c samples/smalltalk/src/runtime/primitives/fiber_catalog.c samples/smalltalk/src/runtime/primitives/socket_catalog.c \
	samples/smalltalk/src/runtime/lookup.c \
	samples/smalltalk/src/runtime/send_bridge.c \
	samples/smalltalk/src/runtime/image_runtime.c \
	samples/smalltalk/src/runtime/closure_bridge.c \
	samples/smalltalk/src/compiler/lower.c \
	samples/smalltalk/src/compiler/image_layout.c \
	samples/smalltalk/src/compiler/image_emit.c \
	samples/smalltalk/src/compiler/aot_compile.c \
	samples/smalltalk/src/compiler/application_launch.c \
	samples/smalltalk/src/compiler/artifact_bundle.c \
	samples/smalltalk/src/compiler/artifact_materialize.c \
	samples/smalltalk/src/compiler/application_materialize.c \
	samples/smalltalk/src/product/application_aot.c

smalltalk-aotc: $(BUILD_DIR)/samples/smalltalk/st-aotc

$(BUILD_DIR)/samples/smalltalk/st-aotc: \
		$(SMALLTALK_APPLICATION_COMPILER_SRCS) \
		samples/smalltalk/tools/st_aotc.c \
		samples/smalltalk/include/st_application_aot.h \
		samples/smalltalk/include/st_application_materialize.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	CCACHE_DISABLE=1 $(CC) $(CFLAGS) -Isamples/smalltalk/include \
		$(SMALLTALK_APPLICATION_COMPILER_SRCS) \
		samples/smalltalk/tools/st_aotc.c \
		-o $@ $(LDFLAGS) -lanvil -pthread -lm
	@echo "Built $@"

SMALLTALK_PRODUCT_RUNTIME_SRCS = \
	$(wildcard samples/smalltalk/src/platform/$(HOST_PLATFORM)/*.c) \
	samples/smalltalk/src/compiler/primitive.c \
	$(wildcard samples/smalltalk/src/runtime/*.c) \
	$(wildcard samples/smalltalk/src/runtime/control/*.c) \
	$(wildcard samples/smalltalk/src/runtime/primitives/*.c) \
	samples/smalltalk/src/product/application_startup.c

SMALLTALK_PRODUCT_RUNTIME = \
	$(BUILD_DIR)/samples/smalltalk/smalltalk-runtime.o

smalltalk-runtime: $(SMALLTALK_PRODUCT_RUNTIME)

$(SMALLTALK_PRODUCT_RUNTIME): \
		$(SMALLTALK_PRODUCT_RUNTIME_SRCS) \
		$(wildcard samples/smalltalk/include/*.h)
	@mkdir -p $(dir $@)
	CCACHE_DISABLE=1 $(CC) $(CFLAGS) -Isamples/smalltalk/include -r \
		$(SMALLTALK_PRODUCT_RUNTIME_SRCS) -o $@
	@echo "Built $@"

smalltalk-aot-link: $(BUILD_DIR)/samples/smalltalk/st-aot-link

$(BUILD_DIR)/samples/smalltalk/st-aot-link: \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/product/aot_toolchain.c \
		samples/smalltalk/tools/st_aot_link.c \
		samples/smalltalk/include/st_aot_toolchain.h \
		samples/smalltalk/include/st_artifact_bundle.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	CCACHE_DISABLE=1 $(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/compiler/artifact_bundle.c \
		samples/smalltalk/src/product/aot_toolchain.c \
		samples/smalltalk/tools/st_aot_link.c \
		-o $@ $(LDFLAGS) -lanvil
	@echo "Built $@"

ST_IMAGE_DIR ?= samples/smalltalk/st-image
ST_APP_OUTPUT_DIR ?= $(BUILD_DIR)/smalltalk
ST_ENTRY_SELECTOR ?= run

smalltalk-app: smalltalk-aotc smalltalk-aot-link smalltalk-runtime
	@test -n "$(ST_APP_DIR)" || \
		{ echo "ST_APP_DIR is required" >&2; exit 64; }
	@test -n "$(ST_APP_NAME)" || \
		{ echo "ST_APP_NAME is required" >&2; exit 64; }
	@test -n "$(ST_ENTRY_CLASS)" || \
		{ echo "ST_ENTRY_CLASS is required" >&2; exit 64; }
	@mkdir -p "$(abspath $(ST_APP_OUTPUT_DIR))"
	$(BUILD_DIR)/samples/smalltalk/st-aotc \
		"$(abspath $(ST_IMAGE_DIR))" "$(abspath $(ST_APP_DIR))" \
		"$(ST_APP_NAME)" "$(ST_ENTRY_CLASS)" "$(ST_ENTRY_SELECTOR)" \
		"$(abspath $(ST_APP_OUTPUT_DIR))"
	@mkdir -p \
		"$(abspath $(ST_APP_OUTPUT_DIR))/$(ST_APP_NAME)/native" \
		"$(BUILD_DIR)/samples/smalltalk/apps/$(ST_APP_NAME)"
	CCACHE_DISABLE=1 $(CC) $(CFLAGS) -Isamples/smalltalk/include \
		'-DST_APPLICATION_LAUNCH_SYMBOL=st_app_$(ST_APP_NAME)_launch_descriptor' \
		-c samples/smalltalk/examples/support/native_main.c \
		-o "$(BUILD_DIR)/samples/smalltalk/apps/$(ST_APP_NAME)/native_main.o"
	$(BUILD_DIR)/samples/smalltalk/st-aot-link \
		"$(abspath $(ST_APP_OUTPUT_DIR))/$(ST_APP_NAME)/x86_64-sysv-gas-O2" \
		"$(abspath $(ST_APP_OUTPUT_DIR))/$(ST_APP_NAME)/native" \
		host "$(ST_APP_NAME)" \
		"$(abspath $(SMALLTALK_PRODUCT_RUNTIME))" \
		"$(abspath $(BUILD_DIR)/samples/smalltalk/apps/$(ST_APP_NAME)/native_main.o)"
	@echo "Executable: $(abspath $(ST_APP_OUTPUT_DIR))/$(ST_APP_NAME)/native/host/$(ST_APP_NAME)"

$(BUILD_DIR)/tests/smalltalk_dispatch_regression: \
		samples/smalltalk/src/compiler/dispatch.c \
		samples/smalltalk/tests/dispatch_test.c \
		samples/smalltalk/include/st_dispatch.h \
		$(INCLUDE_DIR)/anvil/anvil.h \
		$(INCLUDE_DIR)/anvil/anvil_internal.h \
		$(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isamples/smalltalk/include \
		samples/smalltalk/src/compiler/dispatch.c \
		samples/smalltalk/tests/dispatch_test.c \
		-o $@ $(LDFLAGS) -lanvil
	@echo "Built $@"

# Runtime examples that generate assembly, assemble/link it, and execute it.
examples-runtime: lib
	$(MAKE) -C $(EXAMPLES_DIR)/basic_runtime

test-examples: examples-runtime
	$(MAKE) -C $(EXAMPLES_DIR)/basic_runtime test

clean-examples-runtime:
	$(MAKE) -C $(EXAMPLES_DIR)/basic_runtime clean

# Advanced examples (in subdirectories with their own Makefiles)
examples-advanced: lib
	@echo "Building fp_math_lib example..."
	$(MAKE) -C $(EXAMPLES_DIR)/fp_math_lib
	@echo "Building dynamic_array example..."
	$(MAKE) -C $(EXAMPLES_DIR)/dynamic_array
	@echo "Building base64_lib example..."
	$(MAKE) -C $(EXAMPLES_DIR)/base64_lib

test-examples-advanced: examples-advanced
	@echo "Testing fp_math_lib..."
	$(MAKE) -C $(EXAMPLES_DIR)/fp_math_lib test
	@echo "Testing dynamic_array..."
	$(MAKE) -C $(EXAMPLES_DIR)/dynamic_array test
	@echo "Testing base64_lib..."
	$(MAKE) -C $(EXAMPLES_DIR)/base64_lib test

clean-examples-advanced:
	$(MAKE) -C $(EXAMPLES_DIR)/fp_math_lib clean
	$(MAKE) -C $(EXAMPLES_DIR)/dynamic_array clean
	$(MAKE) -C $(EXAMPLES_DIR)/base64_lib clean

clean:
	rm -rf $(BUILD_DIR) $(LIB_DIR)

install: lib
	@echo "Installing to /usr/local..."
	install -d /usr/local/include/anvil
	install -m 644 $(INCLUDE_DIR)/anvil/*.h /usr/local/include/anvil/
	install -d /usr/local/lib
	install -m 644 $(LIB_PATH) /usr/local/lib/

# Dependencies
$(BUILD_DIR)/core/context.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/types.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/module.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h $(INCLUDE_DIR)/anvil/anvil_opt.h
$(BUILD_DIR)/core/function.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/value.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/builder.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/strbuf.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/backend.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/verify.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/machine/machine_ir.o: $(INCLUDE_DIR)/anvil/anvil_machine.h src/machine/machine_internal.h
$(BUILD_DIR)/machine/regalloc.o: $(INCLUDE_DIR)/anvil/anvil_machine.h src/machine/machine_internal.h
$(BUILD_DIR)/backend/arm64/arm64_mir.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h $(INCLUDE_DIR)/anvil/anvil_machine.h $(INCLUDE_DIR)/anvil/anvil_arm64_mir.h
