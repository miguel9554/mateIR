.PHONY: simulate gdb clean svg flatten-vcd

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
MODULE_NAME = $(notdir $(realpath $(ROOT_DIR)))
STIMULI_DIR = stimuli
OUTPUT_DIR = output
SIM_BUILD_TARGET ?= dev
SIM_BUILD_PRESET ?= $(if $(filter noop,$(SIM_BUILD_TARGET)),dev,$(SIM_BUILD_TARGET))
SIMULATOR_BUILD_DIR = $(PROJECT_ROOT)/build/$(SIM_BUILD_PRESET)
VCD_FLATTEN ?= python3 $(PROJECT_ROOT)/tools/vcd_flatten.py

RTL_GEN_DIR = $(RTL_DIR)/generated
RTL_SRCS = $(shell find -L $(RTL_DIR) -path $(RTL_GEN_DIR) -prune -o -type f \( -name "*.v" -o -name "*.sv" \) -print)
DOMAINS_YAML = $(RTL_DIR)/$(MODULE_NAME).domains.yaml

SIMULATOR = $(SIMULATOR_BUILD_DIR)/mate-vector-simulator
GROUPED_VCD = $(OUTPUT_DIR)/$(MODULE_NAME).vcd
FLAT_VCD = $(OUTPUT_DIR)/$(MODULE_NAME)-raw.vcd
SANITIZER_ENV =
ifeq ($(SIM_BUILD_PRESET),sanitized)
SANITIZER_ENV = ASAN_OPTIONS=symbolize=1,detect_leaks=0,abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1,halt_on_error=1
endif

# Per-test optional args (from custom-sim.args file)
EXTRA_ARGS = $(shell cat custom-sim.args 2>/dev/null)

all: simulate

DEBUG_NODES_ARG = $(if $(DEBUG_NODES),--debug-nodes $(DEBUG_NODES),)

simulate:
	$(MAKE) -C $(PROJECT_ROOT) $(SIM_BUILD_TARGET)
	rm -rf debug_output
	$(SANITIZER_ENV) $(SIMULATOR) --top $(MODULE_NAME) \
		--inputs-dir $(STIMULI_DIR) --output-dir $(OUTPUT_DIR) \
		--domains $(DOMAINS_YAML) \
		--flops-initial zeros \
		$(DEBUG_NODES_ARG) \
		$(EXTRA_ARGS) $(RTL_SRCS)
	$(MAKE) flatten-vcd

flatten-vcd: $(GROUPED_VCD)
	$(VCD_FLATTEN) $(GROUPED_VCD) $(FLAT_VCD)

gdb:
	$(MAKE) -C $(PROJECT_ROOT) debug
	gdb --args $(PROJECT_ROOT)/build/debug/mate-vector-simulator --top $(MODULE_NAME) \
		--inputs-dir $(STIMULI_DIR) --output-dir $(OUTPUT_DIR) \
		--domains $(DOMAINS_YAML) \
		--flops-initial zeros \
		$(DEBUG_NODES_ARG) \
		$(EXTRA_ARGS) $(RTL_SRCS)

svg:
	#find debug_output -name '*.dot' -exec sh -c 'dot -Tsvg -o "$${1%.dot}.svg" "$$1"' _ {} \;

clean:
	rm -rf $(OUTPUT_DIR) debug_output
