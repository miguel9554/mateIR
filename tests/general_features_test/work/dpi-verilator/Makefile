.PHONY: simulate clean force

WAVES = waves.vcd

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
TB_DIR = $(ROOT_DIR)/tb
DPI_DIR = $(ROOT_DIR)/dpi
BUILD_DIR = $(PROJECT_ROOT)/build/dev
TOP_MODULE = tb
OUT = obj_dir/V$(TOP_MODULE)

TB_SRCS = \
	$(TB_DIR)/generated/uut_if.sv \
	$(TB_DIR)/uut_tb.sv \
	$(TB_DIR)/dpi/tb.sv \
	$(TB_DIR)/dpi/general_features_test_dpi.sv

DPI_CPP = $(DPI_DIR)/general_features_test_dpi.cpp

MATE_LIBS = \
	$(abspath $(BUILD_DIR)/libmate-dpi-support.a) \
	$(abspath $(BUILD_DIR)/libmate-rtl-runtime-compiler.a) \
	$(abspath $(BUILD_DIR)/libmate-systemverilog-frontend.a) \
	$(abspath $(BUILD_DIR)/libmate-rtl-runtime.a) \
	$(abspath $(BUILD_DIR)/libmate-mateir.a) \
	$(abspath $(PROJECT_ROOT)/build/external/yaml-cpp/libyaml-cpp.a) \
	/opt/slang/lib/libsvlang.a \
	/usr/local/lib/libfmt.a

VERILATOR_WARNS ?= -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC
SEED_ARG = $(if $(seed),+verilator+seed+$(seed))

MATE_CPPFLAGS = \
	-std=c++20 \
	-I$(abspath $(PROJECT_ROOT)/src) \
	-I$(abspath $(PROJECT_ROOT)/external/slang/external/ieee1800) \
	-DMATE_GFT_TOP_MODULE=\"general_features_test\" \
	-DMATE_GFT_RTL=\"$(abspath $(RTL_DIR)/general_features_test.v)\" \
	-DMATE_GFT_DOMAINS=\"$(abspath $(RTL_DIR)/general_features_test.domains.yaml)\"

all: simulate

$(BUILD_DIR)/libmate-dpi-support.a: force
	$(MAKE) -C $(PROJECT_ROOT) dev

$(OUT): $(TB_SRCS) $(DPI_CPP) $(BUILD_DIR)/libmate-dpi-support.a
	rm -rf obj_dir
	verilator --trace --timing --cc --exe --build --main \
		$(VERILATOR_WARNS) \
		--top-module $(TOP_MODULE) \
		-CFLAGS '$(MATE_CPPFLAGS)' \
		-LDFLAGS '$(MATE_LIBS)' \
		$(TB_SRCS) \
		$(DPI_CPP)

$(WAVES): $(OUT) force
	./$(OUT) $(SEED_ARG)

simulate:
	$(MAKE) $(WAVES)

force:

clean:
	rm -rf obj_dir $(WAVES)
