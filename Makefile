CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -g -Iinclude -D_POSIX_C_SOURCE=200809L -MMD -MP
LDFLAGS := -pthread
LDLIBS  :=            # M1: -lcjson (vai no fim da linha de link, depois dos .o)

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

.PHONY: all clean

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

clean:
	rm -rf $(BUILD) $(BIN)

-include $(COMMON_OBJ:.o=.d) $(SERVER_OBJ:.o=.d) $(CLIENT_OBJ:.o=.d)
