.PHONY: validate clean clean_all

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
SIMULATOR = $(PROJECT_ROOT)/build/sanitized/mate
RTL_SRCS = $(shell find -L $(RTL_DIR) -type f \( -name "*.v" -o -name "*.sv" \))
DOMAINS_YAMLS = $(shell find -L $(RTL_DIR) -name '*.domains.yaml')

validate:
	$(MAKE) -C $(PROJECT_ROOT) sanitized
	$(SIMULATOR) --top global_domains_opposite_edges --domains $(DOMAINS_YAMLS) $(RTL_SRCS)

clean:

clean_all: clean
