.PHONY: simulate clean force prep

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

# Source file discovery — packages before modules that import them
RTL_GEN_DIR  = $(RTL_DIR)/generated
RTL_PKGS     = $(shell find -L $(RTL_DIR) -path $(RTL_GEN_DIR) -prune -o -type f -name "*_pkg.sv" -print)
RTL_SRCS     = $(RTL_PKGS) $(shell find -L $(RTL_DIR) -path $(RTL_GEN_DIR) -prune -o -type f \( -name "*.v" -o -name "*.sv" \) -print | grep -v "_pkg.sv")
DPI_DIR      = $(TB_DIR)/dpi
TB_SRCS      = $(shell find -L $(TB_DIR) \( -path $(GEN_DIR) -o -path $(DPI_DIR) \) -prune -o -type f \( -name "*.v" -o -name "*.sv" \) -print)
# Wall-clock time DPI helper, used by tb_common for simulation-speed reporting.
DPI_TIME_C   = $(TB_DIR)/common/dpi_time.c
GEN_SRCS     = $(shell find -L $(GEN_DIR) -type f \( -name "*.v" -o -name "*.sv" \) 2>/dev/null)
DOMAINS_YAML = $(RTL_DIR)/$(MODULE_NAME).domains.yaml
TOP_MODULE   = tb
OUT          = obj_dir/V$(TOP_MODULE)
GEN_TB_SCRIPT = $(PROJECT_ROOT)/tools/gen_tb.py
GEN_TB_OUTPUTS = $(GEN_DIR)/tb.sv $(GEN_DIR)/uut_if.sv $(GEN_DIR)/uut_recorder.sv $(GEN_DIR)/checker_dpi.sv

VERILATOR_WARNS ?= -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC
VERILATOR_WARNS += $(shell cat verilator.warns 2>/dev/null)
SEED_ARG = $(if $(seed),+verilator+seed+$(seed))

# Set DPI=1 to build and run against the compiled DPI model instead of RTL-only.
DPI ?= 0
# Set GENERATE_TB=0 to use existing files in tb/generated without rerunning gen_tb.py.
GENERATE_TB ?= 1

ifneq ($(GENERATE_TB),0)
ifneq ($(GENERATE_TB),1)
$(error GENERATE_TB must be 0 or 1)
endif
endif

ifeq ($(DPI),1)

DPI_BUILD_TARGET ?= dev
# When DPI_BUILD_TARGET=noop (regression pre-builds the tree once upfront),
# DPI_BUILD_PRESET still selects which prebuilt build dir to use.
DPI_BUILD_PRESET ?= $(if $(filter noop,$(DPI_BUILD_TARGET)),dev,$(DPI_BUILD_TARGET))
BUILD_DIR   = $(PROJECT_ROOT)/build/$(DPI_BUILD_PRESET)
DPI_GEN_DIR = $(RTL_GEN_DIR)
GEN_SV_PKG  = $(DPI_GEN_DIR)/$(MODULE_NAME)_dpi_pkg.sv
GEN_SV      = $(DPI_GEN_DIR)/$(MODULE_NAME)_dpi.sv
DPI_LIB     = $(DPI_GEN_DIR)/$(MODULE_NAME)_dpi.a
GEN_STAMP   = $(DPI_GEN_DIR)/.stamp

# mate --dpi-lib compiles the generated DPI glue/model into one self-contained
# static library; Verilator only needs to link it, not compile any of our
# C++ itself. These are the only two things that step needs to know about our
# tree: where svdpi.h/our headers live, and which of our own static libraries
# to fold into the output archive.
DPI_INCLUDE_DIRS = $(abspath $(PROJECT_ROOT)/src),$(abspath $(PROJECT_ROOT)/external/slang/external/ieee1800)
DPI_LINK_LIBS = $(abspath $(BUILD_DIR)/libmate-abi-native.a)

# Sanitized preset: compile the generated DPI model with the same sanitizer
# flags as libmate-abi-native (see CMakeLists ENABLE_SANITIZERS), pull the
# sanitizer runtimes into the final Verilator link, and run both mate and the
# simulation binary with the repo's standard ASan/UBSan options.
DPI_CXX_FLAGS_ARG =
DPI_SANITIZE_LDFLAGS =
ifeq ($(DPI_BUILD_PRESET),sanitized)
DPI_SANITIZE_FLAGS = -fno-omit-frame-pointer -fsanitize=address,undefined
DPI_CXX_FLAGS_ARG = --dpi-cxx-flags '$(DPI_SANITIZE_FLAGS)'
DPI_SANITIZE_LDFLAGS = -fsanitize=address,undefined
SANITIZER_ENV = ASAN_OPTIONS=symbolize=1,detect_leaks=0,abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1,halt_on_error=1
endif

