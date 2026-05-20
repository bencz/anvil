# ANVIL Basic Runtime Example

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

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

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -O2
ANVIL_INC = -I../../include
ANVIL_LIB = -L../../lib -lanvil

GENERATOR = generate_basic_runtime
RUNTIME_ASM = basic_runtime.s
RUNTIME_OBJ = basic_runtime.o
TEST_PROG = test_basic_runtime
TEST_OBJ = test_basic_runtime.o

.PHONY: all generate test clean show-asm

all: $(TEST_PROG)

$(GENERATOR): generate_basic_runtime.c ../arch_select.h
	$(CC) $(CFLAGS) $(ANVIL_INC) generate_basic_runtime.c -o $(GENERATOR) $(ANVIL_LIB)

generate: $(GENERATOR)

$(RUNTIME_ASM): $(GENERATOR)
	@echo "Generating assembly for $(ARCH)..."
	./$(GENERATOR) $(ARCH) > $(RUNTIME_ASM)

$(RUNTIME_OBJ): $(RUNTIME_ASM)
	@echo "Assembling $(RUNTIME_ASM)..."
	as $(AS_FLAGS) $(RUNTIME_ASM) -o $(RUNTIME_OBJ)

$(TEST_OBJ): test_basic_runtime.c
	$(CC) $(CFLAGS) -c test_basic_runtime.c -o $(TEST_OBJ)

$(TEST_PROG): $(RUNTIME_OBJ) $(TEST_OBJ)
	@echo "Linking $(TEST_PROG)..."
	$(CC) $(TEST_OBJ) $(RUNTIME_OBJ) -o $(TEST_PROG) -lm

test: $(TEST_PROG)
	@echo ""
	@echo "Running basic runtime test..."
	@echo ""
	./$(TEST_PROG)

show-asm: $(RUNTIME_ASM)
	@cat $(RUNTIME_ASM)

clean:
	rm -f $(GENERATOR) $(RUNTIME_ASM) $(RUNTIME_OBJ) $(TEST_OBJ) $(TEST_PROG)
