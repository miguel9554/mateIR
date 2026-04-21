ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
MODULE_NAME = $(notdir $(realpath $(ROOT_DIR)))
VCD_COMPARE_BUILD_DIR ?= $(PROJECT_ROOT)/build/vcd-compare/sanitized
VCD_COMPARE = $(VCD_COMPARE_BUILD_DIR)/vcd-compare
SANITIZER_ENV = ASAN_OPTIONS=symbolize=1,detect_leaks=0,abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1,halt_on_error=1

WORK_REL_TO_ROOT = tests/$(MODULE_NAME)/work
CUSTOM_SIM_DIR = $(WORK_REL_TO_ROOT)/custom-sim
VERILATOR_SIM_DIR = $(WORK_REL_TO_ROOT)/verilator

CUSTOM_SIM_VCD_PATH = $(CUSTOM_SIM_DIR)/output/$(MODULE_NAME)
CUSTOM_SIM_VCD_GROUPED_FILE = $(CUSTOM_SIM_VCD_PATH).vcd
CUSTOM_SIM_VCD_RAW_FILE = $(CUSTOM_SIM_VCD_PATH)-raw.vcd
VERILATOR_SIM_VCD_FILE = $(VERILATOR_SIM_DIR)/waves.vcd

CUSTOM_SIM_VCD = custom=$(PROJECT_ROOT)/$(CUSTOM_SIM_VCD_RAW_FILE):$(MODULE_NAME)
VERILATOR_SIM_VCD = verilator=$(PROJECT_ROOT)/$(VERILATOR_SIM_VCD_FILE):TOP.tb.uut
HIERARCHY_JSON = $(CUSTOM_SIM_DIR)/debug_output/$(MODULE_NAME)/hierarchy.json

# Per-test optional args (from vcd-diff.args file)
VCD_COMPARE_ARGS = $(shell cat vcd-diff.args 2>/dev/null)

compare:
	cmake --preset sanitized -S $(PROJECT_ROOT)/tools/vcd-compare
	cmake --build $(VCD_COMPARE_BUILD_DIR) --parallel
	$(SANITIZER_ENV) $(VCD_COMPARE) --hierarchy $(PROJECT_ROOT)/$(HIERARCHY_JSON) $(VCD_COMPARE_ARGS) \
		$(CUSTOM_SIM_VCD) \
		$(VERILATOR_SIM_VCD)

waves:
	gtkwave $(PROJECT_ROOT)/$(CUSTOM_SIM_VCD_GROUPED_FILE) &>/dev/null & disown
	gtkwave $(PROJECT_ROOT)/$(VERILATOR_SIM_VCD_FILE) &>/dev/null & disown
