.PHONY: validate clean

ROOT_DIR = ../..

validate:
	$(MAKE) -C ../verilator simulate
	$(MAKE) -C ../custom-sim simulate
	$(MAKE) -C ../vcd-diff compare

clean:
	$(MAKE) -C ../verilator clean
	$(MAKE) -C ../custom-sim clean
