CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -g -Iinclude -D_POSIX_C_SOURCE=200809L -MMD -MP
LDFLAGS := -pthread
LDLIBS  := -lcjson    # no fim da linha de link, depois dos .o (senão o linker não resolve)

# Para apanhar bugs de memória: make SAN=1
ifdef SAN
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

BUILD := build
BIN   := bin

COMMON_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(wildcard src/common/*.c))
SERVER_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(wildcard src/server/*.c))
CLIENT_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(wildcard src/client/*.c))

# Testes unitários. O wildcard apanha automaticamente cada test_*.c novo,
# por isso acrescentar uma suite não exige mexer aqui.
TEST_SRC := $(wildcard tests/unit/test_*.c)
TEST_OBJ := $(patsubst tests/unit/%.c,$(BUILD)/tests/%.o,$(TEST_SRC))
TEST_BIN := $(patsubst tests/unit/%.c,$(BIN)/tests/%,$(TEST_SRC))

ALL_OBJ := $(COMMON_OBJ) $(SERVER_OBJ) $(CLIENT_OBJ) $(TEST_OBJ)

.PHONY: all clean test

all: $(BIN)/server $(BIN)/client

$(BIN)/server: $(SERVER_OBJ) $(COMMON_OBJ)
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BIN)/client: $(CLIENT_OBJ) $(COMMON_OBJ)
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# --- testes unitários ------------------------------------------------------
# Código de teste é código nosso: leva as mesmas flags estritas.
$(BUILD)/tests/%.o: tests/unit/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests/unit -c $< -o $@

$(BIN)/tests/%: $(BUILD)/tests/%.o $(COMMON_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

# Corre TODAS as suites (não pára na primeira: queremos o panorama todo) e
# devolve != 0 se alguma falhar.
test: $(TEST_BIN)
	@fail=0; \
	for t in $(TEST_BIN); do \
	    echo "=== $$t"; \
	    ./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "RESULTADO: FALHOU"; exit 1; fi; \
	echo "RESULTADO: tudo passou"

clean:
	rm -rf $(BUILD) $(BIN)

-include $(ALL_OBJ:.o=.d)
