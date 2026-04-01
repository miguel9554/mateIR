.PHONY: all build run clean debug debug-build ensure-debug-slang diagnostic-build gdb sim vcd-diff

# Find all source files using wildcards
CPP_SOURCES := $(shell find src -name '*.cpp')
HEADER_SOURCES := $(shell find src -name '*.h')
EXTERNAL_SOURCES := $(shell find external/cpp-vcd-tracer/src -name '*.cpp' -o -name '*.hpp' -o -name '*.h')
SOURCES := $(CPP_SOURCES) $(HEADER_SOURCES) $(EXTERNAL_SOURCES)
BUILD_DIR ?= build
CMAKE_ARGS ?=
BINARY := $(BUILD_DIR)/custom_hdl_compiler

all: $(BINARY) run

$(BUILD_DIR)/CMakeCache.txt: $(SOURCES) CMakeLists.txt
	cmake -B $(BUILD_DIR) $(CMAKE_ARGS)
ifdef HOST_PROJECT_ROOT
	sed -i \
		-e "s|/workspace|$(HOST_PROJECT_ROOT)|g" \
		-e "s|/opt/slang|$(HOST_PROJECT_ROOT)/external/slang-install|g" \
		$(BUILD_DIR)/compile_commands.json 2>/dev/null || true
endif

$(BINARY): $(BUILD_DIR)/CMakeCache.txt $(SOURCES)
	cmake --build $(BUILD_DIR) -j$(shell nproc)

build: $(BINARY)

# Debug targets
ensure-debug-slang:
	@if [ ! -f external/slang-install-debug/lib/cmake/slang/slangConfig.cmake ]; then \
		echo "Building Debug slang into external/slang-install-debug"; \
		CMAKE_BUILD_TYPE=Debug bash scripts/build_slang.sh; \
	fi

debug-build:
	$(MAKE) ensure-debug-slang
	$(MAKE) BUILD_DIR=build/debug CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Debug" build

diagnostic-build:
	$(MAKE) BUILD_DIR=build/diagnostic-relwithdebinfo CMAKE_ARGS="-DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SANITIZERS=ON" build

debug: debug-build
	./build/debug/custom_hdl_compiler $(source)

gdb: debug-build
	gdb --args ./build/debug/custom_hdl_compiler $(source)

clean:
	rm -rf build build/debug build/diagnostic build/diagnostic-relwithdebinfo
