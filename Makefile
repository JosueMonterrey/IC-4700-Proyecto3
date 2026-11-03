CC      ?= cc
BASE    := -std=c11 -Wall -Wextra -Icommon -Iserver -Iclient
MODE    ?= release
BUILD   := build
BIN     := bin

ifeq ($(MODE),debug)
CFLAGS  += $(BASE) -g -O0 -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
else
CFLAGS  += $(BASE) -O2 -DNDEBUG
endif

vpath %.c common server client
vpath %.h common server client

COMUN    = $(BUILD)/protocol.o $(BUILD)/log.o
SRV_OBJ  = $(BUILD)/server.o $(BUILD)/session.o $(BUILD)/catalog.o $(BUILD)/channel.o $(COMUN)
CLI_OBJ  = $(BUILD)/client.o $(BUILD)/reproductor.o $(COMUN)

all: $(BIN)/server $(BIN)/client

release:
	$(MAKE) clean
	$(MAKE) MODE=release all

debug:
	$(MAKE) clean
	$(MAKE) MODE=debug all

$(BIN)/server: $(SRV_OBJ) | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^

$(BIN)/client: $(CLI_OBJ) | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD) $(BIN):
	mkdir -p $@

$(BUILD)/%.o: %.c $(wildcard common/*.h server/*.h client/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# Solo borra lo generado (build/ y bin/), nunca el codigo fuente
clean:
	rm -rf $(BUILD) $(BIN)

.PHONY: all release debug clean
