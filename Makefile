# Ethernos Tiny C Compiler (ETCC)
# Build the compiler with MinGW GCC and the freestanding runtime with Clang.
#
#   mingw32-make            build compiler + runtime
#   mingw32-make test       run the test suite
#   mingw32-make clean

CC       ?= gcc
CFLAGS   ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude

CLANG     ?= clang
LLVM_LIB  ?= llvm-lib
PYTHON    ?= python

BUILD_DIR     := build
RUNTIME_BUILD := $(BUILD_DIR)/runtime
ETCC          := $(BUILD_DIR)/etcc.exe
RUNTIME_LIB   := $(RUNTIME_BUILD)/etccrt.lib

COMPILER_SOURCES := $(wildcard src/*.c)
COMPILER_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(COMPILER_SOURCES))
HEADERS          := $(wildcard include/etcc/*.h)

RUNTIME_SOURCES := runtime/src/crt0.c runtime/src/string.c \
                   runtime/src/stdlib.c runtime/src/stdio.c
RUNTIME_OBJECTS := $(patsubst runtime/src/%.c,$(RUNTIME_BUILD)/%.obj,$(RUNTIME_SOURCES))
RUNTIME_CFLAGS  := --target=x86_64-pc-windows-msvc -c -O2 -ffreestanding \
                   -fno-builtin -fno-stack-protector \
                   -fno-asynchronous-unwind-tables -Wall -Wextra -Wpedantic \
                   -Iruntime/src

ifeq ($(OS),Windows_NT)
MKDIR_BUILD   := if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
MKDIR_RUNTIME := if not exist "$(RUNTIME_BUILD)" mkdir "$(RUNTIME_BUILD)"
REMOVE_BUILD  := if exist "$(BUILD_DIR)" rmdir /s /q "$(BUILD_DIR)"
else
MKDIR_BUILD   := mkdir -p "$(BUILD_DIR)"
MKDIR_RUNTIME := mkdir -p "$(RUNTIME_BUILD)"
REMOVE_BUILD  := rm -rf "$(BUILD_DIR)"
endif

.PHONY: all test clean runtime
all: $(ETCC) $(RUNTIME_LIB)

$(BUILD_DIR):
	$(MKDIR_BUILD)

$(RUNTIME_BUILD):
	$(MKDIR_RUNTIME)

$(BUILD_DIR)/%.o: src/%.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(ETCC): $(COMPILER_OBJECTS)
	$(CC) $(COMPILER_OBJECTS) -o $@

$(RUNTIME_BUILD)/%.obj: runtime/src/%.c runtime/src/rt_internal.h | $(RUNTIME_BUILD)
	$(CLANG) $(RUNTIME_CFLAGS) $< -o $@

$(RUNTIME_LIB): $(RUNTIME_OBJECTS)
	$(LLVM_LIB) /nologo /out:$@ $(RUNTIME_OBJECTS)

test: all
	$(PYTHON) tests/run_tests.py

clean:
	$(REMOVE_BUILD)
