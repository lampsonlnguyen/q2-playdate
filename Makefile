CC ?= cc
CFLAGS ?= -O2
HOST_CC ?= cc
PDC ?= pdc
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

$(BUILD)/q2-playdate: src/runner.c src/card.c src/card.h src/bundle.h src/picker.c src/picker.h src/runtime.c src/runtime.h Makefile $(LUA_DIR)/.pinned $(TINYALSA_DIR)/.pinned
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(LUA_DIR) -I$(TINYALSA_DIR)/include $(LDFLAGS) -o $@ src/runner.c src/card.c src/picker.c src/runtime.c $(TINYALSA_SOURCES) $(LUA_SOURCES) -lm -lz

build/luac: Makefile $(LUA_DIR)/.pinned
	$(HOST_CC) -O2 -I$(LUA_DIR) -o $@ $(LUA_DIR)/luac.c $(LUA_SOURCES) -lm

check: all build/luac
	python3 tests/smoke.py build/luac $(BUILD)/q2-playdate $(RUNNER)


card: $(BUILD)/q2-playdate
	readelf -h $< | grep -q 'Machine:.*MIPS'
	mkdir -p $(BUILD)/card/.playdate/games
	cp $< q2app LICENSE $(BUILD)/card/.playdate/
	cp $(LUA_DIR)/doc/readme.html $(BUILD)/card/.playdate/Lua-license.html
	cp $(TINYALSA_DIR)/LICENSE $(BUILD)/card/.playdate/TinyALSA-license.txt
	$(PDC) demo $(BUILD)/card/.playdate/demo.pdx
	$(PDC) demos/save-tone $(BUILD)/card/.playdate/games/Save-and-Tone.pdx
	cd $(BUILD)/card && rm -f ../q2-playdate-card.zip && zip -q -r ../q2-playdate-card.zip .playdate/q2app .playdate/q2-playdate .playdate/LICENSE .playdate/Lua-license.html .playdate/TinyALSA-license.txt .playdate/demo.pdx .playdate/games/
