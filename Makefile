# ==========================================================
#  Servidor de video - Makefile
#    make              -> produccion (-O2, sin avisos)
#    make debug        -> depuracion (-g -O0, sanitizers)
#    make test         -> pruebas unitarias del protocolo
#    make test-integracion -> pruebas de integracion (servidor+cliente
#                             reales: concurrencia, cliente contra mock,
#                             sincronia del canal). Mas lentas (~1 min);
#                             no corren con 'make test' para no demorar
#                             el ciclo rapido de desarrollo.
#    make test-all     -> test + test-integracion
#    make clean
# ==========================================================
CC       ?= gcc
WARN     := -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L
INCLUDES := -Iinclude
LDLIBS   := -pthread

# Modo (release por defecto). 'make debug' lo cambia.
MODE ?= release
ifeq ($(MODE),debug)
  OPT := -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
else
  OPT := -O2 -DNDEBUG
endif
CFLAGS := $(WARN) $(OPT) $(INCLUDES) -pthread

BUILD := build

COMMON_SRC  := src/common/protocol.c
CLIENTE_SRC := src/cliente/cliente.c src/cliente/args.c src/cliente/red.c \
               src/cliente/ops.c src/cliente/player.c
SERVIDOR_SRC := $(filter-out src/servidor/servidor.c,$(wildcard src/servidor/*.c))

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

# Prueba del catalogo
UNIT_TESTS += $(BUILD)/tests/test_catalog

$(BUILD)/tests/test_catalog: tests/test_catalog.c src/servidor/catalog.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) $^ -o $@

# Prueba de transferencia VOD
UNIT_TESTS += $(BUILD)/tests/test_vod

$(BUILD)/tests/test_vod: tests/test_vod.c \
                       src/servidor/vod.c \
                       src/servidor/catalog.c \
                       src/common/protocol.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) $^ -o $@

# Prueba de cancelacion y cierre durante VOD
UNIT_TESTS += $(BUILD)/tests/test_vod_control

$(BUILD)/tests/test_vod_control: tests/test_vod_control.c \
                               src/servidor/vod_control.c \
                               src/common/protocol.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) $^ -o $@

# Ejecutar todas las pruebas
# Prueba del canal (necesita canal.c y timeline.c)
UNIT_TESTS += $(BUILD)/tests/test_canal

$(BUILD)/tests/test_canal: tests/test_canal.c src/servidor/canal.c src/servidor/timeline.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) $^ -o $@

test: $(UNIT_TESTS) servidor
	@for t in $(UNIT_TESTS); do echo "\n== $$t"; $$t || exit 1; done
	@echo "\n== tests/test_protocol_version.py"
	@python3 tests/test_protocol_version.py
	@echo "\nTodas las pruebas unitarias pasaron."

test-integracion: all
	@echo "\n== tests/test_concurrencia.sh (FIFO, FORKED, THREADED)"
	bash tests/test_concurrencia.sh
	@echo "\n== tests/test_cliente_vod.sh (cliente contra servidor simulado)"
	bash tests/test_cliente_vod.sh
	@echo "\n== tests/diagnostico_sincronia.sh (sincronia del canal, RNF-04)"
	bash tests/diagnostico_sincronia.sh
	@echo "\nTodas las pruebas de integracion pasaron."

test-all: test test-integracion
	@echo "\nTodas las pruebas (unitarias + integracion) pasaron."

clean:
	rm -rf $(BUILD) cliente servidor

.PHONY: all debug test test-integracion test-all clean