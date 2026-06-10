CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pedantic
CPPFLAGS ?= -Iinclude

BIN := xgw
SRCS := src/main.c src/config.c src/policy.c src/protocol.c src/session.c src/frame.c src/transport_udp.c src/dataplane.c src/runtime.c src/acl.c src/pool.c src/tuning.c src/obfs.c src/outbound.c src/tun_stub.c src/tun_linux.c src/afxdp_stub.c src/afxdp_linux.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

clean:
	del /q $(OBJS) $(BIN).exe 2>nul || exit 0
