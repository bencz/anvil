# ANVIL Floating-Point Math Library Example
#
# This Makefile builds the math library generator and demonstrates
# how to link ANVIL-generated assembly with C code.
#
# Usage:
#   make              - Build everything for the current platform
#   make generate     - Build only the generator
#   make test         - Build and run the test
#   make clean        - Clean all generated files
#   make show-asm     - Show the generated assembly code

# Detect OS and architecture
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Set architecture based on platform
ifeq ($(UNAME_S),Darwin)
    ifeq ($(UNAME_M),arm64)
        ARCH = arm64_macos
        AS_FLAGS = -arch arm64
    else
        ARCH = x86_64
        AS_FLAGS =
    endif
else
    ifeq ($(UNAME_M),x86_64)
        ARCH = x86_64
        AS_FLAGS =
    else ifeq ($(UNAME_M),aarch64)
        ARCH = arm64
        AS_FLAGS =
    else ifeq ($(UNAME_M),ppc64le)
        ARCH = ppc64le
        AS_FLAGS =
    else ifeq ($(UNAME_M),ppc64)
        ARCH = ppc64
        AS_FLAGS =
    else ifeq ($(UNAME_M),ppc)
        ARCH = ppc32
        AS_FLAGS =
    else
        ARCH = x86_64
        AS_FLAGS =
    endif
endif

# Compiler settings
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -O2
ANVIL_INC = -I../../include
ANVIL_LIB = -L../../lib -lanvil

# Output files
GENERATOR = generate_math
MATH_LIB_ASM = math_lib.s
MATH_LIB_OBJ = math_lib.o
TEST_PROG = test_math
TEST_OBJ = test_math.o

.PHONY: all generate test clean show-asm help

all: $(TEST_PROG)

help:
	@echo "ANVIL Floating-Point Math Library Example"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build everything (default)"
	@echo "  generate  - Build only the generator"
	@echo "  test      - Build and run the test"
	@echo "  show-asm  - Show the generated assembly"
	@echo "  clean     - Remove generated files"
	@echo ""
	@echo "Detected platform: $(UNAME_S) $(UNAME_M)"
	@echo "Using architecture: $(ARCH)"

# Build the generator
$(GENERATOR): generate_math.c ../arch_select.h
	$(CC) $(CFLAGS) $(ANVIL_INC) generate_math.c -o $(GENERATOR) $(ANVIL_LIB)

generate: $(GENERATOR)

# Generate the assembly code
$(MATH_LIB_ASM): $(GENERATOR)
	@echo "Generating assembly for $(ARCH)..."
	./$(GENERATOR) $(ARCH) > $(MATH_LIB_ASM)

# Assemble the generated code
$(MATH_LIB_OBJ): $(MATH_LIB_ASM)
	@echo "Assembling $(MATH_LIB_ASM)..."
	as $(AS_FLAGS) $(MATH_LIB_ASM) -o $(MATH_LIB_OBJ)

# Compile the test program
$(TEST_OBJ): test_math.c
	$(CC) $(CFLAGS) -c test_math.c -o $(TEST_OBJ)

# Link everything together
$(TEST_PROG): $(MATH_LIB_OBJ) $(TEST_OBJ)
	@echo "Linking $(TEST_PROG)..."
	$(CC) $(TEST_OBJ) $(MATH_LIB_OBJ) -o $(TEST_PROG) -lm

# Run the test
test: $(TEST_PROG)
	@echo ""
	@echo "Running test..."
	@echo ""
	./$(TEST_PROG)

# Show the generated assembly
show-asm: $(MATH_LIB_ASM)
	@echo "=== Generated Assembly ($(ARCH)) ==="
	@cat $(MATH_LIB_ASM)

# Clean up
clean:
	rm -f $(GENERATOR) $(MATH_LIB_ASM) $(MATH_LIB_OBJ) $(TEST_OBJ) $(TEST_PROG)
