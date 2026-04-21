.PHONY: all build dev debug sanitized release ensure-debug-slang test regression gdb clean

all: dev

build: dev

dev:
	cmake --preset dev
	cmake --build --preset dev --parallel

ensure-debug-slang:
	@if [ ! -f external/slang-install-debug/lib/cmake/slang/slangConfig.cmake ]; then \
		echo "Building Debug slang into external/slang-install-debug"; \
		CMAKE_BUILD_TYPE=Debug bash scripts/build_slang.sh; \
	fi

debug: ensure-debug-slang
	cmake --preset debug
	cmake --build --preset debug --parallel

sanitized:
	cmake --preset sanitized
	cmake --build --preset sanitized --parallel

release:
	cmake --preset release
	cmake --build --preset release --parallel

test: regression

regression: sanitized
	python tests/regression.py

gdb: debug
	gdb --args ./build/debug/mate $(source)

clean:
	rm -rf build/dev build/debug build/sanitized build/release build/vcd-compare
