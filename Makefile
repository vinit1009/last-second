# Host-side targets. The QNX build is target/build.sh (needs SDP 8.0's qcc).
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2

.PHONY: test clean
test: build/test_core
	./build/test_core

build/test_core: src/core.c src/core.h include/blackbox.h tests/test_core.c
	mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -Iinclude -Isrc src/core.c tests/test_core.c -o $@

clean:
	rm -rf build
