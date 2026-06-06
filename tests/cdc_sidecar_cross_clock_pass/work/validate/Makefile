.PHONY: validate clean clean_all

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
SIM_BUILD_TARGET ?= dev
SIMULATOR = $(PROJECT_ROOT)/build/$(SIM_BUILD_TARGET)/mate
RTL_SRCS = $(shell find -L $(RTL_DIR) -type f \( -name "*.v" -o -name "*.sv" \))
DOMAINS_YAML = $(RTL_DIR)/$(notdir $(realpath $(ROOT_DIR))).domains.yaml

validate:
	$(MAKE) -C $(PROJECT_ROOT) $(SIM_BUILD_TARGET)
	$(SIMULATOR) --top cdc_sidecar_cross_clock_pass --domains $(DOMAINS_YAML) $(RTL_SRCS)

clean:

clean_all: clean
