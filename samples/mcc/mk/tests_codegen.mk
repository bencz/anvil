# MCC - Micro C Compiler
# Test Rules - Code Generation Tests
#
# This file contains targets for running code generation tests.
# These tests compile C code to assembly and optionally run it.

# Output directory for generated assembly
TEST_OUTPUT_DIR = tests/output

# ============================================================
# Code Generation Tests
# ============================================================

# Create output directory
$(TEST_OUTPUT_DIR):
	mkdir -p $(TEST_OUTPUT_DIR)

# Run all codegen tests
test-codegen: test-codegen-basic test-codegen-multifile
	@echo ""
	@echo "Code generation tests complete."

# Basic code generation tests - compile to assembly
test-codegen-basic: $(TARGET) | $(TEST_OUTPUT_DIR)
	@echo "=== Basic Code Generation Tests ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/c89/*.c; do \
		if [ -f "$$f" ]; then \
			base=$$(basename "$$f" .c); \
			printf "  %-45s " "$$f"; \
			if ./$(TARGET) -std=c89 -I includes -o $(TEST_OUTPUT_DIR)/$$base.s "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  Basic codegen: $$passed passed, $$failed failed"; \
	echo ""

# Multi-file compilation tests
test-codegen-multifile: $(TARGET) | $(TEST_OUTPUT_DIR)
	@echo "=== Multi-File Compilation Tests ==="
	@printf "  %-45s " "tests/multi_file/"
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

# Test x86_64 code generation
test-codegen-x86_64: $(TARGET) | $(TEST_OUTPUT_DIR)
	@echo "=== x86_64 Code Generation Tests ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/c89/*.c; do \
		if [ -f "$$f" ]; then \
			base=$$(basename "$$f" .c); \
			printf "  %-45s " "$$f"; \
			if ./$(TARGET) -arch=x86_64 -std=c89 -I includes -o $(TEST_OUTPUT_DIR)/$$base.x86_64.s "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  x86_64: $$passed passed, $$failed failed"; \
	echo ""

# Test ARM64 code generation
test-codegen-arm64: $(TARGET) | $(TEST_OUTPUT_DIR)
	@echo "=== ARM64 Code Generation Tests ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/c89/*.c; do \
		if [ -f "$$f" ]; then \
			base=$$(basename "$$f" .c); \
			printf "  %-45s " "$$f"; \
			if ./$(TARGET) -arch=arm64 -std=c89 -I includes -o $(TEST_OUTPUT_DIR)/$$base.arm64.s "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  ARM64: $$passed passed, $$failed failed"; \
	echo ""

# Test ARM64 macOS code generation
test-codegen-arm64-macos: $(TARGET) | $(TEST_OUTPUT_DIR)
	@echo "=== ARM64 macOS Code Generation Tests ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/c89/*.c; do \
		if [ -f "$$f" ]; then \
			base=$$(basename "$$f" .c); \
			printf "  %-45s " "$$f"; \
			if ./$(TARGET) -arch=arm64-macos -std=c89 -I includes -o $(TEST_OUTPUT_DIR)/$$base.arm64-macos.s "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  ARM64-macOS: $$passed passed, $$failed failed"; \
	echo ""

# Test S/370 code generation
test-codegen-s370: $(TARGET) | $(TEST_OUTPUT_DIR)
	@echo "=== S/370 Code Generation Tests ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/c89/*.c; do \
		if [ -f "$$f" ]; then \
			base=$$(basename "$$f" .c); \
			printf "  %-45s " "$$f"; \
			if ./$(TARGET) -arch=s370 -std=c89 -I includes -o $(TEST_OUTPUT_DIR)/$$base.s370.asm "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  S/370: $$passed passed, $$failed failed"; \
	echo ""

# Test all architectures
test-codegen-all-arch: test-codegen-x86_64 test-codegen-arm64 test-codegen-arm64-macos test-codegen-s370
	@echo ""
	@echo "All architecture tests complete."

# ============================================================
# Execution Tests (compile and run)
# ============================================================

# These tests require a working assembler and linker for the target platform.
# Currently only supported on the host architecture.

test-run: $(TARGET) | $(TEST_OUTPUT_DIR)
	@echo "=== Execution Tests ==="
	@echo "Note: These tests compile and run code on the host system."
	@echo "Requires: assembler (as/gas) and linker (ld/gcc)"
	@echo ""
	@if [ -f $(TESTDIR)/hello.c ]; then \
		printf "  %-45s " "tests/hello.c"; \
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

# Clean test outputs
clean-tests:
	rm -rf $(TEST_OUTPUT_DIR)
