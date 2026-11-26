CC ?= cc
CFLAGS ?= -O2
HOST_CC ?= cc
PLAYDATE_SDK_VERSION := 2.7.1
SDK_PDC := $(abspath build/playdate-sdk/bin/pdc)
PDC ?= $(or $(shell command -v pdc),$(SDK_PDC))
PDC_DEP = $(filter $(SDK_PDC),$(PDC))
GO ?= go
Q2_CA_BUNDLE ?= /etc/ssl/certs/ca-certificates.crt
BUILD ?= build/host
LUA_DIR := build/lua54
LUA_REV := c47f7fe1f14b1c424b0835e6c4fb2df892f358e4
LUA_SOURCES = $(filter-out $(addprefix $(LUA_DIR)/,lua.c luac.c linit.c loadlib.c loslib.c liolib.c),$(wildcard $(LUA_DIR)/*.c))

TINYALSA_DIR := build/tinyalsa
TINYALSA_REV := a7cd26a17fe2add2ff38d2f281ed0b919187a068
TINYALSA_SOURCES := $(addprefix $(TINYALSA_DIR)/src/,pcm.c pcm_hw.c limits.c snd_card_plugin.c)

.PHONY: all check card
all: $(BUILD)/q2-playdate

$(LUA_DIR)/.pinned:
	mkdir -p build
	git clone --no-checkout https://github.com/scratchminer/lua54.git $(LUA_DIR)
	git -C $(LUA_DIR) checkout --detach $(LUA_REV)
	touch $@

$(TINYALSA_DIR)/.pinned:
	mkdir -p build
	git clone --no-checkout https://github.com/tinyalsa/tinyalsa.git $(TINYALSA_DIR)
	git -C $(TINYALSA_DIR) checkout --detach $(TINYALSA_REV)
	touch $@

build/json_lua.h: src/json.lua tools/embed_lua.py
	mkdir -p build
	python3 tools/embed_lua.py src/json.lua json_lua $@

$(BUILD)/q2-playdate: src/runner.c src/card.c src/card.h src/bundle.h src/file.c src/file.h src/image.c src/image.h src/picker.c src/picker.h src/runtime.c src/runtime.h src/itch.c src/itch.h build/json_lua.h Makefile $(LUA_DIR)/.pinned $(TINYALSA_DIR)/.pinned
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Ibuild -I$(LUA_DIR) -I$(TINYALSA_DIR)/include $(LDFLAGS) -o $@ src/runner.c src/card.c src/file.c src/image.c src/picker.c src/runtime.c src/itch.c $(TINYALSA_SOURCES) $(LUA_SOURCES) -lm -lz

build/luac: Makefile $(LUA_DIR)/.pinned
	$(HOST_CC) -O2 -I$(LUA_DIR) -o $@ $(LUA_DIR)/luac.c $(LUA_SOURCES) -lm

build/host/image-test: tests/image_test.c src/image.c src/image.h Makefile $(LUA_DIR)/.pinned
	mkdir -p build/host
	$(HOST_CC) $(CFLAGS) -Isrc -I$(LUA_DIR) -o $@ tests/image_test.c src/image.c $(LUA_SOURCES) -lm -lz

check: all build/luac build/host/image-test
	python3 tests/smoke.py build/luac $(BUILD)/q2-playdate $(RUNNER)
	python3 tests/image_check.py build/host/image-test
	python3 tests/image_smoke.py build/luac $(BUILD)/q2-playdate $(RUNNER)
	python3 tests/file_smoke.py build/luac $(BUILD)/q2-playdate
	python3 tests/json_smoke.py build/luac $(BUILD)/q2-playdate


$(BUILD)/q2-playdate-itch: $(wildcard itch/*.go) itch/go.mod itch/go.sum
	mkdir -p $(BUILD)
	cd itch && CGO_ENABLED=0 GOOS=linux GOARCH=mipsle GOMIPS=softfloat $(GO) build -buildvcs=false -trimpath -ldflags="-s -w" -o "$(abspath $@)" .

.PHONY: check-itch
check-itch:
	cd itch && export CGO_ENABLED=0 && $(GO) vet ./... && $(GO) test ./...

.PHONY: sdk
sdk: $(SDK_PDC)

$(SDK_PDC):
	mkdir -p "$(dir $@)"
	curl -fLsS "https://download.panic.com/playdate_sdk/Linux/PlaydateSDK-$(PLAYDATE_SDK_VERSION).tar.gz" -o "$@.tar.gz"
	tar -xOf "$@.tar.gz" "PlaydateSDK-$(PLAYDATE_SDK_VERSION)/bin/pdc" > "$@.tmp"
	chmod +x "$@.tmp"
	mv "$@.tmp" "$@"
	rm -f "$@.tar.gz"

card: $(BUILD)/q2-playdate $(BUILD)/q2-playdate-itch $(PDC_DEP)
	readelf -h $< | grep -q 'Machine:.*MIPS'
	mkdir -p $(BUILD)/card/.playdate/games
	cp $(BUILD)/q2-playdate $(BUILD)/q2-playdate-itch q2app LICENSE $(BUILD)/card/.playdate/
	cp $(LUA_DIR)/doc/readme.html $(BUILD)/card/.playdate/Lua-license.html
	cp "$(Q2_CA_BUNDLE)" $(BUILD)/card/.playdate/itch-ca.pem
	cp itch/NOTICE itch/NextUI-Itchio-Pak-LICENSE itch/COPYING $(BUILD)/card/.playdate/
	cp "$$($(GO) env GOROOT)/LICENSE" $(BUILD)/card/.playdate/Go-license.txt
	cd itch && cp "$$($(GO) list -m -f '{{.Dir}}' golang.org/x/net)/LICENSE" "$(abspath $(BUILD))/card/.playdate/x-net-license.txt"
	cp $(TINYALSA_DIR)/LICENSE $(BUILD)/card/.playdate/TinyALSA-license.txt
	"$(PDC)" demo $(BUILD)/card/.playdate/demo.pdx
	"$(PDC)" demos/save-tone $(BUILD)/card/.playdate/games/Save-and-Tone.pdx
	cd $(BUILD)/card && rm -f ../q2-playdate-card.zip && zip -q -r ../q2-playdate-card.zip .playdate/q2app .playdate/q2-playdate .playdate/q2-playdate-itch .playdate/itch-ca.pem .playdate/NOTICE .playdate/NextUI-Itchio-Pak-LICENSE .playdate/COPYING .playdate/Go-license.txt .playdate/x-net-license.txt .playdate/LICENSE .playdate/Lua-license.html .playdate/TinyALSA-license.txt .playdate/demo.pdx .playdate/games/

ARM_AS ?= arm-none-eabi-as
ARM_LD ?= arm-none-eabi-ld
UNICORN_SRC ?= build/unicorn
UNICORN_BUILD ?= build/unicorn-cross
UNICORN_REV := 7c5db94191defc1e04a4f66f4eb1220903cba837
UNICORN_LIBS := $(addprefix $(UNICORN_BUILD)/,libunicorn-static.a libarm-softmmu.a libunicorn-common.a)
CMAKE ?= cmake
JOBS ?= 4

.PHONY: native-probe check-native
native-probe: $(BUILD)/native-probe $(BUILD)/native.pdx/pdex.bin

check-native: native-probe
	$(RUNNER) $(BUILD)/native-probe $(BUILD)/native.pdx/pdex.bin

$(UNICORN_SRC)/.pinned:
	mkdir -p $(dir $(UNICORN_SRC))
	git clone --no-checkout https://github.com/unicorn-engine/unicorn.git $(UNICORN_SRC)
	git -C $(UNICORN_SRC) checkout --detach $(UNICORN_REV)
	touch $@

$(UNICORN_BUILD)/.built: $(UNICORN_SRC)/.pinned
	$(CMAKE) -S $(UNICORN_SRC) -B $(UNICORN_BUILD) -DCMAKE_SYSTEM_NAME=Linux \
		-DCMAKE_SYSTEM_PROCESSOR=mipsel -DCMAKE_C_COMPILER="$(CC)" \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-static \
		-DUNICORN_ARCH=arm -DBUILD_SHARED_LIBS=OFF -DUNICORN_BUILD_TESTS=OFF -DUNICORN_INSTALL=OFF
	$(CMAKE) --build $(UNICORN_BUILD) --target unicorn -j $(JOBS)
	touch $@

$(BUILD)/native-probe: tests/native/probe.c Makefile $(UNICORN_BUILD)/.built
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(UNICORN_SRC)/include -static $(LDFLAGS) -o $@ $< \
		-Wl,--start-group $(UNICORN_LIBS) -Wl,--end-group -latomic -lz -lm -pthread

$(BUILD)/native.pdx/pdex.bin: tests/native/demo.s tests/native/demo.ld tests/native/pdxinfo Makefile $(PDC_DEP)
	mkdir -p $(BUILD)/native-input
	$(ARM_AS) -mcpu=cortex-m7 -o $(BUILD)/native-input/demo.o tests/native/demo.s
	$(ARM_LD) --emit-relocs -T tests/native/demo.ld -o $(BUILD)/native-input/pdex.elf $(BUILD)/native-input/demo.o
	cp tests/native/pdxinfo $(BUILD)/native-input/
	"$(PDC)" -k $(BUILD)/native-input $(BUILD)/native.pdx

PROBE_DIR := $(BUILD)/probe-card/.pdprobe

.PHONY: native-card
native-card: native-probe
	readelf -h $(BUILD)/native-probe | grep -q 'Machine:.*MIPS'
	mkdir -p "$(PROBE_DIR)"
	cp $(BUILD)/native-probe "$(PROBE_DIR)/q2-pdprobe"
	cp $(BUILD)/native.pdx/pdex.bin tests/native/q2app LICENSE "$(PROBE_DIR)/"
	cp $(UNICORN_SRC)/COPYING $(UNICORN_SRC)/COPYING.LGPL2 $(UNICORN_SRC)/COPYING_GLIB "$(PROBE_DIR)/"
	cd "$(PROBE_DIR)/.." && rm -f ../q2-playdate-native-probe.zip && zip -q ../q2-playdate-native-probe.zip .pdprobe/q2-pdprobe .pdprobe/pdex.bin .pdprobe/q2app .pdprobe/LICENSE .pdprobe/COPYING*
