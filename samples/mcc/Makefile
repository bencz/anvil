# MCC - Micro C Compiler
# Main Makefile
#
# This is the main Makefile that includes modular configuration files.
# Structure:
#   mk/config.mk       - Compiler flags, directories, source files
#   mk/rules.mk        - Compilation rules and dependencies
#   mk/tests.mk        - Syntax test targets
#   mk/tests_codegen.mk - Code generation test targets

# Default target (must be first!)
.DEFAULT_GOAL := all

# Include configuration
include mk/config.mk

# Include build rules
include mk/rules.mk

# Include test rules
include mk/tests.mk
include mk/tests_codegen.mk

# ============================================================
# Main Targets
# ============================================================

# Default target
all: $(TARGET)

# Clean build artifacts
clean: clean-tests
	rm -rf $(OBJDIR) $(TARGET)

# Install (to parent project)
install: $(TARGET)
	cp $(TARGET) ../../bin/

# ============================================================
# Combined Test Targets
# ============================================================

# Run all tests (syntax + codegen + cross-standard)
test-all: test-unit test-build-deps test-syntax test-negative test-codegen test-cross test-layout-matrix
	@echo ""
	@echo "=========================================="
	@echo "All tests complete!"
	@echo "=========================================="

# Quick test (just C89 syntax)
test-quick: test-syntax-c89

# ============================================================
# Help
# ============================================================

help:
	@echo "MCC - Micro C Compiler Build System"
	@echo ""
	@echo "Build Targets:"
	@echo "  all              Build the compiler (default)"
	@echo "  clean            Remove build artifacts and test outputs"
	@echo "  install          Install to parent project bin/"
	@echo ""
	@echo "Syntax Test Targets:"
	@echo "  test             Run all syntax tests (alias for test-syntax)"
	@echo "  test-syntax      Run all syntax tests"
	@echo "  test-syntax-c89  Run C89 syntax tests only"
	@echo "  test-syntax-c99  Run C99 syntax tests only"
	@echo "  test-syntax-c11  Run C11 syntax tests only"
	@echo "  test-syntax-c23  Run C23 syntax tests only"
	@echo "  test-syntax-gnu  Run GNU extension syntax tests"
	@echo "  test-syntax-legacy  Run legacy tests (tests/*.c)"
	@echo "  test-negative     Require unsupported/invalid programs to fail"
	@echo "  test-unit         Run allocator/target contract unit tests"
	@echo ""
	@echo "Cross-Standard Test Targets:"
	@echo "  test-cross       Run cross-standard warning tests"
	@echo ""
	@echo "Code Generation Test Targets:"
	@echo "  test-codegen     Run basic code generation tests"
	@echo "  test-codegen-basic     Compile C89 tests to assembly"
	@echo "  test-codegen-multifile Multi-file compilation test"
	@echo "  test-codegen-x86_64    x86_64 code generation"
	@echo "  test-codegen-arm64     ARM64 code generation"
	@echo "  test-codegen-arm64-macos  ARM64 macOS code generation"
	@echo "  test-codegen-ppc32     PowerPC 32-bit code generation"
	@echo "  test-codegen-ppc64     PowerPC 64-bit code generation"
	@echo "  test-codegen-ppc64le   PowerPC 64-bit little-endian code generation"
	@echo "  test-codegen-s370      S/370 code generation"
	@echo "  test-codegen-all-arch  All architecture tests"
	@echo "  test-run         Compile and run tests (host only)"
	@echo ""
	@echo "Combined Test Targets:"
	@echo "  test-all         Run all tests (syntax + codegen + cross)"
	@echo "  test-quick       Quick test (C89 syntax only)"
	@echo ""
	@echo "Cleanup:"
	@echo "  clean-tests      Remove test output files only"

.PHONY: all clean install test test-all test-quick help
.PHONY: test-syntax test-syntax-c89 test-syntax-c99 test-syntax-c11 test-syntax-c23 test-syntax-gnu test-syntax-legacy
.PHONY: test-cross test-negative test-unit test-build-deps
.PHONY: test-codegen test-codegen-basic test-codegen-multifile
.PHONY: test-codegen-x86_64 test-codegen-arm64 test-codegen-arm64-macos
.PHONY: test-codegen-ppc32 test-codegen-ppc64 test-codegen-ppc64le test-codegen-s370 test-codegen-all-arch
.PHONY: test-run clean-tests
.PHONY: test-layout-matrix
