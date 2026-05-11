.PHONY: validate clean

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
MODULE_NAME = $(notdir $(realpath $(ROOT_DIR)))
SIMULATOR = $(PROJECT_ROOT)/build/sanitized/mate
RTL_SRCS = $(shell find -L $(RTL_DIR) -type f \( -name "*.v" -o -name "*.sv" \))

validate:
	$(MAKE) -C $(PROJECT_ROOT) sanitized
	rm -rf debug_output
	$(SIMULATOR) --top $(MODULE_NAME) --infer-top-domains $(RTL_SRCS)

clean:
	rm -rf debug_output
