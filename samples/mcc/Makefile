# MCC - Micro C Compiler
# Makefile

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -I../../include -Iinclude -Isrc/preprocessor -Isrc/lexer
LDFLAGS = -L../../lib -lanvil

# Directories
SRCDIR = src
INCDIR = include
OBJDIR = obj
BINDIR = .
PP_SRCDIR = src/preprocessor
LEX_SRCDIR = src/lexer

# Source files (excluding old preprocessor.c and lexer.c, using new modular versions)
SRCS = $(filter-out $(SRCDIR)/preprocessor.c $(SRCDIR)/lexer.c, $(wildcard $(SRCDIR)/*.c))
PP_SRCS = $(wildcard $(PP_SRCDIR)/*.c)
LEX_SRCS = $(wildcard $(LEX_SRCDIR)/*.c)

# Object files
OBJS = $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
PP_OBJS = $(PP_SRCS:$(PP_SRCDIR)/%.c=$(OBJDIR)/pp_%.o)
LEX_OBJS = $(LEX_SRCS:$(LEX_SRCDIR)/%.c=$(OBJDIR)/lex_%.o)
ALL_OBJS = $(OBJS) $(PP_OBJS) $(LEX_OBJS)

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

# Dependencies - Main source files
$(OBJDIR)/main.o: $(INCDIR)/mcc.h $(INCDIR)/c_std.h
$(OBJDIR)/lexer.o: $(INCDIR)/lexer.h $(INCDIR)/token.h $(INCDIR)/c_std.h
$(OBJDIR)/parser.o: $(INCDIR)/parser.h $(INCDIR)/ast.h $(INCDIR)/lexer.h $(INCDIR)/c_std.h
$(OBJDIR)/ast.o: $(INCDIR)/ast.h
$(OBJDIR)/types.o: $(INCDIR)/types.h
$(OBJDIR)/symtab.o: $(INCDIR)/symtab.h $(INCDIR)/types.h
$(OBJDIR)/sema.o: $(INCDIR)/sema.h $(INCDIR)/ast.h $(INCDIR)/symtab.h $(INCDIR)/c_std.h
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

# Clean
clean:
	rm -rf $(OBJDIR) $(TARGET)

# Run tests
test: $(TARGET)
	@echo "Running tests..."
	@for f in tests/*.c; do \
		echo "Testing $$f..."; \
		./$(TARGET) -fsyntax-only $$f && echo "  OK" || echo "  FAILED"; \
	done

# Install (to parent project)
install: $(TARGET)
	cp $(TARGET) ../../bin/

.PHONY: all clean test install
