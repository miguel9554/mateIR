.PHONY: validate clean clean_all waves

ROOT_DIR = ../..

validate:
	$(MAKE) -C ../verilator simulate
	$(MAKE) -C ../custom-sim simulate
	$(MAKE) -C ../vcd-diff compare
	! rg -n "UNARY_PLUS|LOGICAL_NOT|LOGICAL_AND|LOGICAL_OR" \
		../custom-sim/debug_output -g '*.json' -g '*.dot'
	! rg -n "MODULE" ../custom-sim/debug_output -g '*.json' -g '*.dot'

waves:
	$(MAKE) -C ../vcd-diff waves

clean:
	$(MAKE) -C ../verilator clean

clean_all: clean
	$(MAKE) -C ../custom-sim clean
