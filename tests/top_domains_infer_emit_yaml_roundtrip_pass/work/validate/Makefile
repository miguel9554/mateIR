.PHONY: validate clean

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
MODULE_NAME = $(notdir $(realpath $(ROOT_DIR)))
SIMULATOR = $(PROJECT_ROOT)/build/sanitized/mate
RTL_SRCS = $(shell find -L $(RTL_DIR) -type f \( -name "*.v" -o -name "*.sv" \))
EMITTED_YAML = $(CURDIR)/$(MODULE_NAME).generated.domains.yaml
EXPECTED_YAML = $(RTL_DIR)/$(MODULE_NAME).expected.domains.yaml

validate:
	$(MAKE) -C $(PROJECT_ROOT) sanitized
	rm -rf debug_output infer.log roundtrip.log $(EMITTED_YAML)
	$(SIMULATOR) --top $(MODULE_NAME) --infer-top-domains \
		--emit-inferred-domains $(EMITTED_YAML) $(RTL_SRCS) > infer.log 2>&1
	diff -u $(EXPECTED_YAML) $(EMITTED_YAML)
	$(SIMULATOR) --top $(MODULE_NAME) --domains $(EMITTED_YAML) $(RTL_SRCS) > roundtrip.log 2>&1

clean:
	rm -rf debug_output infer.log roundtrip.log $(EMITTED_YAML)