SRCS = $(RTL_SRCS) $(TB_SRCS) $(GEN_SRCS) $(GEN_SV_PKG) $(GEN_SV)

$(info RTL_SRCS=$(RTL_SRCS))
$(info TB_SRCS=$(TB_SRCS))
$(info GEN_SRCS=$(GEN_SRCS))

prep: force
	@if [ "$(DPI_BUILD_TARGET)" != "noop" ]; then \
		$(MAKE) -C $(PROJECT_ROOT) $(DPI_BUILD_TARGET); \
	fi

ifeq ($(GENERATE_TB),1)
$(GEN_TB_OUTPUTS): $(RTL_SRCS) $(DOMAINS_YAML) $(GEN_TB_SCRIPT) force
	cd $(PROJECT_ROOT) && python tools/gen_tb.py --dpi $(MODULE_NAME)
endif

$(GEN_STAMP): $(RTL_SRCS) $(DOMAINS_YAML) $(BUILD_DIR)/mate | prep
	mkdir -p $(DPI_GEN_DIR)
	$(SANITIZER_ENV) $(BUILD_DIR)/mate \
		--dpi-lib \
		--top $(MODULE_NAME) \
		--domains $(DOMAINS_YAML) \
		--output-dir $(DPI_GEN_DIR) \
		--module-name $(MODULE_NAME)_dpi \
		--function-prefix mate_$(MODULE_NAME) \
		--dpi-out-lib $(DPI_LIB) \
		--dpi-include-dirs $(DPI_INCLUDE_DIRS) \
		--dpi-link-libs $(DPI_LINK_LIBS) \
		$(DPI_CXX_FLAGS_ARG) \
		$(RTL_SRCS)
	touch $(GEN_STAMP)

$(GEN_SV_PKG) $(GEN_SV) $(DPI_LIB): $(GEN_STAMP)

$(OUT): $(GEN_DIR)/tb.sv $(DPI_LIB) | prep
	rm -rf obj_dir
	verilator $(TRACE_FLAG) --timing --cc --exe --build --main \
		$(VERILATOR_WARNS) \
		--top-module $(TOP_MODULE) \
		-I$(RTL_DIR) \
		-LDFLAGS '$(abspath $(DPI_LIB)) $(DPI_SANITIZE_LDFLAGS)' \
		$(SRCS) \
		$(DPI_TIME_C)

clean:
	rm -rf obj_dir waves.vcd waves.fst $(DPI_GEN_DIR)

else

SRCS = $(RTL_SRCS) $(TB_SRCS) $(GEN_SRCS)

$(info RTL_SRCS=$(RTL_SRCS))
$(info TB_SRCS=$(TB_SRCS))
$(info GEN_SRCS=$(GEN_SRCS))

ifeq ($(GENERATE_TB),1)
$(GEN_TB_OUTPUTS): $(RTL_SRCS) $(DOMAINS_YAML) $(GEN_TB_SCRIPT) force
	cd $(PROJECT_ROOT) && python tools/gen_tb.py $(MODULE_NAME)
endif

$(OUT): $(SRCS) $(GEN_DIR)/tb.sv
	rm -rf obj_dir
	verilator $(TRACE_FLAG) --binary --x-initial unique --main-top-name TOP \
		$(VERILATOR_WARNS) --top $(TOP_MODULE) -I$(RTL_DIR) $(SRCS) $(DPI_TIME_C)

clean:
	rm -rf obj_dir waves.vcd waves.fst

endif

# Extra +plusargs for the sim run, e.g. PLUSARGS=+PROGRESS_STEP_NS=500
PLUSARGS ?=

$(WAVES): $(OUT) force
	mkdir -p $(CUSTOM_SIM_DIR)/stimuli
	$(SANITIZER_ENV) ./obj_dir/V$(TOP_MODULE) +WAVES=$(WAVES) $(SEED_ARG) $(PLUSARGS)

simulate:
	$(MAKE) $(WAVES)

force:
