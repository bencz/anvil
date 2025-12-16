# ANVIL Base64 Library Example - Makefile
#
# This Makefile builds the base64 library generator, generates assembly,
# assembles it, and links with a C test program.

CC = gcc
AS = as
CFLAGS = -Wall -Wextra -std=c11 -g -O2
ANVIL_INCLUDE = -I../../include
ANVIL_LIB = -L../../lib -lanvil

# Detect architecture
UNAME_M := $(shell uname -m)
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    ifeq ($(UNAME_M),arm64)
        ARCH = arm64_macos
        ASFLAGS = -arch arm64
    else
        ARCH = x86_64
        ASFLAGS = 
    endif
else
    ifeq ($(UNAME_M),x86_64)
        ARCH = x86_64
        ASFLAGS = 
    else ifeq ($(UNAME_M),aarch64)
        ARCH = arm64_linux
        ASFLAGS = 
    else ifeq ($(UNAME_M),ppc64le)
        ARCH = ppc64le
        ASFLAGS = 
    else ifeq ($(UNAME_M),ppc64)
        ARCH = ppc64
        ASFLAGS = 
    else
        ARCH = x86
        ASFLAGS = --32
    endif
endif

.PHONY: all clean test

all: test_base64

# Build the generator
generate_base64: generate_base64.c
	$(CC) $(CFLAGS) $(ANVIL_INCLUDE) $< -o $@ $(ANVIL_LIB)

# Generate assembly
base64_lib.s: generate_base64
	@echo "Generating assembly for $(ARCH)..."
	./generate_base64 $(ARCH) > $@

# Assemble
base64_lib.o: base64_lib.s
	@echo "Assembling base64_lib.s..."
	$(AS) $(ASFLAGS) $< -o $@

# Compile test program
test_base64.o: test_base64.c
	$(CC) $(CFLAGS) -c $< -o $@

# Link
test_base64: test_base64.o base64_lib.o
	@echo "Linking test_base64..."
	$(CC) $^ -o $@

# Run test
test: test_base64
	@echo ""
	@echo "Running test..."
	@echo ""
	./test_base64

clean:
	rm -f generate_base64 base64_lib.s base64_lib.o test_base64.o test_base64
