.PHONY: analyze clean

ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
RTL_DIR = $(ROOT_DIR)/rtl
MODULE_NAME = $(notdir $(realpath $(ROOT_DIR)))
STATIC_BUILD_TARGET ?= sanitized
STATIC_BUILD_DIR = $(PROJECT_ROOT)/build/sanitized

RTL_PKGS = $(shell find -L $(RTL_DIR) -type f -name "*_pkg.sv")
RTL_SRCS = $(RTL_PKGS) $(shell find -L $(RTL_DIR) -type f \( -name "*.v" -o -name "*.sv" \) | grep -v "_pkg.sv")
DOMAINS_YAML = $(RTL_DIR)/$(MODULE_NAME).domains.yaml
INFER_TOP_DOMAINS_ARG = $(if $(INFER_TOP_DOMAINS),--infer-top-domains,)
INFER_SYNCHRONIZERS_ARG = $(if $(INFER_SYNCHRONIZERS),--infer-synchronizers,)
DOMAINS_ARG = $(if $(INFER_TOP_DOMAINS),,$(if $(wildcard $(DOMAINS_YAML)),--domains $(DOMAINS_YAML),))

STATIC_ANALYZER = $(STATIC_BUILD_DIR)/mate
SANITIZER_ENV = ASAN_OPTIONS=symbolize=1,detect_leaks=0,abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1,halt_on_error=1

# Per-test optional args (from static.args file)
EXTRA_ARGS = $(shell cat static.args 2>/dev/null)
DEBUG_NODES_ARG = $(if $(DEBUG_NODES),--debug-nodes $(DEBUG_NODES),)
DEBUG_NODE_DEPS_ARG = $(if $(DEBUG_NODE_DEPS),--debug-node-deps $(DEBUG_NODE_DEPS),)
DEBUG_NODE_PATHS_ARG = $(if $(DEBUG_NODE_PATHS),--debug-node-paths $(DEBUG_NODE_PATHS),)

all: analyze

analyze:
	$(MAKE) -C $(PROJECT_ROOT) $(STATIC_BUILD_TARGET)
	rm -rf debug_output
	$(SANITIZER_ENV) $(STATIC_ANALYZER) --analyze --top $(MODULE_NAME) \
		$(INFER_TOP_DOMAINS_ARG) \
		$(INFER_SYNCHRONIZERS_ARG) \
		$(DOMAINS_ARG) \
		$(DEBUG_NODES_ARG) \
		$(DEBUG_NODE_DEPS_ARG) \
		$(DEBUG_NODE_PATHS_ARG) \
		$(EXTRA_ARGS) $(RTL_SRCS)
	@find debug_output -type f -name '*.dot' -print0 | \
		xargs -0 -P$$(nproc) -I{} sh -c 'echo "Generating $${1%.dot}.svg"; dot -Tsvg "$$1" -o "$${1%.dot}.svg"' _ {}

clean:
	rm -rf debug_output
