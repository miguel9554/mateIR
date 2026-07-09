.PHONY: simulate clean prep force

TRACE_FORMAT ?= VCD
ifeq ($(TRACE_FORMAT),VCD)
TRACE_FLAG = --trace --trace-underscore
TRACE_EXT = vcd
else ifeq ($(TRACE_FORMAT),FST)
TRACE_FLAG = --trace-fst --trace-underscore
TRACE_EXT = fst
else
$(error TRACE_FORMAT must be VCD or FST)
endif

WAVES ?= waves.$(TRACE_EXT)

ROOT_DIR       = ../..
PROJECT_ROOT   = $(ROOT_DIR)/../..
RTL_DIR        = $(ROOT_DIR)/rtl
TB_DIR         = $(ROOT_DIR)/tb
GEN_DIR        = $(TB_DIR)/generated
MODULE_NAME    = $(notdir $(realpath $(ROOT_DIR)))
CUSTOM_SIM_DIR = $(ROOT_DIR)/work/custom-sim

RTL_SRCS      = $(shell find -L $(RTL_DIR) -type f \( -name "*.v" -o -name "*.sv" \) -print)
DPI_GEN_DIR   = $(RTL_DIR)/generated
DOMAINS_YAML  = $(RTL_DIR)/$(MODULE_NAME).domains.yaml
DPI ?= 0

ifeq ($(DPI),1)
BUILD_DIR   = $(PROJECT_ROOT)/build/dev
DPI_BUILD_TARGET ?= dev
DPI_INCLUDE_DIRS = $(abspath $(PROJECT_ROOT)/src),$(abspath $(PROJECT_ROOT)/external/slang/external/ieee1800)
DPI_LINK_LIBS = $(abspath $(BUILD_DIR)/libmate-abi-native.a)
GEN_STAMP   = $(DPI_GEN_DIR)/.stamp
DPI_LIB     = $(DPI_GEN_DIR)/$(MODULE_NAME)_dpi.a
GEN_SV_PKG  = $(DPI_GEN_DIR)/$(MODULE_NAME)_dpi_pkg.sv
GEN_SV      = $(DPI_GEN_DIR)/$(MODULE_NAME)_dpi.sv
OUT         = obj_dir/Vtb

prep:
	@if [ "$(DPI_BUILD_TARGET)" != "noop" ]; then \
		$(MAKE) -C $(PROJECT_ROOT) $(DPI_BUILD_TARGET); \
	fi

$(GEN_STAMP): $(RTL_SRCS) $(DOMAINS_YAML) $(BUILD_DIR)/mate | prep
	mkdir -p $(DPI_GEN_DIR)
	$(BUILD_DIR)/mate \
		--dpi-lib \
		--top $(MODULE_NAME) \
		--domains $(DOMAINS_YAML) \
		--output-dir $(DPI_GEN_DIR) \
		--module-name $(MODULE_NAME)_dpi \
		--function-prefix mate_$(MODULE_NAME) \
		--dpi-out-lib $(DPI_LIB) \
		--dpi-include-dirs $(DPI_INCLUDE_DIRS) \
		--dpi-link-libs $(DPI_LINK_LIBS) \
		$(RTL_SRCS)
	touch $(GEN_STAMP)

$(GEN_SV_PKG) $(GEN_SV) $(DPI_LIB): $(GEN_STAMP)

$(OUT): $(RTL_SRCS) $(TB_DIR)/clockless_dpi_tb.sv $(GEN_SV_PKG) $(GEN_SV) $(DPI_LIB) | prep
	rm -rf obj_dir
	verilator $(TRACE_FLAG) --timing --cc --exe --build --main \
		-Wno-TIMESCALEMOD \
		--top-module tb \
		-I$(RTL_DIR) \
		-LDFLAGS '$(abspath $(DPI_LIB))' \
		$(RTL_SRCS) $(GEN_SV_PKG) $(GEN_SV) $(TB_DIR)/clockless_dpi_tb.sv \
		$(TB_DIR)/common/dpi_time.c

$(WAVES): $(OUT) force
	./obj_dir/Vtb +WAVES=$(WAVES)

else
OUT = obj_dir/Vtb
TB_SRCS = \
	$(TB_DIR)/common/signal_checker.sv \
	$(TB_DIR)/common/recorder.sv \
	$(TB_DIR)/common/tb_pkg.sv \
	$(TB_DIR)/common/tb_common.sv \
	$(TB_DIR)/clockless_rtl_tb.sv

prep:
	@true

$(OUT): $(RTL_SRCS) $(TB_SRCS) | prep
	rm -rf obj_dir
	verilator $(TRACE_FLAG) --binary --x-initial unique --main-top-name TOP \
		-Wno-TIMESCALEMOD --top tb -I$(RTL_DIR) \
		$(RTL_SRCS) $(TB_SRCS) $(TB_DIR)/common/dpi_time.c

$(WAVES): $(OUT) force
	mkdir -p $(CUSTOM_SIM_DIR)/stimuli
	./obj_dir/Vtb +WAVES=$(WAVES)
endif

simulate: $(WAVES)

clean:
	rm -rf obj_dir waves.vcd waves.fst $(DPI_GEN_DIR)

force:
