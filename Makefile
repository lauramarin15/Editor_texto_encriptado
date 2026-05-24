CC     = gcc
STD    = -std=c11
WARN   = -Wall -Wextra -Wpedantic
OPT    = -O2 -g
INC    = -I./include

# ── Detectar macOS vs Linux ──────────────────────────────────────
UNAME := $(shell uname)

ifeq ($(UNAME), Darwin)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /usr/local)
    INC     += -I$(BREW_PREFIX)/include
    LDFLAGS  = -L$(BREW_PREFIX)/lib -llz4
    PLATFORM = macOS
else
    LDFLAGS  = -llz4
    PLATFORM = Linux
endif

CFLAGS = $(WARN) $(STD) $(INC) $(OPT)

SRC_DIR = src
OBJ_DIR = build

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/gap_buffer.c \
       $(SRC_DIR)/compress.c \
       $(SRC_DIR)/io.c \
       $(SRC_DIR)/format.c \
	   $(SRC_DIR)/crypto.c

OBJS      = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o,      $(SRCS))
OBJS_MMAP = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%_mmap.o, $(SRCS))

TARGET      = editor
TARGET_MMAP = editor_mmap

.PHONY: all clean valgrind test benchmark help

all: $(OBJ_DIR) $(TARGET) $(TARGET_MMAP)
	@echo ""
	@echo "Compilado para $(PLATFORM)"
	@echo "  Binarios: ./$(TARGET) (fd+write)  ./$(TARGET_MMAP) (mmap)"

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -DUSE_MMAP=0 -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -DUSE_MMAP=0 -c $< -o $@

$(TARGET_MMAP): $(OBJS_MMAP)
	$(CC) $(CFLAGS) -DUSE_MMAP=1 -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%_mmap.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -DUSE_MMAP=1 -c $< -o $@

valgrind: $(TARGET)
ifeq ($(UNAME), Darwin)
	@echo "=== macOS: usando leaks ==="
	MallocStackLogging=1 leaks --atExit -- ./$(TARGET) view /dev/null 2>/dev/null || true
else
	valgrind --leak-check=full --show-leak-kinds=all \
	         --track-origins=yes --error-exitcode=1 \
	         ./$(TARGET) view /dev/null 2>/dev/null || true
endif

test: $(TARGET)
	@echo "=== Smoke test ==="
	./$(TARGET) view /dev/null 2>/dev/null && echo "PASS" || echo "SKIP"

benchmark: $(TARGET) $(TARGET_MMAP)
	@bash benchmark.sh

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(TARGET_MMAP)

help:
	@echo "make all        compilar"
	@echo "make test       smoke tests"
	@echo "make valgrind   analisis de memoria (Linux) / leaks (macOS)"
	@echo "make benchmark  benchmark con dtruss/strace + time"
	@echo "make clean      limpiar"
