.PHONY: validate clean waves

ROOT_DIR = ../..

validate:
	$(MAKE) -C ../verilator simulate
	$(MAKE) -C ../custom-sim simulate
	$(MAKE) -C ../vcd-diff compare

waves:
	$(MAKE) -C ../vcd-diff waves

clean:
	$(MAKE) -C ../verilator clean
	$(MAKE) -C ../custom-sim clean
