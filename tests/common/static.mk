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
DOMAINS_ARG = $(if $(wildcard $(DOMAINS_YAML)),--domains $(DOMAINS_YAML),)

STATIC_ANALYZER = $(STATIC_BUILD_DIR)/mate
SANITIZER_ENV = ASAN_OPTIONS=symbolize=1,detect_leaks=0,abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1,halt_on_error=1

# Per-test optional args (from static.args file)
EXTRA_ARGS = $(shell cat static.args 2>/dev/null)

all: analyze

analyze:
	$(MAKE) -C $(PROJECT_ROOT) $(STATIC_BUILD_TARGET)
	rm -rf debug_output
	$(SANITIZER_ENV) $(STATIC_ANALYZER) --analyze --top $(MODULE_NAME) \
		$(DOMAINS_ARG) \
		$(EXTRA_ARGS) $(RTL_SRCS)

clean:
	rm -rf debug_output
