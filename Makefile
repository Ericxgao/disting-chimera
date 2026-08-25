
# Recipes are POSIX sh (mkdir -p, rm -f). Started from cmd.exe, make picks cmd
# instead and they all break. A bare "SHELL := /bin/sh" does not fix that: Git
# ships sh.exe under bin/ while only cmd/ is on PATH, so make cannot find it and
# silently falls back to cmd. Probe for a real one instead. The 8.3 short names
# keep the space in "Program Files" out of $(wildcard), which splits on
# whitespace. If nothing matches, leave SHELL alone rather than guess.
sh_found := $(firstword $(wildcard /bin/sh \
	C:/PROGRA~1/Git/bin/sh.exe C:/PROGRA~2/Git/bin/sh.exe))
ifneq ($(sh_found),)
SHELL := $(sh_found)
endif

# Setting SHELL is not enough on its own: make execs commands with no shell
# metacharacters directly, so `rm -f` still dies with "cannot find the file"
# when the tools are not on PATH. Prepend them. Matches nothing off Windows,
# where $(wildcard) on a C: path is empty and this whole block is skipped.
git_usr := $(firstword $(wildcard \
	C:/PROGRA~1/Git/usr/bin/rm.exe C:/PROGRA~2/Git/usr/bin/rm.exe))
ifneq ($(git_usr),)
export PATH := $(patsubst %/,%,$(dir $(git_usr)));$(PATH)
endif

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

# the guard is a make function, not a shell test, so it reports the same way
# whichever shell the recipe ends up running under
deploy: all
	$(if $(wildcard $(NT_TOOL)),,$(error deploy tool not found: $(NT_TOOL) -- override with: make deploy NT_TOOL=/path/to/nt_plugin.py))
	python "$(NT_TOOL)" deploy $(outputs)

# which MIDI ports the deploy tool can see
ports:
	python "$(NT_TOOL)" ports

.PHONY: all clean check deploy ports
