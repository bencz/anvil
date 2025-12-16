# ANVIL Dynamic Array Library Example
#
# This example demonstrates calling C library functions (malloc, free, memcpy)
# from ANVIL-generated assembly code.

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
GENERATOR = generate_dynarray
LIB_ASM = dynarray_lib.s
LIB_OBJ = dynarray_lib.o
TEST_PROG = test_dynarray
TEST_OBJ = test_dynarray.o

.PHONY: all generate test clean show-asm help

all: $(TEST_PROG)

help:
	@echo "ANVIL Dynamic Array Library Example"
	@echo ""
	@echo "This example demonstrates calling C library functions"
	@echo "(malloc, free, memcpy) from ANVIL-generated code."
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
$(GENERATOR): generate_dynarray.c ../arch_select.h
	$(CC) $(CFLAGS) $(ANVIL_INC) generate_dynarray.c -o $(GENERATOR) $(ANVIL_LIB)

generate: $(GENERATOR)

# Generate the assembly code
$(LIB_ASM): $(GENERATOR)
	@echo "Generating assembly for $(ARCH)..."
	./$(GENERATOR) $(ARCH) > $(LIB_ASM)

# Assemble the generated code
$(LIB_OBJ): $(LIB_ASM)
	@echo "Assembling $(LIB_ASM)..."
	as $(AS_FLAGS) $(LIB_ASM) -o $(LIB_OBJ)

# Compile the test program
$(TEST_OBJ): test_dynarray.c
	$(CC) $(CFLAGS) -c test_dynarray.c -o $(TEST_OBJ)

# Link everything together
$(TEST_PROG): $(LIB_OBJ) $(TEST_OBJ)
	@echo "Linking $(TEST_PROG)..."
	$(CC) $(TEST_OBJ) $(LIB_OBJ) -o $(TEST_PROG)

# Run the test
test: $(TEST_PROG)
	@echo ""
	@echo "Running test..."
	@echo ""
	./$(TEST_PROG)

# Show the generated assembly
show-asm: $(LIB_ASM)
	@echo "=== Generated Assembly ($(ARCH)) ==="
	@cat $(LIB_ASM)

# Clean up
clean:
	rm -f $(GENERATOR) $(LIB_ASM) $(LIB_OBJ) $(TEST_OBJ) $(TEST_PROG)
