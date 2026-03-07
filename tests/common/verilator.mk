.PHONY: simulate clean force

# Waveform database name
WAVES = waves.vcd

# Directories
ROOT_DIR = ../..
RTL_DIR = $(ROOT_DIR)/rtl
TB_DIR = $(ROOT_DIR)/tb

# Source files
RTL_SRCS = $(shell find -L $(RTL_DIR) -type f \( -name "*.v" -o -name "*.sv" \))
TB_SRCS  = $(shell find -L $(TB_DIR)  -type f \( -name "*.v" -o -name "*.sv" \))
SRCS= $(RTL_SRCS) $(TB_SRCS)
TOP_MODULE = tb

$(info RTL_SRCS=$(RTL_SRCS))
$(info TB_SRCS=$(TB_SRCS))

# Compiled file name
OUT = obj_dir/V$(TOP_MODULE)

VERILATOR_WARNS ?= -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC
VERILATOR_OPTS ?= --trace --binary --x-initial unique

# seed 0 uses a random seed
SEED_ARG = $(if $(seed),+verilator+seed+$(seed))

# Default target
all: simulate

# Compile the TB and RTL
$(OUT): $(SRCS)
	verilator $(VERILATOR_OPTS) $(VERILATOR_WARNS) --top $(TOP_MODULE) $(SRCS)

# Generate waves database
$(WAVES): $(OUT) force
	./obj_dir/V$(TOP_MODULE) +WAVES=$(WAVES) $(SEED_ARG)

simulate: $(WAVES)

force:

clean:
	rm -rf obj_dir $(WAVES)
