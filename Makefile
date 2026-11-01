# ==========================================================
#  Servidor de video - Makefile
#    make              -> produccion (-O2, sin avisos)
#    make debug        -> depuracion (-g -O0, sanitizers)
#    make test         -> pruebas unitarias del protocolo
#    make clean
# ==========================================================
CC       ?= gcc
WARN     := -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L
INCLUDES := -Iinclude
LDLIBS   :=

# Modo (release por defecto). 'make debug' lo cambia.
MODE ?= release
ifeq ($(MODE),debug)
  OPT := -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
else
  OPT := -O2 -DNDEBUG
endif
CFLAGS := $(WARN) $(OPT) $(INCLUDES)

BUILD := build

COMMON_SRC  := src/common/protocol.c
CLIENTE_SRC := src/cliente/cliente.c src/cliente/args.c src/cliente/red.c \
               src/cliente/ops.c src/cliente/player.c
SERVIDOR_SRC := $(wildcard src/servidor/*.c)

COMMON_OBJ   := $(patsubst %.c,$(BUILD)/%.o,$(COMMON_SRC))
CLIENTE_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(CLIENTE_SRC))
SERVIDOR_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SERVIDOR_SRC))

all: cliente servidor

cliente: $(CLIENTE_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

servidor: $(SERVIDOR_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/%.o: %.c $(wildcard include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

debug:
	$(MAKE) clean
	$(MAKE) MODE=debug all

# ---------- pruebas ----------
UNIT_TESTS := $(patsubst tests/%.c,$(BUILD)/tests/%,$(wildcard tests/test_protocol_*.c))

# Los tests NUNCA se compilan con -DNDEBUG: si no, assert() no comprueba nada.
TEST_CFLAGS := $(filter-out -DNDEBUG,$(CFLAGS))

$(BUILD)/tests/%: tests/%.c $(COMMON_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) $^ -o $@

# Prueba del timeline (necesita timeline.c)
UNIT_TESTS += $(BUILD)/tests/test_timeline

$(BUILD)/tests/test_timeline: tests/test_timeline.c src/servidor/timeline.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) $^ -o $@

test: $(UNIT_TESTS)
	@for t in $(UNIT_TESTS); do echo "\n== $$t"; $$t || exit 1; done
	@echo "\nTodas las pruebas unitarias pasaron."

clean:
	rm -rf $(BUILD) cliente servidor

.PHONY: all debug test clean