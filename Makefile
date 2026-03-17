.PHONY: all build run clean clean_project clean_all debug debug-configure debug-build gdb

# Find all source files using wildcards
CPP_SOURCES := $(shell find src -name '*.cpp')
HEADER_SOURCES := $(shell find src -name '*.h')
EXTERNAL_SOURCES := $(shell find external/cpp-vcd-tracer/src -name '*.cpp' -o -name '*.hpp' -o -name '*.h')
SOURCES := $(CPP_SOURCES) $(HEADER_SOURCES) $(EXTERNAL_SOURCES)
BINARY := build/custom_hdl_compiler
source ?= tests/counter/rtl/counter.v

all: $(BINARY) run

$(BINARY): $(SOURCES)
	cmake --build build -j$(shell nproc)

build: $(BINARY)

run: $(BINARY)
	./$(BINARY) $(source)

sim: $(BINARY)
	$(MAKE) -C tests/$(test)/work/custom-sim simulate

# Debug targets
debug-configure:
	cmake -B build -DCMAKE_BUILD_TYPE=Debug

debug-build: debug-configure
	cmake --build build -j$(shell nproc)

debug: debug-build
	./$(BINARY) $(source)

gdb: debug-build
	gdb --args ./$(BINARY) $(source)

# Clean only project artifacts (preserves slang)
clean_project:
	cmake --build build --target clean_project

clean: clean_project

# Clean all artifacts including slang
clean_all:
	cmake --build build --target clean

vcd-diff:
	./build/scratchpad/vcd-compare/vcd_compare $(vcd1) $(vcd2)
