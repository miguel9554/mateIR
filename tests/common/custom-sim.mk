.PHONY: simulate clean force

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
MODULE_NAME = $(notdir $(realpath $(ROOT_DIR)))
STIMULI_DIR = stimuli
OUTPUT_DIR = output

RTL_SRCS = $(shell find -L $(RTL_DIR) -type f \( -name "*.v" -o -name "*.sv" \))

SIMULATOR = $(PROJECT_ROOT)/build/custom_hdl_compiler

# Per-test optional args (from custom-sim.args file)
EXTRA_ARGS = $(shell cat custom-sim.args 2>/dev/null)

all: simulate

# Rebuild simulator via root Makefile
$(SIMULATOR): force
	$(MAKE) -C $(PROJECT_ROOT) build

# Symlink for convenience
sim: $(SIMULATOR)
	ln -sf $(realpath $(SIMULATOR)) sim

simulate: sim
	./sim --simulate --top $(MODULE_NAME) \
		--inputs-dir $(STIMULI_DIR) --output-dir $(OUTPUT_DIR) \
		$(EXTRA_ARGS) $(RTL_SRCS)

clean:
	rm -rf $(OUTPUT_DIR) sim

force:
