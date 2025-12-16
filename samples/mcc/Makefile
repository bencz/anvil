# MCC - Micro C Compiler
# Makefile

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -I../../include -Iinclude -Isrc/preprocessor -Isrc/lexer -Isrc/parser -Isrc/sema
LDFLAGS = -L../../lib -lanvil

# Directories
SRCDIR = src
INCDIR = include
OBJDIR = obj
BINDIR = .
PP_SRCDIR = src/preprocessor
LEX_SRCDIR = src/lexer
PARSE_SRCDIR = src/parser
SEMA_SRCDIR = src/sema

# Source files (excluding old preprocessor.c, lexer.c, parser.c, sema.c - using new modular versions)
SRCS = $(filter-out $(SRCDIR)/preprocessor.c $(SRCDIR)/lexer.c $(SRCDIR)/parser.c $(SRCDIR)/sema.c, $(wildcard $(SRCDIR)/*.c))
PP_SRCS = $(wildcard $(PP_SRCDIR)/*.c)
LEX_SRCS = $(wildcard $(LEX_SRCDIR)/*.c)
PARSE_SRCS = $(wildcard $(PARSE_SRCDIR)/*.c)
SEMA_SRCS = $(wildcard $(SEMA_SRCDIR)/*.c)

# Object files
OBJS = $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
PP_OBJS = $(PP_SRCS:$(PP_SRCDIR)/%.c=$(OBJDIR)/pp_%.o)
LEX_OBJS = $(LEX_SRCS:$(LEX_SRCDIR)/%.c=$(OBJDIR)/lex_%.o)
PARSE_OBJS = $(PARSE_SRCS:$(PARSE_SRCDIR)/%.c=$(OBJDIR)/parse_%.o)
SEMA_OBJS = $(SEMA_SRCS:$(SEMA_SRCDIR)/%.c=$(OBJDIR)/sema_%.o)
ALL_OBJS = $(OBJS) $(PP_OBJS) $(LEX_OBJS) $(PARSE_OBJS) $(SEMA_OBJS)

# Target
TARGET = $(BINDIR)/mcc

# Default target
all: $(TARGET)

# Create directories
$(OBJDIR):
	mkdir -p $(OBJDIR)

# Link
$(TARGET): $(ALL_OBJS)
	$(CC) $(ALL_OBJS) -o $@ $(LDFLAGS)

# Compile main source files
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile preprocessor source files
$(OBJDIR)/pp_%.o: $(PP_SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile lexer source files
$(OBJDIR)/lex_%.o: $(LEX_SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile parser source files
$(OBJDIR)/parse_%.o: $(PARSE_SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile sema source files
$(OBJDIR)/sema_%.o: $(SEMA_SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Dependencies - Main source files
$(OBJDIR)/main.o: $(INCDIR)/mcc.h $(INCDIR)/c_std.h
$(OBJDIR)/lexer.o: $(INCDIR)/lexer.h $(INCDIR)/token.h $(INCDIR)/c_std.h
$(OBJDIR)/parser.o: $(INCDIR)/parser.h $(INCDIR)/ast.h $(INCDIR)/lexer.h $(INCDIR)/c_std.h
$(OBJDIR)/ast.o: $(INCDIR)/ast.h
$(OBJDIR)/types.o: $(INCDIR)/types.h
$(OBJDIR)/symtab.o: $(INCDIR)/symtab.h $(INCDIR)/types.h
$(OBJDIR)/codegen.o: $(INCDIR)/codegen.h $(INCDIR)/ast.h
$(OBJDIR)/c_std.o: $(INCDIR)/c_std.h $(INCDIR)/mcc.h
$(OBJDIR)/context.o: $(INCDIR)/mcc.h $(INCDIR)/c_std.h

# Dependencies - Preprocessor source files
PP_INTERNAL_H = $(PP_SRCDIR)/pp_internal.h
$(OBJDIR)/pp_preprocessor.o: $(PP_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/pp_macro.o: $(PP_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/pp_expr.o: $(PP_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/pp_directive.o: $(PP_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/pp_include.o: $(PP_INTERNAL_H) $(INCDIR)/mcc.h

# Dependencies - Lexer source files
LEX_INTERNAL_H = $(LEX_SRCDIR)/lex_internal.h
$(OBJDIR)/lex_lexer.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_token.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_char.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_comment.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_string.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_number.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_ident.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_operator.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h

# Dependencies - Parser source files
PARSE_INTERNAL_H = $(PARSE_SRCDIR)/parse_internal.h
$(OBJDIR)/parse_parser.o: $(PARSE_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/parse_expr.o: $(PARSE_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/parse_stmt.o: $(PARSE_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/parse_type.o: $(PARSE_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/parse_decl.o: $(PARSE_INTERNAL_H) $(INCDIR)/mcc.h

# Dependencies - Sema source files
SEMA_INTERNAL_H = $(SEMA_SRCDIR)/sema_internal.h
$(OBJDIR)/sema_sema.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/sema_expr.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/sema_stmt.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/sema_decl.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/sema_type.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/sema_const.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h

# Clean
clean:
	rm -rf $(OBJDIR) $(TARGET)

# Run tests
test: $(TARGET)
	@echo "Running MCC Test Suite..."
	@echo ""
	@echo "=== C89 Tests (tests/c89/) ==="
	@passed=0; failed=0; \
	for f in tests/c89/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-40s " "$$f"; \
			if ./$(TARGET) -std=c89 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  C89: $$passed passed, $$failed failed"
	@echo ""
	@echo "=== C99 Tests (tests/c99/) ==="
	@passed=0; failed=0; \
	for f in tests/c99/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-40s " "$$f"; \
			if ./$(TARGET) -std=c99 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  C99: $$passed passed, $$failed failed"
	@echo ""
	@echo "=== C11 Tests (tests/c11/) ==="
	@passed=0; failed=0; \
	for f in tests/c11/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-40s " "$$f"; \
			if ./$(TARGET) -std=c11 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  C11: $$passed passed, $$failed failed"
	@echo ""
	@echo "=== C23 Tests (tests/c23/) ==="
	@passed=0; failed=0; \
	for f in tests/c23/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-40s " "$$f"; \
			if ./$(TARGET) -std=c23 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  C23: $$passed passed, $$failed failed"
	@echo ""
	@echo "=== GNU Extension Tests (tests/gnu/) ==="
	@passed=0; failed=0; \
	for f in tests/gnu/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-40s " "$$f"; \
			if ./$(TARGET) -std=gnu99 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  GNU: $$passed passed, $$failed failed"
	@echo ""
	@echo "=== Legacy Tests (tests/*.c) ==="
	@passed=0; failed=0; \
	for f in tests/*.c; do \
		if [ -f "$$f" ]; then \
			printf "  %-40s " "$$f"; \
			if ./$(TARGET) -std=c99 -I includes -fsyntax-only "$$f" 2>/dev/null; then \
				echo "OK"; passed=$$((passed+1)); \
			else \
				echo "FAILED"; failed=$$((failed+1)); \
			fi; \
		fi; \
	done; \
	echo "  Legacy: $$passed passed, $$failed failed"
	@echo ""
	@echo "Test suite complete."

# Cross-standard tests (verify warnings for features used in older standards)
test-cross: $(TARGET)
	@echo "Running Cross-Standard Tests..."
	@echo "(These tests verify that warnings are emitted for newer features in older standards)"
	@echo ""
	@echo "=== C99 features in C89 mode ==="
	@printf "  %-40s " "tests/cross/c99_in_c89.c"
	@output=$$(./$(TARGET) -std=c89 -I includes -fsyntax-only tests/cross/c99_in_c89.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings warnings)"; \
	else \
		echo "MISSING WARNINGS"; \
	fi
	@echo ""
	@echo "=== C11 features in C99 mode ==="
	@printf "  %-40s " "tests/cross/c11_in_c99.c"
	@output=$$(./$(TARGET) -std=c99 -I includes -fsyntax-only tests/cross/c11_in_c99.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|requires" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; \
	fi
	@echo ""
	@echo "=== C11 features in C89 mode ==="
	@printf "  %-40s " "tests/cross/c11_in_c89.c"
	@output=$$(./$(TARGET) -std=c89 -I includes -fsyntax-only tests/cross/c11_in_c89.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|requires" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; \
	fi
	@echo ""
	@echo "=== C23 features in C11 mode ==="
	@printf "  %-40s " "tests/cross/c23_in_c11.c"
	@output=$$(./$(TARGET) -std=c11 -I includes -fsyntax-only tests/cross/c23_in_c11.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|C23" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; \
	fi
	@echo ""
	@echo "=== C23 features in C89 mode ==="
	@printf "  %-40s " "tests/cross/c23_in_c89.c"
	@output=$$(./$(TARGET) -std=c89 -I includes -fsyntax-only tests/cross/c23_in_c89.c 2>&1); \
	warnings=$$(echo "$$output" | grep -c "warning\|extension\|error\|C23" || true); \
	if [ "$$warnings" -gt 0 ]; then \
		echo "OK ($$warnings diagnostics)"; \
	else \
		echo "MISSING WARNINGS"; \
	fi
	@echo ""
	@echo "Cross-standard tests complete."

# Run all tests including cross-standard
test-all: test test-cross

# Install (to parent project)
install: $(TARGET)
	cp $(TARGET) ../../bin/

.PHONY: all clean test test-cross test-all install
