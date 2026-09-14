# Makefile for Renesas RA demo projects
#
# Drives arduino-cli so you can build, upload and watch the serial monitor
# without opening the Arduino IDE.
#
#   make                 build the default sketch
#   make upload          build, then flash the board
#   make monitor         open the serial monitor (Ctrl-C to quit)
#   make run             upload, then immediately open the monitor
#   make ble-test        watch the board's Bluetooth LE output (Ctrl-C to quit)
#   make faust           regenerate a Faust-based sketch's synth.h from its .dsp source
#   make clean           delete build output
#   make ports           show connected boards
#   make help            list every target
#
# Build a different sketch by name:
#
#   make run SKETCH=HelloWorld
#
# A sketch that lives inside a library's examples/ folder works the same
# way, just with the full relative path:
#
#   make run SKETCH=RV112FF/examples/SerialPlotterDemo
#
# ---------------------------------------------------------------------------

# Which sketch folder to build. Each sketch lives in its own directory whose
# name matches its .ino file, as the Arduino IDE requires.
SKETCH ?= HelloWorld

# Fully Qualified Board Name. The SparkFun Thing Plus RA6M5 is supported by
# SparkFun's port of the Arduino Renesas core, which installs itself into the
# arduino:renesas_portenta package alongside the Portenta C33.
FQBN ?= arduino:renesas_portenta:thingplus_ra6m5

# Must match the Serial.begin() rate in the sketch.
BAUD ?= 115200

# Ask arduino-cli which port the board is on, so you don't have to hardcode
# it. stderr is discarded because unrelated broken cores can complain there.
# Override on the command line if auto-detection picks wrong: make PORT=/dev/cu.usbmodem12201
PORT ?= $(shell arduino-cli board list 2>/dev/null | grep -F '$(FQBN)' | head -1 | awk '{print $$1}')

# Keeping build output in one place per sketch makes rebuilds fast and
# `make clean` obvious.
BUILD_DIR ?= build/$(SKETCH)

ARDUINO_CLI ?= arduino-cli

# Our own local Arduino libraries (this project's RV112FF driver, and any
# future ones), passed via --library rather than installed into the global
# sketchbook - keeps them versioned with this repo instead of scattered
# into ~/Documents/Arduino/libraries.
LIBRARIES ?= RV112FF
LIBRARY_FLAGS = $(foreach lib,$(LIBRARIES),--library $(lib))

# Python virtual environment for tools/ble_test.py. Isolated from system
# Python so `pip install` here can't collide with anything else on the Mac.
BLE_VENV ?= tools/venv
BLE_PYTHON ?= $(BLE_VENV)/bin/python3

FAUST ?= faust

.DEFAULT_GOAL := build
.PHONY: build upload monitor run clean ports boards info help require-port ble-venv ble-test faust

# ---------------------------------------------------------------------------

build:
	$(ARDUINO_CLI) compile --fqbn $(FQBN) --build-path $(BUILD_DIR) $(LIBRARY_FLAGS) $(SKETCH)

# Guard used by the targets that need hardware attached.
require-port:
ifeq ($(strip $(PORT)),)
	@echo "No SparkFun Thing Plus RA6M5 detected."
	@echo
	@echo "  - Check the USB cable is a data cable, not charge-only."
	@echo "  - If the board is running a sketch that crashed, double-tap RST"
	@echo "    to enter DFU mode. The STAT LED fades slowly when it's waiting."
	@echo "  - Or name the port yourself:  make $(MAKECMDGOALS) PORT=/dev/cu.usbmodemXXXX"
	@echo
	@exit 1
endif

upload: require-port build
	$(ARDUINO_CLI) upload --fqbn $(FQBN) --port $(PORT) --input-dir $(BUILD_DIR)

monitor: require-port
	$(ARDUINO_CLI) monitor --port $(PORT) --config baudrate=$(BAUD)

# `Serial` on this board is native USB, so the port vanishes during the reset
# that follows an upload and comes back under a possibly different name. Wait
# for it to reappear, then re-detect rather than reusing the stale path.
run: upload
	@echo "Waiting for the board to re-enumerate..."
	@sleep 3
	@$(MAKE) --no-print-directory monitor

# Sets itself up on first use: creates the venv and installs bleak if either
# is missing, then does nothing on later runs. bleak's macOS backend needs
# pyobjc-core, which fails to build from source on current Xcode/clang - the
# --only-binary flag forces a prebuilt wheel instead.
ble-venv:
	@test -x $(BLE_PYTHON) || python3 -m venv $(BLE_VENV)
	@$(BLE_PYTHON) -c "import bleak" 2>/dev/null || \
		$(BLE_VENV)/bin/pip install -q --only-binary=:all: bleak

# Live text monitor for HelloWorldBLE's Bluetooth LE output - the terminal
# equivalent of connecting with a phone app. Requires the sketch on the
# board to actually be advertising (see HelloWorldBLE.ino), and on macOS,
# Terminal must have Bluetooth permission (System Settings > Privacy &
# Security > Bluetooth).
ble-test: ble-venv
	$(BLE_PYTHON) -u tools/ble_test.py

# Regenerates AudioKnobsDemo's synth.h from synth.dsp. Not needed for a
# normal build - the generated header is committed - only after editing
# the .dsp source. Needs the `faust` compiler on PATH.
faust:
	$(FAUST) -cn KnobSynth AudioKnobsDemo/synth.dsp -o AudioKnobsDemo/synth.h

clean:
	rm -rf build

ports:
	@$(ARDUINO_CLI) board list 2>/dev/null

boards:
	@$(ARDUINO_CLI) board listall 2>/dev/null | grep -i renesas || true

info:
	@echo "Sketch     : $(SKETCH)"
	@echo "FQBN       : $(FQBN)"
	@echo "Port       : $(if $(strip $(PORT)),$(PORT),<not detected>)"
	@echo "Baud       : $(BAUD)"
	@echo "Build dir  : $(BUILD_DIR)"

help:
	@echo "Targets:"
	@echo "  build     compile SKETCH (default)"
	@echo "  upload    compile and flash the board"
	@echo "  monitor   open the serial monitor at $(BAUD) baud"
	@echo "  run       upload, then open the monitor"
	@echo "  ble-test  watch the board's Bluetooth LE output (sets up a venv on first run)"
	@echo "  faust     regenerate AudioKnobsDemo/synth.h from synth.dsp (needs faust on PATH)"
	@echo "  clean     remove build output"
	@echo "  ports     list connected boards"
	@echo "  boards    list installed Renesas board names"
	@echo "  info      show the current settings"
	@echo
	@echo "Variables: SKETCH, FQBN, PORT, BAUD, LIBRARIES"
	@echo "Example:   make run SKETCH=HelloWorld"
	@echo "Example:   make run SKETCH=RV112FF/examples/SerialPlotterDemo"
