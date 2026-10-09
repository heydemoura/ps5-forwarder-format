# PS5 Forwarder Format - build and test.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   make test        host tests for the library and the launcher protocol
#   make launcher    template/launcher.elf (needs PS5_PAYLOAD_SDK)
#   make embed       forwarder/src/launcher_payload.inc from template/launcher.elf
#
# The forwarder program (template/eboot.bin) is a small app built with
# ps5-native-app-boilerplate; see README.md, "Building the template".

CC ?= cc
TEST_PORT ?= 20199
BUILD := build

.PHONY: all test launcher embed clean

all: test

$(BUILD)/test_launcher: launcher/launcher.c tests/launcher_stubs.c
	@mkdir -p $(BUILD)/log
	$(CC) -O1 -Wall -Wextra -DLAUNCHER_PORT=$(TEST_PORT) -DLOG_DIR='"$(BUILD)/log"' -o $@ $^

$(BUILD)/test_psfwd: tests/test_psfwd.c src/psfwd.c include/psfwd.h
	@mkdir -p $(BUILD)
	$(CC) -O1 -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE -DPSFWD_LAUNCHER_PORT=$(TEST_PORT) \
		-Iinclude -o $@ tests/test_psfwd.c src/psfwd.c

test: $(BUILD)/test_psfwd $(BUILD)/test_launcher
	$(BUILD)/test_psfwd $(BUILD)/test_launcher

launcher: template/launcher.elf

template/launcher.elf: launcher/launcher.c
	@test -n "$(PS5_PAYLOAD_SDK)" || { echo "set PS5_PAYLOAD_SDK to a ps5-payload-sdk"; exit 2; }
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang -Wall -Wextra -Werror -O2 -o $@ $< \
		$(PS5_PAYLOAD_SDK)/target/lib/crt1.o -lSceSystemService -lSceUserService

embed: forwarder/src/launcher_payload.inc

forwarder/src/launcher_payload.inc: template/launcher.elf forwarder/embed_payload.py
	python3 forwarder/embed_payload.py $< $@

clean:
	rm -rf $(BUILD)
