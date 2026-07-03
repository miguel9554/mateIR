---
name: run-tests
description: Run the repo's canonical build and test workflows. Use this when validating compiler changes, running one test, running regression, or understanding expected-failure tests. Prefer the existing Makefile and tests/regression.py flows.
---

Canonical builds:
- `make dev`
- `make sanitized`
- `make debug`
- `python tests/regression.py`

Important:
- regression expects `build/sanitized/mate`
- debug builds require Debug slang
- prefer wrapping build, test, and compiler commands with `scripts/docker-run.sh` unless the current environment is already known-good

Per-test workflows:
- PASS test: `make -C tests/<name>/work/validate validate`
- custom sim only: `make -C tests/<name>/work/custom-sim simulate`
- static only: `make -C tests/<name>/work/static analyze`

Regression behavior:
- runs `tests/check_dfg_api_surface.py`
- runs `tests/check_module_node_api_surface.py`
- then runs tests from `tests/regression_tests.txt`

Test layout:
- `rtl/`
- `tb/`
- `work/custom-sim/stimuli`
- `work/custom-sim/output`
- `work/static`
- `work/validate`
- `work/verilator`

Do not invent alternate test entry points unless the repo lacks one.
