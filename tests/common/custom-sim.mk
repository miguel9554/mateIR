.PHONY: simulate gdb clean svg

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
MODULE_NAME = $(notdir $(realpath $(ROOT_DIR)))
STIMULI_DIR = stimuli
OUTPUT_DIR = output

RTL_SRCS = $(shell find -L $(RTL_DIR) -type f \( -name "*.v" -o -name "*.sv" \))
DOMAINS_YAMLS = $(shell find -L $(RTL_DIR) -name '*.domains.yaml')

SIMULATOR = $(PROJECT_ROOT)/build/custom_hdl_compiler

# Per-test optional args (from custom-sim.args file)
EXTRA_ARGS = $(shell cat custom-sim.args 2>/dev/null)

all: simulate

DEBUG_NODES_ARG = $(if $(DEBUG_NODES),--debug-nodes $(DEBUG_NODES),)

simulate:
	$(MAKE) -C $(PROJECT_ROOT) build
	$(SIMULATOR) --simulate --top $(MODULE_NAME) \
		--inputs-dir $(STIMULI_DIR) --output-dir $(OUTPUT_DIR) \
		--domains $(DOMAINS_YAMLS) \
		--flops-initial zeros \
		$(DEBUG_NODES_ARG) \
		$(EXTRA_ARGS) $(RTL_SRCS)

gdb:
	$(MAKE) -C $(PROJECT_ROOT) debug-build
	gdb --args $(SIMULATOR) --simulate --top $(MODULE_NAME) \
		--inputs-dir $(STIMULI_DIR) --output-dir $(OUTPUT_DIR) \
		--domains $(DOMAINS_YAMLS) \
		--flops-initial zeros \
		$(DEBUG_NODES_ARG) \
		$(EXTRA_ARGS) $(RTL_SRCS)

svg:
	find debug_output -name '*.dot' -exec sh -c 'dot -Tsvg -o "$${1%.dot}.svg" "$$1"' _ {} \;

clean:
	rm -rf $(OUTPUT_DIR)
