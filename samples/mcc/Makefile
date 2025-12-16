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
	@echo "Running tests..."
	@echo "=== C89 Tests ==="
	@for f in tests/hello.c tests/arithmetic.c tests/bitwise.c tests/control_flow.c \
	          tests/simple_bitwise.c tests/simple_struct.c tests/typedef_simple.c \
	          tests/typedef_multi.c tests/advanced_types.c tests/preprocessor_test.c \
	          tests/line_continuation_test.c tests/multiline_macro_test.c; do \
		if [ -f "$$f" ]; then \
			echo -n "Testing $$f... "; \
			./$(TARGET) -I includes -fsyntax-only $$f 2>/dev/null && echo "OK" || echo "FAILED"; \
		fi; \
	done
	@echo "=== C99 Tests ==="
	@for f in tests/c99_keywords_test.c tests/line_comment_test.c; do \
		if [ -f "$$f" ]; then \
			echo -n "Testing $$f... "; \
			./$(TARGET) -std=c99 -I includes -fsyntax-only $$f 2>/dev/null && echo "OK" || echo "FAILED"; \
		fi; \
	done
	@echo "=== C11 Tests ==="
	@for f in tests/c11_keywords_test.c tests/generic_test.c; do \
		if [ -f "$$f" ]; then \
			echo -n "Testing $$f... "; \
			./$(TARGET) -std=c11 -I includes -fsyntax-only $$f 2>/dev/null && echo "OK" || echo "FAILED"; \
		fi; \
	done
	@echo "=== Tests with includes ==="
	@for f in tests/struct_test.c; do \
		if [ -f "$$f" ]; then \
			echo -n "Testing $$f... "; \
			./$(TARGET) -I includes -fsyntax-only $$f 2>/dev/null && echo "OK" || echo "FAILED"; \
		fi; \
	done

# Install (to parent project)
install: $(TARGET)
	cp $(TARGET) ../../bin/

.PHONY: all clean test install
