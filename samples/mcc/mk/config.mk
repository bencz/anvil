# MCC - Micro C Compiler
# Build Configuration
#
# This file contains compiler flags, directories, and source file definitions.

# Compiler settings
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -I../../include -Iinclude -Isrc/preprocessor -Isrc/lexer -Isrc/parser -Isrc/sema -Isrc/codegen -Isrc/opt -Isrc/ast
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
CODEGEN_SRCDIR = src/codegen
OPT_SRCDIR = src/opt
AST_SRCDIR = src/ast
TESTDIR = tests

# Source files (excluding old monolithic versions - using new modular versions)
SRCS = $(filter-out $(SRCDIR)/preprocessor.c $(SRCDIR)/lexer.c $(SRCDIR)/parser.c $(SRCDIR)/sema.c $(SRCDIR)/codegen.c $(SRCDIR)/ast.c, $(wildcard $(SRCDIR)/*.c))
PP_SRCS = $(wildcard $(PP_SRCDIR)/*.c)
LEX_SRCS = $(wildcard $(LEX_SRCDIR)/*.c)
PARSE_SRCS = $(wildcard $(PARSE_SRCDIR)/*.c)
SEMA_SRCS = $(wildcard $(SEMA_SRCDIR)/*.c)
CODEGEN_SRCS = $(wildcard $(CODEGEN_SRCDIR)/*.c)
OPT_SRCS = $(wildcard $(OPT_SRCDIR)/*.c)
AST_SRCS = $(wildcard $(AST_SRCDIR)/*.c)

# Object files
OBJS = $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
PP_OBJS = $(PP_SRCS:$(PP_SRCDIR)/%.c=$(OBJDIR)/pp_%.o)
LEX_OBJS = $(LEX_SRCS:$(LEX_SRCDIR)/%.c=$(OBJDIR)/lex_%.o)
PARSE_OBJS = $(PARSE_SRCS:$(PARSE_SRCDIR)/%.c=$(OBJDIR)/parse_%.o)
SEMA_OBJS = $(SEMA_SRCS:$(SEMA_SRCDIR)/%.c=$(OBJDIR)/sema_%.o)
CODEGEN_OBJS = $(CODEGEN_SRCS:$(CODEGEN_SRCDIR)/%.c=$(OBJDIR)/cg_%.o)
OPT_OBJS = $(OPT_SRCS:$(OPT_SRCDIR)/%.c=$(OBJDIR)/opt_%.o)
AST_OBJS = $(AST_SRCS:$(AST_SRCDIR)/%.c=$(OBJDIR)/ast_%.o)
ALL_OBJS = $(OBJS) $(PP_OBJS) $(LEX_OBJS) $(PARSE_OBJS) $(SEMA_OBJS) $(CODEGEN_OBJS) $(OPT_OBJS) $(AST_OBJS)

# Target
TARGET = $(BINDIR)/mcc

# Internal headers
PP_INTERNAL_H = $(PP_SRCDIR)/pp_internal.h
LEX_INTERNAL_H = $(LEX_SRCDIR)/lex_internal.h
PARSE_INTERNAL_H = $(PARSE_SRCDIR)/parse_internal.h
SEMA_INTERNAL_H = $(SEMA_SRCDIR)/sema_internal.h
CODEGEN_INTERNAL_H = $(CODEGEN_SRCDIR)/codegen_internal.h
OPT_INTERNAL_H = $(OPT_SRCDIR)/opt_internal.h
AST_INTERNAL_H = $(AST_SRCDIR)/ast_internal.h
