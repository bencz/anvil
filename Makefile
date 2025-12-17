# ANVIL - Makefile
#
# Build the ANVIL library and examples

CC = gcc
AR = ar
CFLAGS = -Wall -Wextra -std=c11 -I./include -g -O2
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
	$(SRC_DIR)/core/types.c \
	$(SRC_DIR)/core/module.c \
	$(SRC_DIR)/core/function.c \
	$(SRC_DIR)/core/value.c \
	$(SRC_DIR)/core/builder.c \
	$(SRC_DIR)/core/strbuf.c \
	$(SRC_DIR)/core/backend.c \
	$(SRC_DIR)/core/memory.c \
	$(SRC_DIR)/core/ir_dump.c

BACKEND_SRCS = \
	$(SRC_DIR)/backend/x86/x86.c \
	$(SRC_DIR)/backend/x86_64/x86_64.c \
	$(SRC_DIR)/backend/s370/s370.c \
	$(SRC_DIR)/backend/s370_xa/s370_xa.c \
	$(SRC_DIR)/backend/s390/s390.c \
	$(SRC_DIR)/backend/zarch/zarch.c \
	$(SRC_DIR)/backend/ppc32/ppc32.c \
	$(SRC_DIR)/backend/ppc64/ppc64.c \
	$(SRC_DIR)/backend/ppc64/ppc64_emit.c \
	$(SRC_DIR)/backend/ppc64/ppc64_cpu.c \
	$(SRC_DIR)/backend/ppc64le/ppc64le.c \
	$(SRC_DIR)/backend/arm64/arm64.c \
	$(SRC_DIR)/backend/arm64/arm64_helpers.c \
	$(SRC_DIR)/backend/arm64/arm64_emit.c \
	$(SRC_DIR)/backend/arm64/opt/arm64_opt.c \
	$(SRC_DIR)/backend/arm64/opt/arm64_peephole.c \
	$(SRC_DIR)/backend/arm64/opt/arm64_dead_store.c \
	$(SRC_DIR)/backend/arm64/opt/arm64_load_elim.c \
	$(SRC_DIR)/backend/arm64/opt/arm64_branch.c \
	$(SRC_DIR)/backend/arm64/opt/arm64_immediate.c

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
	$(SRC_DIR)/opt/loop_unroll.c \
	$(SRC_DIR)/opt/ctx_opt.c \
	$(SRC_DIR)/opt/store_load_prop.c

ALL_SRCS = $(CORE_SRCS) $(BACKEND_SRCS) $(OPT_SRCS)

# Object files
OBJS = $(ALL_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

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
	$(BUILD_DIR)/examples/loop_unroll_test \
	$(BUILD_DIR)/examples/memory_opt_test \
	$(BUILD_DIR)/examples/cse_test \
	$(BUILD_DIR)/examples/global_test \
	$(BUILD_DIR)/examples/cpu_model_test \
	$(BUILD_DIR)/examples/ir_dump_test

.PHONY: all clean lib examples install examples-advanced test-examples-advanced clean-examples-advanced

all: lib examples

lib: $(LIB_PATH)

$(LIB_PATH): $(OBJS)
	@mkdir -p $(LIB_DIR)
	$(AR) $(ARFLAGS) $@ $^
	@echo "Built $(LIB_PATH)"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

examples: lib $(EXAMPLES)

$(BUILD_DIR)/examples/%: $(EXAMPLES_DIR)/%.c $(LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -lanvil
	@echo "Built $@"

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
$(BUILD_DIR)/core/module.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/function.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/value.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/builder.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/strbuf.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
$(BUILD_DIR)/core/backend.o: $(INCLUDE_DIR)/anvil/anvil.h $(INCLUDE_DIR)/anvil/anvil_internal.h
