.PHONY: validate clean clean_all

ROOT := ../../../..
TEST := module_node_role_collision_fail
SIM_BUILD_TARGET ?= dev
RTL := ../../rtl/$(TEST).v
DOMAINS := ../../rtl/$(TEST).domains.yaml
EXPECTED := duplicate named-value node 'dup' across roles 'input' and 'output'

validate:
	@set -e; \
	out="$$(mktemp)"; \
	$(MAKE) -C $(ROOT) $(SIM_BUILD_TARGET); \
	if $(ROOT)/build/$(SIM_BUILD_TARGET)/mate --top $(TEST) --domains $(DOMAINS) $(RTL) >"$$out" 2>&1; then \
		cat "$$out"; \
		echo "Expected compilation failure, but command succeeded"; \
		rm -f "$$out"; \
		exit 1; \
	fi; \
	cat "$$out"; \
	if ! grep -Fq "$(EXPECTED)" "$$out"; then \
		echo "Missing expected error substring: $(EXPECTED)"; \
		rm -f "$$out"; \
		exit 1; \
	fi; \
	rm -f "$$out"

clean:
	rm -rf ../custom-sim/output ../custom-sim/debug_output ../verilator/obj_dir

clean_all: clean
