# MCC - Micro C Compiler
# Test Rules - Code Generation Tests
#
# This file contains targets for running code generation tests.
# Tests are organized to run through ALL test directories.

# Output directory for generated assembly
TEST_OUTPUT_DIR = tests/output

# ============================================================
# Helper function to run codegen tests on a directory
# Usage: $(call run_codegen_tests,directory,standard,label)
# ============================================================

define run_codegen_tests
	@echo "=== $(3) Code Generation Tests ($(1)) ==="
	@passed=0; failed=0; \
	for f in $(1)/*.c; do \
		if [ -f "$$f" ]; then \
			base=$$(basename "$$f" .c); \
			printf "  %-50s " "$$f"; \
			if ./$(TARGET) -std=$(2) -I includes -o $(TEST_OUTPUT_DIR)/$$base.s "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  $(3): $$passed passed, $$failed failed"; \
	echo ""
endef

# ============================================================
# Setup
# ============================================================

# Create output directory
$(TEST_OUTPUT_DIR):
	mkdir -p $(TEST_OUTPUT_DIR)

# ============================================================
# Individual Code Generation Test Targets
# ============================================================

# C89 codegen tests
test-codegen-c89: $(TARGET) | $(TEST_OUTPUT_DIR)
	$(call run_codegen_tests,$(TESTDIR)/c89,c89,C89)

# C99 codegen tests
test-codegen-c99: $(TARGET) | $(TEST_OUTPUT_DIR)
	$(call run_codegen_tests,$(TESTDIR)/c99,c99,C99)

# C11 codegen tests
test-codegen-c11: $(TARGET) | $(TEST_OUTPUT_DIR)
	$(call run_codegen_tests,$(TESTDIR)/c11,c11,C11)

# C23 codegen tests
test-codegen-c23: $(TARGET) | $(TEST_OUTPUT_DIR)
	$(call run_codegen_tests,$(TESTDIR)/c23,c23,C23)

# GNU extension codegen tests
test-codegen-gnu: $(TARGET) | $(TEST_OUTPUT_DIR)
	$(call run_codegen_tests,$(TESTDIR)/gnu,gnu99,GNU)

# Legacy codegen tests (tests/*.c)
test-codegen-legacy: $(TARGET) | $(TEST_OUTPUT_DIR)
	$(call run_codegen_tests,$(TESTDIR),c99,Legacy)

# ============================================================
# Combined Code Generation Test Targets
# ============================================================

# Run all codegen tests (all directories)
test-codegen: $(TARGET) | $(TEST_OUTPUT_DIR)
	@echo "=========================================="
	@echo "Running ALL Code Generation Tests"
	@echo "=========================================="
	@echo ""
	$(call run_codegen_tests,$(TESTDIR)/c89,c89,C89)
	$(call run_codegen_tests,$(TESTDIR)/c99,c99,C99)
	$(call run_codegen_tests,$(TESTDIR)/c11,c11,C11)
	$(call run_codegen_tests,$(TESTDIR)/c23,c23,C23)
	$(call run_codegen_tests,$(TESTDIR)/gnu,gnu99,GNU)
	$(call run_codegen_tests,$(TESTDIR),c99,Legacy)
	@echo "=========================================="
	@echo "Code generation tests complete."
	@echo "=========================================="

# Basic codegen tests (alias for backward compatibility)
test-codegen-basic: test-codegen-c89

# ============================================================
# Multi-File Compilation Tests
# ============================================================

test-codegen-multifile: $(TARGET) | $(TEST_OUTPUT_DIR)
	@echo "=== Multi-File Compilation Tests ==="
	@printf "  %-50s " "tests/multi_file/"
	@if ./$(TARGET) -std=c99 -I includes -o $(TEST_OUTPUT_DIR)/multi_file.s \
		$(TESTDIR)/multi_file/main_test.c \
		$(TESTDIR)/multi_file/math_funcs.c \
		$(TESTDIR)/multi_file/utils.c 2>/dev/null; then \
		echo "OK"; \
	else \
		echo "FAILED"; \
	fi
	@echo ""

# ============================================================
# Architecture-Specific Code Generation Tests
# ============================================================

# Helper for architecture-specific tests
define run_arch_codegen_tests
	@echo "=== $(2) Code Generation Tests ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/c89/*.c $(TESTDIR)/c99/*.c $(TESTDIR)/*.c; do \
		if [ -f "$$f" ]; then \
			base=$$(basename "$$f" .c); \
			dir=$$(dirname "$$f" | sed 's|.*/||'); \
			printf "  %-50s " "$$f"; \
			if ./$(TARGET) -arch=$(1) -std=c99 -I includes -o $(TEST_OUTPUT_DIR)/$$base.$(1).s "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  $(2): $$passed passed, $$failed failed"; \
	echo ""
endef

# Test x86_64 code generation
test-codegen-x86_64: $(TARGET) | $(TEST_OUTPUT_DIR)
	$(call run_arch_codegen_tests,x86_64,x86_64)

# Test ARM64 code generation
test-codegen-arm64: $(TARGET) | $(TEST_OUTPUT_DIR)
	$(call run_arch_codegen_tests,arm64,ARM64)

# Test ARM64 macOS code generation
test-codegen-arm64-macos: $(TARGET) | $(TEST_OUTPUT_DIR)
	$(call run_arch_codegen_tests,arm64_macos,ARM64-macOS)

# Test S/370 code generation
test-codegen-s370: $(TARGET) | $(TEST_OUTPUT_DIR)
	$(call run_arch_codegen_tests,s370,S/370)

# Test all architectures
test-codegen-all-arch: test-codegen-x86_64 test-codegen-arm64 test-codegen-arm64-macos test-codegen-s370
	@echo ""
	@echo "All architecture tests complete."

# ============================================================
# Execution Tests (compile and run)
# ============================================================

test-run: $(TARGET) | $(TEST_OUTPUT_DIR)
	@echo "=== Execution Tests ==="
	@echo "Note: These tests compile and run code on the host system."
	@echo "Requires: assembler (as/gas) and linker (ld/gcc)"
	@echo ""
	@if [ -f $(TESTDIR)/hello.c ]; then \
		printf "  %-50s " "tests/hello.c"; \
		if ./$(TARGET) -std=c99 -I includes -o $(TEST_OUTPUT_DIR)/hello.s $(TESTDIR)/hello.c 2>/dev/null && \
		   gcc -c $(TEST_OUTPUT_DIR)/hello.s -o $(TEST_OUTPUT_DIR)/hello.o 2>/dev/null && \
		   gcc $(TEST_OUTPUT_DIR)/hello.o -o $(TEST_OUTPUT_DIR)/hello 2>/dev/null; then \
			output=$$($(TEST_OUTPUT_DIR)/hello 2>&1); \
			if echo "$$output" | grep -q "Hello"; then \
				echo "OK (output: $$output)"; \
			else \
				echo "WRONG OUTPUT: $$output"; \
			fi; \
		else \
			echo "COMPILE FAILED"; \
		fi; \
	fi
	@echo ""

# ============================================================
# Cleanup
# ============================================================

clean-tests:
	rm -rf $(TEST_OUTPUT_DIR)
