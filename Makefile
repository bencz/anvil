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

# Source files
CORE_SRCS = \
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
	$(SRC_DIR)/backend/x86/x86.c \
	$(SRC_DIR)/backend/x86/x86_helpers.c \
	$(SRC_DIR)/backend/x86/x86_mir.c \
	$(SRC_DIR)/backend/x86_64/x86_64.c \
	$(SRC_DIR)/backend/x86_64/x86_64_helpers.c \
	$(SRC_DIR)/backend/x86_64/x86_64_mir.c \
	$(SRC_DIR)/backend/mainframe/mainframe_mir.c \
	$(SRC_DIR)/backend/s370/s370.c \
	$(SRC_DIR)/backend/s370_xa/s370_xa.c \
	$(SRC_DIR)/backend/s390/s390.c \
	$(SRC_DIR)/backend/zarch/zarch.c \
	$(SRC_DIR)/backend/ppc/ppc_mir.c \
	$(SRC_DIR)/backend/ppc32/ppc32.c \
	$(SRC_DIR)/backend/ppc64/ppc64.c \
	$(SRC_DIR)/backend/ppc64le/ppc64le.c \
	$(SRC_DIR)/backend/arm64/arm64.c \
	$(SRC_DIR)/backend/arm64/arm64_helpers.c \
	$(SRC_DIR)/backend/arm64/arm64_mir.c

OPT_SRCS = \
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
	$(SRC_DIR)/machine/machine_ir.c \
	$(SRC_DIR)/machine/regalloc.c

ALL_SRCS = $(CORE_SRCS) $(BACKEND_SRCS) $(OPT_SRCS) $(MACHINE_SRCS)

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
	$(BUILD_DIR)/tests/core_arm64_regression \
	$(BUILD_DIR)/tests/ir_verifier_regression \
	$(BUILD_DIR)/tests/optimizer_regression \
	$(BUILD_DIR)/tests/machine_regalloc_regression \
	$(BUILD_DIR)/tests/arm64_mir_lowering_regression \
	$(BUILD_DIR)/tests/x86_64_mir_lowering_regression \
	$(BUILD_DIR)/tests/x86_mir_lowering_regression \
	$(BUILD_DIR)/tests/ppc_mir_lowering_regression \
	$(BUILD_DIR)/tests/mainframe_mir_lowering_regression \
	$(BUILD_DIR)/tests/fcmp_backend_regression \
	$(BUILD_DIR)/tests/typed_gep_backend_regression

.PHONY: all clean lib examples tests test-win64-abi test-fcmp-i1-runtime test-sanitize test-valgrind install examples-runtime test-examples clean-examples-runtime examples-advanced test-examples-advanced clean-examples-advanced

all: lib examples

lib: $(LIB_PATH)

$(LIB_PATH): $(OBJS)
	@mkdir -p $(LIB_DIR)
	$(AR) $(ARFLAGS) $@ $^
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

test-valgrind: tests
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

$(BUILD_DIR)/tests/%: tests/%.c $(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -lanvil
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
$(BUILD_DIR)/backend/mainframe/mainframe_mir.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h $(INCLUDE_DIR)/anvil/anvil_machine.h $(INCLUDE_DIR)/anvil/anvil_mainframe_mir.h
$(BUILD_DIR)/backend/ppc/ppc_mir.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h $(INCLUDE_DIR)/anvil/anvil_machine.h $(INCLUDE_DIR)/anvil/anvil_ppc_mir.h
$(BUILD_DIR)/backend/arm64/arm64_mir.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h $(INCLUDE_DIR)/anvil/anvil_machine.h $(INCLUDE_DIR)/anvil/anvil_arm64_mir.h
$(BUILD_DIR)/backend/x86/x86.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h $(INCLUDE_DIR)/anvil/anvil_machine.h $(INCLUDE_DIR)/anvil/anvil_x86_mir.h src/backend/x86/x86_internal.h
$(BUILD_DIR)/backend/x86/x86_helpers.o: src/backend/x86/x86_internal.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/backend/x86/x86_mir.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h $(INCLUDE_DIR)/anvil/anvil_machine.h $(INCLUDE_DIR)/anvil/anvil_x86_mir.h src/backend/x86/x86_internal.h
$(BUILD_DIR)/backend/x86_64/x86_64.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h $(INCLUDE_DIR)/anvil/anvil_machine.h $(INCLUDE_DIR)/anvil/anvil_x86_64_mir.h src/backend/x86_64/x86_64_internal.h
$(BUILD_DIR)/backend/x86_64/x86_64_helpers.o: src/backend/x86_64/x86_64_internal.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/backend/x86_64/x86_64_mir.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h $(INCLUDE_DIR)/anvil/anvil_machine.h $(INCLUDE_DIR)/anvil/anvil_x86_64_mir.h src/backend/x86_64/x86_64_internal.h
