CC ?= cc
CFLAGS ?= -O2
HOST_CC ?= cc
PDC ?= pdc
BUILD ?= build/host
LUA_DIR := build/lua54
LUA_REV := c47f7fe1f14b1c424b0835e6c4fb2df892f358e4
LUA_SOURCES = $(filter-out $(addprefix $(LUA_DIR)/,lua.c luac.c linit.c loadlib.c loslib.c liolib.c),$(wildcard $(LUA_DIR)/*.c))

.PHONY: all check card
all: $(BUILD)/q2-playdate

$(LUA_DIR)/.pinned:
	mkdir -p build
	git clone --no-checkout https://github.com/scratchminer/lua54.git $(LUA_DIR)
	git -C $(LUA_DIR) checkout --detach $(LUA_REV)
	touch $@

$(BUILD)/q2-playdate: src/runner.c src/card.c src/card.h Makefile $(LUA_DIR)/.pinned
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(LUA_DIR) $(LDFLAGS) -o $@ src/runner.c src/card.c $(LUA_SOURCES) -lm -lz

build/luac: Makefile $(LUA_DIR)/.pinned
	$(HOST_CC) -O2 -I$(LUA_DIR) -o $@ $(LUA_DIR)/luac.c $(LUA_SOURCES) -lm

check: all build/luac
	python3 tests/smoke.py build/luac $(BUILD)/q2-playdate $(RUNNER)


card: $(BUILD)/q2-playdate
	readelf -h $< | grep -q 'Machine:.*MIPS'
	mkdir -p $(BUILD)/card/.playdate
	cp $< q2app $(BUILD)/card/.playdate/
	$(PDC) demo $(BUILD)/card/.playdate/demo.pdx
	cd $(BUILD)/card && zip -q -r ../q2-playdate-card.zip .playdate
