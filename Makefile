
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
# Needs python-rtmidi.
#
# The rescan resets the whole plug-in list, so it is refused while *any*
# plug-in is in use -- not just this one. `make scan` names the culprit
# (the one marked "loaded"); remove that algorithm from the preset on the
# module and `make rescan`. The push has already happened by then, so there
# is no need to deploy again.
NT_TOOL ?= ../disting-nt-plugins/tools/nt_plugin.py

# expanded inside a recipe, so a bad path is reported when a tool target runs
# rather than on every make. A make function, not a shell test, so it reads the
# same whichever shell the recipe ends up under.
nt_tool = $(if $(wildcard $(NT_TOOL)),$(NT_TOOL),$(error nt_plugin.py not found: $(NT_TOOL) -- override with: make $@ NT_TOOL=/path/to/nt_plugin.py))

deploy: all
	python "$(nt_tool)" deploy $(outputs)

# reset and rescan the plug-in list, without pushing again
rescan:
	python "$(nt_tool)" rescan

# which plug-ins the module found, and which are currently loaded
scan:
	python "$(nt_tool)" scan

# which MIDI ports the deploy tool can see
ports:
	python "$(nt_tool)" ports

.PHONY: all clean check deploy rescan scan ports
