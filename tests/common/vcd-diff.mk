ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
MODULE_NAME = $(notdir $(realpath $(ROOT_DIR)))

WORK_REL_TO_ROOT = tests/$(MODULE_NAME)/work
CUSTOM_SIM_DIR = $(WORK_REL_TO_ROOT)/custom-sim
VERILATOR_SIM_DIR = $(WORK_REL_TO_ROOT)/verilator

CUSTOM_SIM_VCD_FILE = $(CUSTOM_SIM_DIR)/output/$(MODULE_NAME)-flatten.vcd
VERILATOR_SIM_VCD_FILE = $(VERILATOR_SIM_DIR)/waves.vcd

CUSTOM_SIM_VCD = $(CUSTOM_SIM_VCD_FILE):$(MODULE_NAME)
VERILATOR_SIM_VCD = $(VERILATOR_SIM_VCD_FILE):tb.uut

compare:
	make -C $(PROJECT_ROOT) vcd-diff vcd1=$(CUSTOM_SIM_VCD) vcd2=$(VERILATOR_SIM_VCD)

waves:
	gtkwave $(PROJECT_ROOT)/$(CUSTOM_SIM_VCD_FILE) &>/dev/null & disown
	gtkwave $(PROJECT_ROOT)/$(VERILATOR_SIM_VCD_FILE) &>/dev/null & disown
