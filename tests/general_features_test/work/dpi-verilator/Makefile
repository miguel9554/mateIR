.PHONY: simulate clean force prep

WAVES = waves.vcd

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
TB_DIR = $(ROOT_DIR)/tb
DPI_DIR = $(ROOT_DIR)/dpi
BUILD_DIR = $(PROJECT_ROOT)/build/dev
GENERATED_DIR = $(RTL_DIR)/generated
TOP_MODULE = tb
OUT = obj_dir/V$(TOP_MODULE)
RTL_SRC = $(RTL_DIR)/general_features_test.v
DOMAINS_SRC = $(RTL_DIR)/general_features_test.domains.yaml
GEN_SV_PKG = $(GENERATED_DIR)/general_features_test_dpi_pkg.sv
GEN_SV = $(GENERATED_DIR)/general_features_test_dpi.sv
GEN_CPP = $(GENERATED_DIR)/general_features_test_dpi.cpp
GEN_STAMP = $(GENERATED_DIR)/.stamp

TB_SRCS = \
	$(RTL_SRC) \
	$(TB_DIR)/generated/uut_if.sv \
	$(TB_DIR)/uut_tb.sv \
	$(TB_DIR)/common/tb_common.sv \
	$(TB_DIR)/dpi/checker.sv \
	$(TB_DIR)/dpi/tb.sv \
	$(GEN_SV_PKG) \
	$(GEN_SV)

DPI_CPP = $(GEN_CPP)

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
	-I$(abspath $(PROJECT_ROOT)/external/slang/external/ieee1800)

all: simulate

prep: force
	$(MAKE) -C $(PROJECT_ROOT) dev

$(GEN_STAMP): prep $(RTL_SRC) $(DOMAINS_SRC)
	mkdir -p $(GENERATED_DIR)
	$(BUILD_DIR)/mate-dpi-codegen \
		--top general_features_test \
		--domains $(DOMAINS_SRC) \
		--out-dir $(GENERATED_DIR) \
		--module-name general_features_test_dpi \
		--function-prefix mate_gft \
		$(RTL_SRC)
	touch $(GEN_STAMP)

$(GEN_SV_PKG) $(GEN_SV) $(GEN_CPP): $(GEN_STAMP)

$(OUT): prep $(TB_SRCS) $(DPI_CPP) $(MATE_LIBS)
	rm -rf obj_dir
	verilator --trace --timing --cc --exe --build --main \
		$(VERILATOR_WARNS) \
		--top-module $(TOP_MODULE) \
		-CFLAGS '$(MATE_CPPFLAGS)' \
		-LDFLAGS '$(MATE_LIBS)' \
		$(TB_SRCS) \
		$(DPI_CPP)

$(WAVES): $(OUT) force
	./$(OUT) +WAVES=$(WAVES) $(SEED_ARG)

simulate:
	$(MAKE) $(WAVES)

force:

clean:
	rm -rf obj_dir $(WAVES) $(GENERATED_DIR)
