# MCC - Micro C Compiler
# Test Rules - Syntax Tests
#
# This file contains targets for running syntax-only tests.
# Tests are organized to run through ALL test directories.

# ============================================================
# Helper function to run syntax tests on a directory
# Usage: $(call run_syntax_tests,directory,standard,label)
# ============================================================

define run_syntax_tests
	@echo "=== $(3) Syntax Tests ($(1)) ==="
	@passed=0; failed=0; \
	for f in $(1)/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-50s " "$$f"; \
			if ./$(TARGET) -std=$(2) -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  $(3): $$passed passed, $$failed failed"; \
	echo ""; \
	test "$$failed" -eq 0
endef

# ============================================================
# Individual Syntax Test Targets
# ============================================================

# C89 syntax tests
test-syntax-c89: $(TARGET)
	$(call run_syntax_tests,$(TESTDIR)/c89,c89,C89)

# C99 syntax tests
test-syntax-c99: $(TARGET)
	$(call run_syntax_tests,$(TESTDIR)/c99,c99,C99)

# C11 syntax tests
test-syntax-c11: $(TARGET)
	$(call run_syntax_tests,$(TESTDIR)/c11,c11,C11)

# C23 syntax tests
test-syntax-c23: $(TARGET)
	$(call run_syntax_tests,$(TESTDIR)/c23,c23,C23)

# GNU extension syntax tests
test-syntax-gnu: $(TARGET)
	$(call run_syntax_tests,$(TESTDIR)/gnu,gnu99,GNU)

# Legacy syntax tests (tests/*.c)
test-syntax-legacy: $(TARGET)
	$(call run_syntax_tests,$(TESTDIR),c99,Legacy)

# Unsupported or invalid constructs must fail, rather than becoming syntax-only
# false greens after tokens or semantics are discarded.
test-negative: $(TARGET)
	@echo "=== Negative capability/semantic tests ==="
	@passed=0; failed=0; \
	for f in $(TESTDIR)/negative/*.c; do \
		case "$$f" in \
		*c23_*) std=c23 ;; \
		*c11_*|*ambiguous_*) std=c11 ;; \
		*c99_*) std=c99 ;; \
		*gnu_*) std=gnu99 ;; \
		*include_next*|*pragma_operator*) std=gnu99 ;; \
			*) std=c89 ;; \
		esac; \
		printf "  %-50s " "$$f"; \
		if ./$(TARGET) -std=$$std -I includes -fsyntax-only "$$f" >/dev/null 2>&1; then \
			echo "FALSE GREEN"; failed=$$((failed+1)); \
		else \
			echo "REJECTED"; passed=$$((passed+1)); \
		fi; \
	done; \
	echo "  Negative: $$passed rejected, $$failed false greens"; \
	test "$$failed" -eq 0
	@if ./$(TARGET) -I >/dev/null 2>&1; then \
		echo "  -I missing argument: FALSE GREEN"; exit 1; \
	else echo "  -I missing argument: REJECTED"; fi
	@if ./$(TARGET) -D >/dev/null 2>&1; then \
		echo "  -D missing argument: FALSE GREEN"; exit 1; \
	else echo "  -D missing argument: REJECTED"; fi

test-unit:
	@mkdir -p $(OBJDIR)
	@$(CC) $(CFLAGS) $(TESTDIR)/unit/context_target.c src/context.c src/c_std.c \
		-o $(OBJDIR)/context_target_test
	@$(OBJDIR)/context_target_test
	@$(CC) $(CFLAGS) $(TESTDIR)/unit/layout.c src/context.c src/c_std.c \
		src/types.c src/codegen/codegen_type.c $(ANVIL_LIB) \
		-o $(OBJDIR)/layout_test
	@$(OBJDIR)/layout_test
	@echo "MCC context/allocator unit tests passed"

test-build-deps: $(TARGET)
	@test -s $(OBJDIR)/parse_parse_decl.d
	@grep -q 'include/parser.h' $(OBJDIR)/parse_parse_decl.d
	@echo "MCC generated header dependencies present"

# ============================================================
# Combined Syntax Test Targets
# ============================================================

# Run all syntax tests (all directories)
test-syntax: $(TARGET)
	@echo "=========================================="
	@echo "Running ALL Syntax Tests"
	@echo "=========================================="
	@echo ""
	$(call run_syntax_tests,$(TESTDIR)/c89,c89,C89)
	$(call run_syntax_tests,$(TESTDIR)/c99,c99,C99)
	$(call run_syntax_tests,$(TESTDIR)/c11,c11,C11)
	$(call run_syntax_tests,$(TESTDIR)/c23,c23,C23)
	$(call run_syntax_tests,$(TESTDIR)/gnu,gnu99,GNU)
	$(call run_syntax_tests,$(TESTDIR),c99,Legacy)
	@echo "=========================================="
	@echo "Syntax tests complete."
	@echo "=========================================="

# ============================================================
# Cross-Standard Tests
# ============================================================

test-cross: $(TARGET)
	@echo "=========================================="
	@echo "Running Cross-Standard Tests"
	@echo "(Verify warnings for features in older standards)"
	@echo "=========================================="
	@echo ""
	@echo "=== C99 features in C89 mode ==="
	@printf "  %-50s " "tests/cross/c99_in_c89.c"
	@output=$$(./$(TARGET) -std=c89 -I includes -fsyntax-only $(TESTDIR)/cross/c99_in_c89.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings warnings)"; \
	else \
		echo "MISSING WARNINGS"; exit 1; \
	fi
	@echo ""
	@echo "=== C11 features in C99 mode ==="
	@printf "  %-50s " "tests/cross/c11_in_c99.c"
	@output=$$(./$(TARGET) -std=c99 -I includes -fsyntax-only $(TESTDIR)/cross/c11_in_c99.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|requires" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; exit 1; \
	fi
	@echo ""
	@echo "=== C11 features in C89 mode ==="
	@printf "  %-50s " "tests/cross/c11_in_c89.c"
	@output=$$(./$(TARGET) -std=c89 -I includes -fsyntax-only $(TESTDIR)/cross/c11_in_c89.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|requires" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; exit 1; \
	fi
	@echo ""
	@echo "=== C23 features in C11 mode ==="
	@printf "  %-50s " "tests/cross/c23_in_c11.c"
	@output=$$(./$(TARGET) -std=c11 -I includes -fsyntax-only $(TESTDIR)/cross/c23_in_c11.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|C23" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; exit 1; \
	fi
	@echo ""
	@echo "=== C23 features in C89 mode ==="
	@printf "  %-50s " "tests/cross/c23_in_c89.c"
	@output=$$(./$(TARGET) -std=c89 -I includes -fsyntax-only $(TESTDIR)/cross/c23_in_c89.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|C23" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; exit 1; \
	fi
	@echo ""
	@echo "Cross-standard tests complete."

# ============================================================
# Backward Compatibility Alias
# ============================================================

test: test-syntax
