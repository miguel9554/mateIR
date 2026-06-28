.PHONY: simulate clean force prep

WAVES = waves.vcd

ROOT_DIR       = ../..
PROJECT_ROOT   = $(ROOT_DIR)/../..
RTL_DIR        = $(ROOT_DIR)/rtl
TB_DIR         = $(ROOT_DIR)/tb
GEN_DIR        = $(TB_DIR)/generated
MODULE_NAME    = $(notdir $(realpath $(ROOT_DIR)))
CUSTOM_SIM_DIR = $(ROOT_DIR)/work/custom-sim

# Source file discovery — packages before modules that import them
RTL_GEN_DIR  = $(RTL_DIR)/generated
RTL_PKGS     = $(shell find -L $(RTL_DIR) -path $(RTL_GEN_DIR) -prune -o -type f -name "*_pkg.sv" -print)
RTL_SRCS     = $(RTL_PKGS) $(shell find -L $(RTL_DIR) -path $(RTL_GEN_DIR) -prune -o -type f \( -name "*.v" -o -name "*.sv" \) -print | grep -v "_pkg.sv")
DPI_DIR      = $(TB_DIR)/dpi
TB_SRCS      = $(shell find -L $(TB_DIR) \( -path $(GEN_DIR) -o -path $(DPI_DIR) \) -prune -o -type f \( -name "*.v" -o -name "*.sv" \) -print)
GEN_SRCS     = $(shell find -L $(GEN_DIR) -type f \( -name "*.v" -o -name "*.sv" \) 2>/dev/null)
DOMAINS_YAML = $(RTL_DIR)/$(MODULE_NAME).domains.yaml
TOP_MODULE   = tb
OUT          = obj_dir/V$(TOP_MODULE)
GEN_TB_SCRIPT = $(PROJECT_ROOT)/tools/gen_tb.py

VERILATOR_WARNS ?= -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC
VERILATOR_WARNS += $(shell cat verilator.warns 2>/dev/null)
SEED_ARG = $(if $(seed),+verilator+seed+$(seed))

# Set DPI=1 to build and run against the compiled DPI model instead of RTL-only.
DPI ?= 0

ifeq ($(DPI),1)

BUILD_DIR   = $(PROJECT_ROOT)/build/dev
DPI_GEN_DIR = $(RTL_GEN_DIR)
GEN_SV_PKG  = $(DPI_GEN_DIR)/$(MODULE_NAME)_dpi_pkg.sv
GEN_SV      = $(DPI_GEN_DIR)/$(MODULE_NAME)_dpi.sv
GEN_CPP     = $(DPI_GEN_DIR)/$(MODULE_NAME)_dpi.cpp
GEN_STAMP   = $(DPI_GEN_DIR)/.stamp

MATE_LIBS = \
	$(abspath $(BUILD_DIR)/libmate-dpi-support.a) \
	$(abspath $(BUILD_DIR)/libmate-rtl-runtime-compiler.a) \
	$(abspath $(BUILD_DIR)/libmate-systemverilog-frontend.a) \
	$(abspath $(BUILD_DIR)/libmate-rtl-runtime.a) \
	$(abspath $(BUILD_DIR)/libmate-mateir.a) \
	$(abspath $(PROJECT_ROOT)/build/external/yaml-cpp/libyaml-cpp.a) \
	/opt/slang/lib/libsvlang.a \
	/usr/local/lib/libfmt.a

MATE_CPPFLAGS = \
	-std=c++20 \
	-I$(abspath $(PROJECT_ROOT)/src) \
	-I$(abspath $(PROJECT_ROOT)/external/slang/external/ieee1800)

SRCS = $(RTL_SRCS) $(TB_SRCS) $(GEN_SRCS) $(GEN_SV_PKG) $(GEN_SV)

$(info RTL_SRCS=$(RTL_SRCS))
$(info TB_SRCS=$(TB_SRCS))
$(info GEN_SRCS=$(GEN_SRCS))

prep: force
	$(MAKE) -C $(PROJECT_ROOT) dev

$(GEN_DIR)/tb.sv $(GEN_DIR)/uut_if.sv $(GEN_DIR)/uut_recorder.sv $(GEN_DIR)/checker_dpi.sv: $(RTL_SRCS) $(DOMAINS_YAML) $(GEN_TB_SCRIPT) force
	cd $(PROJECT_ROOT) && python tools/gen_tb.py --dpi $(MODULE_NAME)

$(GEN_STAMP): prep $(RTL_SRCS) $(DOMAINS_YAML)
	mkdir -p $(DPI_GEN_DIR)
	$(BUILD_DIR)/mate-dpi-codegen \
		--top $(MODULE_NAME) \
		--domains $(DOMAINS_YAML) \
		--out-dir $(DPI_GEN_DIR) \
		--module-name $(MODULE_NAME)_dpi \
		--function-prefix mate_$(MODULE_NAME) \
		$(RTL_SRCS)
	touch $(GEN_STAMP)

$(GEN_SV_PKG) $(GEN_SV) $(GEN_CPP): $(GEN_STAMP)

$(OUT): prep $(GEN_DIR)/tb.sv $(GEN_CPP) $(MATE_LIBS)
	rm -rf obj_dir
	verilator --trace --timing --cc --exe --build --main \
		$(VERILATOR_WARNS) \
		--top-module $(TOP_MODULE) \
		-CFLAGS '$(MATE_CPPFLAGS)' \
		-LDFLAGS '$(MATE_LIBS)' \
		$(SRCS) \
		$(GEN_CPP)

clean:
	rm -rf obj_dir $(WAVES) $(DPI_GEN_DIR)

else

SRCS = $(RTL_SRCS) $(TB_SRCS) $(GEN_SRCS)

$(info RTL_SRCS=$(RTL_SRCS))
$(info TB_SRCS=$(TB_SRCS))
$(info GEN_SRCS=$(GEN_SRCS))

$(GEN_DIR)/tb.sv $(GEN_DIR)/uut_if.sv $(GEN_DIR)/uut_recorder.sv $(GEN_DIR)/checker_dpi.sv: $(RTL_SRCS) $(DOMAINS_YAML) $(GEN_TB_SCRIPT) force
	cd $(PROJECT_ROOT) && python tools/gen_tb.py $(MODULE_NAME)

$(OUT): $(SRCS) $(GEN_DIR)/tb.sv
	rm -rf obj_dir
	verilator --trace --binary --x-initial unique --main-top-name TOP \
		$(VERILATOR_WARNS) --top $(TOP_MODULE) -I$(RTL_DIR) $(SRCS)

clean:
	rm -rf obj_dir $(WAVES)

endif

$(WAVES): $(OUT) force
	mkdir -p $(CUSTOM_SIM_DIR)/stimuli
	./obj_dir/V$(TOP_MODULE) +WAVES=$(WAVES) $(SEED_ARG)

simulate:
	$(MAKE) $(WAVES)

force:
