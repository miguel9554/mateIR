.PHONY: validate clean clean_all waves

ROOT_DIR = ../..

validate:
	$(MAKE) -C ../verilator clean
	$(MAKE) -C ../verilator simulate
	$(MAKE) -C ../custom-sim simulate
	$(MAKE) -C ../vcd-diff compare

waves:
	$(MAKE) -C ../vcd-diff waves

clean:
	$(MAKE) -C ../verilator clean

clean_all: clean
	$(MAKE) -C ../custom-sim clean
