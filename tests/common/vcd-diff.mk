ROOT_DIR = ../..
PROJECT_ROOT = $(ROOT_DIR)/../..
MODULE_NAME = $(notdir $(realpath $(ROOT_DIR)))

WORK_REL_TO_ROOT = tests/$(MODULE_NAME)/work
CUSTOM_SIM_DIR = $(WORK_REL_TO_ROOT)/custom-sim
VERILATOR_SIM_DIR = $(WORK_REL_TO_ROOT)/verilator

CUSTOM_SIM_VCD = $(CUSTOM_SIM_DIR)/output/$(MODULE_NAME)-flatten.vcd:$(MODULE_NAME)
VERILATOR_SIM_VCD = $(VERILATOR_SIM_DIR)/waves.vcd:tb.uut

compare:
	make -C $(PROJECT_ROOT) vcd-diff vcd1=$(CUSTOM_SIM_VCD) vcd2=$(VERILATOR_SIM_VCD)
