# claude-telepresence Makefile

CC ?= gcc
CFLAGS ?= -Wall -O2

# Detect platform for socket libs
UNAME := $(shell uname -s)
ifeq ($(UNAME),SunOS)
    LDFLAGS += -lsocket -lnsl
endif
ifeq ($(UNAME),HP-UX)
    LDFLAGS += -lsocket -lnsl
endif

.PHONY: all clean test

all: claude-telepresence telepresence-helper

claude-telepresence: client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

telepresence-helper: helper.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f claude-telepresence telepresence-helper

# Copy helper to expected location for relay
install-helper: telepresence-helper
	cp telepresence-helper /tmp/telepresence-helper
	chmod 755 /tmp/telepresence-helper

# Quick test - just verify compilation
test: all
	@echo "Build successful!"
	@echo ""
	@echo "To use:"
	@echo "  1. On Linux server: python3 relay.py --port 5000"
	@echo "  2. On legacy box:   ./claude-telepresence <linux-server> 5000"

# ============================================================================
# Build instructions for legacy systems (no make needed):
#
# HP-UX:
#   cc -o claude-telepresence client.c -lsocket -lnsl
#
# IRIX:
#   cc -o claude-telepresence client.c
#
# Solaris:
#   cc -o claude-telepresence client.c -lsocket -lnsl
#
# AIX:
#   cc -o claude-telepresence client.c
#
# NeXT:
#   cc -o claude-telepresence client.c
#
# Tru64/OSF1:
#   cc -o claude-telepresence client.c
#
# Linux:
#   gcc -o claude-telepresence client.c
#
# FreeBSD/NetBSD/OpenBSD:
#   cc -o claude-telepresence client.c
# ============================================================================
