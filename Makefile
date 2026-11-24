CC ?= cc
CPPFLAGS += -I. -Iatop
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wcast-align -std=gnu11
LDFLAGS ?=
LDLIBS += -lz

ifneq (,$(filter $(USE_TLS),yes YES y Y 1))
MODE := tls
CPPFLAGS += -DUSE_TLS
LDLIBS += -lssl -lcrypto
else
MODE := plain
endif

BUILD_DIR := build/$(MODE)
SOURCES := cache.c httpd.c json.c metrics.c output.c rawlog.c connection.c socket.c tls.c
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES)) $(BUILD_DIR)/version.o
DEPS := $(OBJECTS:.o=.d)
ASSETS := $(shell find http -type f)
BIN := atophttpd
PREFIX ?= $(prefix)

.PHONY: all clean install deb submodule test FORCE
all: $(BIN)

$(BIN): $(BUILD_DIR)/$(BIN) FORCE
	cp $< $@

$(BUILD_DIR)/$(BIN): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: %.c | atop/version.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/httpd.o: $(ASSETS)

$(BUILD_DIR)/version.o: atop/version.c atop/version.h atop/versdate.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

atop/version.h:
	git submodule update --init --recursive
	@test -f $@ || { echo 'atop submodule checkout failed' >&2; exit 1; }

atop/version.c: atop/version.h
	@test -f $@ || { echo 'atop version source is missing' >&2; exit 1; }

atop/versdate.h: atop/version.h
	$(MAKE) -C atop versdate.h

submodule:
	git submodule update --init --recursive

install: $(BIN)
	install -D -m 755 $(BIN) $(DESTDIR)$(PREFIX)/usr/bin/$(BIN)
	install -D -m 644 atophttpd.service $(DESTDIR)$(PREFIX)/lib/systemd/system/atophttpd.service
	install -D -m 644 man/atophttpd.1 $(DESTDIR)$(PREFIX)/usr/share/man/man1/atophttpd.1

deb: $(BIN)
	sh packaging/debian/makedeb.sh "$(CURDIR)"

build/test/mkfixture: tests/mkfixture.c $(BUILD_DIR)/version.o
	@mkdir -p build/test
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(BUILD_DIR)/version.o -lz

build/test/status: tests/test_status.c $(BUILD_DIR)/metrics.o
	@mkdir -p build/test
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(BUILD_DIR)/metrics.o

test: $(BIN) build/test/mkfixture build/test/status
	build/test/status
	python3 tests/test_integration.py

clean:
	rm -rf build $(BIN)

-include $(DEPS)
