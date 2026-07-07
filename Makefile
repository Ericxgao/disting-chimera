
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
