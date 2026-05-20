BUILD    := build
TEST_OUT := $(BUILD)/tests
NPROC    := $(shell nproc 2>/dev/null || echo 4)

.PHONY: all firmware test clean

# Default: build firmware then run tests
all: firmware test

# Configure + build the Pico 2 firmware into build/
firmware: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD) -j$(NPROC)

$(BUILD)/CMakeCache.txt:
	cmake -B $(BUILD) -DPICO_BOARD=pico2

# Build all test binaries into build/tests/ and run them
test:
	$(MAKE) -C tests/host OUTDIR=../../$(TEST_OUT) test

clean:
	rm -rf $(BUILD)
	$(MAKE) -C tests/host OUTDIR=../../$(TEST_OUT) clean
