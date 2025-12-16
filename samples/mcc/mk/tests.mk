# MCC - Micro C Compiler
# Test Rules - Syntax Tests
#
# This file contains targets for running syntax-only tests.

# ============================================================
# Syntax Tests (using -fsyntax-only)
# ============================================================

# Run all syntax tests
test-syntax: test-syntax-c89 test-syntax-c99 test-syntax-c11 test-syntax-c23 test-syntax-gnu test-syntax-legacy
	@echo ""
	@echo "Syntax tests complete."

# C89 syntax tests
test-syntax-c89: $(TARGET)
	@echo "=== C89 Syntax Tests (tests/c89/) ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/c89/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-45s " "$$f"; \
			if ./$(TARGET) -std=c89 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  C89: $$passed passed, $$failed failed"; \
	echo ""

# C99 syntax tests
test-syntax-c99: $(TARGET)
	@echo "=== C99 Syntax Tests (tests/c99/) ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/c99/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-45s " "$$f"; \
			if ./$(TARGET) -std=c99 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  C99: $$passed passed, $$failed failed"; \
	echo ""

# C11 syntax tests
test-syntax-c11: $(TARGET)
	@echo "=== C11 Syntax Tests (tests/c11/) ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/c11/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-45s " "$$f"; \
			if ./$(TARGET) -std=c11 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  C11: $$passed passed, $$failed failed"; \
	echo ""

# C23 syntax tests
test-syntax-c23: $(TARGET)
	@echo "=== C23 Syntax Tests (tests/c23/) ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/c23/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-45s " "$$f"; \
			if ./$(TARGET) -std=c23 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  C23: $$passed passed, $$failed failed"; \
	echo ""

# GNU extension syntax tests
test-syntax-gnu: $(TARGET)
	@echo "=== GNU Extension Syntax Tests (tests/gnu/) ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/gnu/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-45s " "$$f"; \
			if ./$(TARGET) -std=gnu99 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  GNU: $$passed passed, $$failed failed"; \
	echo ""

# Legacy syntax tests (tests/*.c)
test-syntax-legacy: $(TARGET)
	@echo "=== Legacy Syntax Tests (tests/*.c) ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-45s " "$$f"; \
			if ./$(TARGET) -std=c99 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  Legacy: $$passed passed, $$failed failed"; \
	echo ""

# ============================================================
# Cross-Standard Tests
# ============================================================

test-cross: $(TARGET)
	@echo "Running Cross-Standard Tests..."
	@echo "(Verify warnings for features used in older standards)"
	@echo ""
	@echo "=== C99 features in C89 mode ==="
	@printf "  %-45s " "tests/cross/c99_in_c89.c"
	@output=$$(./$(TARGET) -std=c89 -I includes -fsyntax-only $(TESTDIR)/cross/c99_in_c89.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings warnings)"; \
	else \
		echo "MISSING WARNINGS"; \
	fi
	@echo ""
	@echo "=== C11 features in C99 mode ==="
	@printf "  %-45s " "tests/cross/c11_in_c99.c"
	@output=$$(./$(TARGET) -std=c99 -I includes -fsyntax-only $(TESTDIR)/cross/c11_in_c99.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|requires" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; \
	fi
	@echo ""
	@echo "=== C11 features in C89 mode ==="
	@printf "  %-45s " "tests/cross/c11_in_c89.c"
	@output=$$(./$(TARGET) -std=c89 -I includes -fsyntax-only $(TESTDIR)/cross/c11_in_c89.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|requires" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; \
	fi
	@echo ""
	@echo "=== C23 features in C11 mode ==="
	@printf "  %-45s " "tests/cross/c23_in_c11.c"
	@output=$$(./$(TARGET) -std=c11 -I includes -fsyntax-only $(TESTDIR)/cross/c23_in_c11.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|C23" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; \
	fi
	@echo ""
	@echo "=== C23 features in C89 mode ==="
	@printf "  %-45s " "tests/cross/c23_in_c89.c"
	@output=$$(./$(TARGET) -std=c89 -I includes -fsyntax-only $(TESTDIR)/cross/c23_in_c89.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|C23" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; \
	fi
	@echo ""
	@echo "Cross-standard tests complete."

# Alias for backward compatibility
test: test-syntax
	@echo ""
	@echo "Note: 'make test' now runs syntax tests only."
	@echo "Use 'make test-all' for all tests including codegen."
