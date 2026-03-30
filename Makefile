.PHONY: all build run clean debug debug-build gdb sim vcd-diff

# Find all source files using wildcards
CPP_SOURCES := $(shell find src -name '*.cpp')
HEADER_SOURCES := $(shell find src -name '*.h')
EXTERNAL_SOURCES := $(shell find external/cpp-vcd-tracer/src -name '*.cpp' -o -name '*.hpp' -o -name '*.h')
SOURCES := $(CPP_SOURCES) $(HEADER_SOURCES) $(EXTERNAL_SOURCES)
BINARY := build/custom_hdl_compiler

all: $(BINARY) run

build/CMakeCache.txt: $(SOURCES)
	cmake -B build

$(BINARY): build/CMakeCache.txt $(SOURCES)
	cmake --build build -j$(shell nproc)

build: $(BINARY)

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
