
# Recipes are POSIX sh (mkdir -p, rm -f, test). Without this, running make
# from cmd.exe picks cmd as the shell and every recipe breaks.
SHELL := /bin/sh

ifndef NT_API_PATH
	NT_API_PATH := ../distingNT_API
endif

INCLUDE_PATH := $(NT_API_PATH)/include

inputs := $(wildcard *.cpp)
outputs := $(patsubst %.cpp,plugins/%.o,$(inputs))

all: $(outputs)

clean:
	rm -f $(outputs)

plugins/%.o: %.cpp
	mkdir -p $(@D)
	arm-none-eabi-c++ -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fno-rtti -fno-exceptions -fno-math-errno -Os -fPIC -Wall -I$(INCLUDE_PATH) -c -o $@ $^

# host-side syntax check (no ARM toolchain needed)
check: $(inputs)
	clang++ -std=c++11 -fsyntax-only -fno-rtti -fno-exceptions -Wall -Wextra -Wno-missing-field-initializers -I$(INCLUDE_PATH) $^
	@echo "syntax OK"

# push the built plug-in to the module over USB MIDI SysEx, then rescan.
# Needs python-rtmidi, and the algorithm removed from the running preset --
# the firmware refuses to rescan a plug-in that is still loaded, so a "still
# in use" failure means "delete the algorithm", not "the file did not arrive".
NT_TOOL ?= ../disting-nt-plugins/tools/nt_plugin.py

deploy: all
	@test -f "$(NT_TOOL)" || { \
		echo "deploy tool not found: $(NT_TOOL)"; \
		echo "override with: make deploy NT_TOOL=/path/to/nt_plugin.py"; \
		exit 1; }
	python "$(NT_TOOL)" deploy $(outputs)

# which MIDI ports the deploy tool can see
ports:
	python "$(NT_TOOL)" ports

.PHONY: all clean check deploy ports
