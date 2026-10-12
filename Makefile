# Esqueleto — os alvos são preenchidos à medida que as fases avançam.
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -g -Iinclude -Ithird_party -D_POSIX_C_SOURCE=200809L
LDFLAGS := -pthread

# Para apanhar bugs de memória: make SAN=1
ifdef SAN
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

BIN := bin

.PHONY: all clean

all:
	@echo "Ainda sem fontes — começa no M0."

clean:
	rm -rf build $(BIN)
