.PHONY: all build run clean debug debug-build gdb sim vcd-diff

# Find all source files using wildcards
CPP_SOURCES := $(shell find src -name '*.cpp')
HEADER_SOURCES := $(shell find src -name '*.h')
EXTERNAL_SOURCES := $(shell find external/cpp-vcd-tracer/src -name '*.cpp' -o -name '*.hpp' -o -name '*.h')
SOURCES := $(CPP_SOURCES) $(HEADER_SOURCES) $(EXTERNAL_SOURCES)
BINARY := build/custom_hdl_compiler
source ?= tests/counter_top/rtl/counter_top.v

all: $(BINARY) run

build/CMakeCache.txt: $(SOURCES)
	cmake -B build

$(BINARY): build/CMakeCache.txt $(SOURCES)
	cmake --build build -j$(shell nproc)

build: $(BINARY)

run: $(BINARY)
	./$(BINARY) $(source)

sim: $(BINARY)
	$(MAKE) -C tests/$(test)/work/custom-sim simulate

vcd-diff:
	./build/tools/vcd-compare/vcd_compare $(vcd1) $(vcd2)

# Debug targets
debug-build:
	cmake -B build -DCMAKE_BUILD_TYPE=Debug
	cmake --build build -j$(shell nproc)

debug: debug-build
	./$(BINARY) $(source)

gdb: debug-build
	gdb --args ./$(BINARY) $(source)

clean:
	cmake --build build --target clean
