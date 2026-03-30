.PHONY: simulate gdb clean svg

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
MODULE_NAME = $(notdir $(realpath $(ROOT_DIR)))
STIMULI_DIR = stimuli
OUTPUT_DIR = output
SIM_BUILD_TARGET ?= diagnostic-build
SIMULATOR_BUILD_DIR = $(PROJECT_ROOT)/build/diagnostic-relwithdebinfo

RTL_SRCS = $(shell find -L $(RTL_DIR) -type f \( -name "*.v" -o -name "*.sv" \))
DOMAINS_YAMLS = $(shell find -L $(RTL_DIR) -name '*.domains.yaml')

SIMULATOR = $(SIMULATOR_BUILD_DIR)/custom_hdl_compiler
SANITIZER_ENV = ASAN_OPTIONS=symbolize=1,detect_leaks=0,abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1,halt_on_error=1

# Per-test optional args (from custom-sim.args file)
EXTRA_ARGS = $(shell cat custom-sim.args 2>/dev/null)

all: simulate

DEBUG_NODES_ARG = $(if $(DEBUG_NODES),--debug-nodes $(DEBUG_NODES),)

simulate:
	$(MAKE) -C $(PROJECT_ROOT) $(SIM_BUILD_TARGET)
	$(SANITIZER_ENV) $(SIMULATOR) --simulate --top $(MODULE_NAME) \
		--inputs-dir $(STIMULI_DIR) --output-dir $(OUTPUT_DIR) \
		--domains $(DOMAINS_YAMLS) \
		--flops-initial zeros \
		$(DEBUG_NODES_ARG) \
		$(EXTRA_ARGS) $(RTL_SRCS)

gdb:
	$(MAKE) -C $(PROJECT_ROOT) debug-build
	gdb --args $(PROJECT_ROOT)/build/debug/custom_hdl_compiler --simulate --top $(MODULE_NAME) \
		--inputs-dir $(STIMULI_DIR) --output-dir $(OUTPUT_DIR) \
		--domains $(DOMAINS_YAMLS) \
		--flops-initial zeros \
		$(DEBUG_NODES_ARG) \
		$(EXTRA_ARGS) $(RTL_SRCS)

svg:
	find debug_output -name '*.dot' -exec sh -c 'dot -Tsvg -o "$${1%.dot}.svg" "$$1"' _ {} \;

clean:
	rm -rf $(OUTPUT_DIR)
