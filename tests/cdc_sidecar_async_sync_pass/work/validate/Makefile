.PHONY: validate clean clean_all

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
SIMULATOR = $(PROJECT_ROOT)/build/sanitized/mate
RTL_SRCS = $(shell find -L $(RTL_DIR) -type f \( -name "*.v" -o -name "*.sv" \))
DOMAINS_YAML = $(RTL_DIR)/$(notdir $(realpath $(ROOT_DIR))).domains.yaml

validate:
	$(MAKE) -C $(PROJECT_ROOT) sanitized
	$(SIMULATOR) --top cdc_sidecar_async_sync_pass --domains $(DOMAINS_YAML) $(RTL_SRCS)

clean:

clean_all: clean
