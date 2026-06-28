.PHONY: simulate clean force

# Waveform database name
WAVES = waves.vcd

# Directories
ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
TB_DIR = $(ROOT_DIR)/tb
GEN_DIR = $(TB_DIR)/generated
MODULE_NAME = $(notdir $(realpath $(ROOT_DIR)))

# Source files — packages must come before modules that import them
RTL_GEN_DIR = $(RTL_DIR)/generated
RTL_PKGS    = $(shell find -L $(RTL_DIR) -path $(RTL_GEN_DIR) -prune -o -type f -name "*_pkg.sv" -print)
RTL_SRCS    = $(RTL_PKGS) $(shell find -L $(RTL_DIR) -path $(RTL_GEN_DIR) -prune -o -type f \( -name "*.v" -o -name "*.sv" \) -print | grep -v "_pkg.sv")
DPI_DIR     = $(TB_DIR)/dpi
TB_SRCS     = $(shell find -L $(TB_DIR) \( -path $(GEN_DIR) -o -path $(DPI_DIR) \) -prune -o -type f \( -name "*.v" -o -name "*.sv" \) -print)
GEN_SRCS    = $(shell find -L $(GEN_DIR) -type f \( -name "*.v" -o -name "*.sv" \) 2>/dev/null)
DOMAINS_YAML = $(RTL_DIR)/$(MODULE_NAME).domains.yaml
SRCS = $(RTL_SRCS) $(TB_SRCS) $(GEN_SRCS)
TOP_MODULE = tb

$(info RTL_SRCS=$(RTL_SRCS))
$(info TB_SRCS=$(TB_SRCS))
$(info GEN_SRCS=$(GEN_SRCS))

# Compiled file name
OUT = obj_dir/V$(TOP_MODULE)
CUSTOM_SIM_DIR = $(ROOT_DIR)/work/custom-sim

VERILATOR_WARNS ?= -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC
# Per-test extra warnings suppression (place -Wno-XXX flags in verilator.warns)
VERILATOR_WARNS += $(shell cat verilator.warns 2>/dev/null)
VERILATOR_OPTS ?= --trace --binary --x-initial unique --main-top-name TOP

# seed 0 uses a random seed
SEED_ARG = $(if $(seed),+verilator+seed+$(seed))

# Default target
all: simulate

GEN_TB_SCRIPT = $(PROJECT_ROOT)/tools/gen_tb.py

# Auto-generate TB files from RTL + domains.yaml
$(GEN_DIR)/tb.sv $(GEN_DIR)/uut_if.sv $(GEN_DIR)/uut_recorder.sv: $(RTL_SRCS) $(DOMAINS_YAML) $(GEN_TB_SCRIPT)
	cd $(PROJECT_ROOT) && python tools/gen_tb.py $(MODULE_NAME)

# Compile the TB and RTL
$(OUT): $(SRCS) $(GEN_DIR)/tb.sv
	rm -rf obj_dir
	verilator $(VERILATOR_OPTS) $(VERILATOR_WARNS) --top $(TOP_MODULE) -I$(RTL_DIR) $(SRCS)

# Generate waves database
$(WAVES): $(OUT) force
	mkdir -p $(CUSTOM_SIM_DIR)/stimuli
	./obj_dir/V$(TOP_MODULE) +WAVES=$(WAVES) $(SEED_ARG)

simulate:
	$(MAKE) $(WAVES)

force:

clean:
	rm -rf obj_dir $(WAVES)
