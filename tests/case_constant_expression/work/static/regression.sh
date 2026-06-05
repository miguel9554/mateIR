#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

module_name=case_constant_expression

make analyze EXTRA_ARGS="--dump-passes"
make -C ../verilator simulate
make -C ../custom-sim simulate SIM_BUILD_TARGET=noop

python3 - <<'PY'
import json
import re
from pathlib import Path

ROOT = Path(".")
MODULE = "case_constant_expression"
DEBUG_DIR = ROOT / "debug_output" / MODULE
SIM_OUT_DIR = ROOT / "../custom-sim/output"
STIM_DIR = ROOT / "../custom-sim/stimuli"

json_files = sorted(DEBUG_DIR.glob("[0-9][0-9]_*.json"))
if not json_files:
    raise SystemExit("no per-pass DFG JSON emitted")

graph = json.loads(json_files[-1].read_text())
nodes = {node["id"]: node for node in graph["nodes"]}
x_nodes = [node for node in nodes.values() if node["op"] == "X"]
if len(x_nodes) < 4:
    raise SystemExit(f"expected at least 4 X nodes, found {len(x_nodes)}")

by_name = {}
for node in graph["nodes"]:
    name = node.get("name")
    if name and name not in by_name:
        by_name[name] = node["id"]

def cone_contains_x(node_id, seen=None):
    if seen is None:
        seen = set()
    if node_id in seen:
        return False
    seen.add(node_id)
    node = nodes[node_id]
    if node["op"] == "X":
        return True
    for inp in node.get("inputs", []):
        inp_id = inp["node"] if isinstance(inp, dict) else inp
        if cone_contains_x(inp_id, seen):
            return True
    return False

for output_name in ("unique_overlap_o", "unique_default_o", "variable_unique_o", "partial_unique_o"):
    out_id = by_name[output_name]
    if not cone_contains_x(out_id):
        raise SystemExit(f"{output_name} cone does not contain an X node")

line_re = re.compile(r"0x([0-9a-fA-F]+)")

def parse_hex_lines(path):
    values = []
    for raw in path.read_text().splitlines():
        raw = raw.strip()
        if not raw or raw.startswith("#"):
            continue
        match = line_re.match(raw)
        if not match:
            raise SystemExit(f"unrecognized recorder line in {path}: {raw}")
        values.append(int(match.group(1), 16))
    return values

stim = {name: parse_hex_lines(STIM_DIR / f"{name}.txt") for name in ("a", "b", "c", "d", "sel")}

def parse_binary_lines(path):
    values = []
    for raw in path.read_text().splitlines():
        raw = raw.strip()
        if not raw:
            continue
        values.append(int(raw, 2))
    return values

outs = {
    name: parse_binary_lines(SIM_OUT_DIR / f"{name}.txt")
    for name in (
        "overlap_o",
        "zero_case_o",
        "grouped_unique_o",
        "unique_overlap_o",
        "unique_default_o",
        "variable_unique_o",
        "retained_o",
        "partial_unique_o",
    )
}

num_rows = len(next(iter(stim.values())))
for name, values in stim.items():
    if len(values) != num_rows:
        raise SystemExit(f"stimulus length mismatch for {name}: {len(values)} vs {num_rows}")

output_offset = 0
for name, values in outs.items():
    if len(values) == num_rows + 1:
        output_offset = 1
    elif len(values) != num_rows:
        raise SystemExit(f"output length mismatch for {name}: {len(values)} vs {num_rows}")

def expect_equal(name, actual, expected, row):
    if actual != expected:
        raise SystemExit(f"{name}[{row}] expected 0x{expected:02x}, got 0x{actual:02x}")

def expect_stable(name, rows):
    if len(rows) < 2:
        return
    vals = [outs[name][row + output_offset] for row in rows]
    if len(set(vals)) != 1:
        rendered = ", ".join(f"0x{value:02x}" for value in vals)
        raise SystemExit(f"{name} expected stable X-backed value across rows {rows}, got {rendered}")

for row in range(num_rows):
    a = stim["a"][row]
    b = stim["b"][row]
    c = stim["c"][row]
    d = stim["d"][row]
    sel = stim["sel"][row]
    out_row = row + output_offset

    overlap_expected = 0x11 if a else 0x22 if b else 0x33 if c else 0x0F
    expect_equal("overlap_o", outs["overlap_o"][out_row], overlap_expected, row)

    zero_expected = 0x41 if not a else 0x42 if not b else 0x43
    expect_equal("zero_case_o", outs["zero_case_o"][out_row], zero_expected, row)

    grouped_matches = int(bool(a or b)) + int(bool(c))
    if grouped_matches == 0:
        grouped_expected = 0x53
    elif grouped_matches == 1 and (a or b):
        grouped_expected = 0x51
    elif grouped_matches == 1:
        grouped_expected = 0x52
    else:
        grouped_expected = None
    if grouped_expected is not None:
        expect_equal("grouped_unique_o", outs["grouped_unique_o"][out_row], grouped_expected, row)

    unique_default_matches = int(bool(c)) + int(bool(d))
    if unique_default_matches == 0:
        expect_equal("unique_default_o", outs["unique_default_o"][out_row], 0x73, row)
    elif unique_default_matches == 1:
        expect_equal("unique_default_o", outs["unique_default_o"][out_row], 0x71 if c else 0x72, row)

    if sel == 0:
        expect_equal("variable_unique_o", outs["variable_unique_o"][out_row], 0x81, row)
    elif sel == 1:
        expect_equal("variable_unique_o", outs["variable_unique_o"][out_row], 0x82, row)

    retained_expected = 0x91 if c else 0x92 if d else 0x90
    expect_equal("retained_o", outs["retained_o"][out_row], retained_expected, row)

    if a and not b:
        expect_equal("partial_unique_o", outs["partial_unique_o"][out_row], 0xC5, row)
    elif b and not a:
        expect_equal("partial_unique_o", outs["partial_unique_o"][out_row], 0xA3, row)

expect_stable("unique_overlap_o", [row for row in range(num_rows) if stim["a"][row] == 0 and stim["b"][row] == 0])
expect_stable("unique_overlap_o", [row for row in range(num_rows) if stim["a"][row] == 1 and stim["b"][row] == 1])
expect_stable("grouped_unique_o", [row for row in range(num_rows) if (stim["a"][row] or stim["b"][row]) and stim["c"][row]])
expect_stable("unique_default_o", [row for row in range(num_rows) if stim["c"][row] and stim["d"][row]])
expect_stable("variable_unique_o", [row for row in range(num_rows) if stim["sel"][row] not in (0, 1)])
expect_stable("partial_unique_o", [row for row in range(num_rows) if stim["a"][row] == 0 and stim["b"][row] == 0])
expect_stable("partial_unique_o", [row for row in range(num_rows) if stim["a"][row] == 1 and stim["b"][row] == 1])
PY
