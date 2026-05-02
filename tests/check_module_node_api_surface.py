#!/usr/bin/env python3
"""Guardrails for resolved-module readers during module-node migration."""

from pathlib import Path
import re
import sys

TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent

GUARDED_FILES = [
    REPO_ROOT / "src/mateir/module.h",
    REPO_ROOT / "src/mateir/module.cpp",
    REPO_ROOT / "src/frontends/systemverilog/passes/elaboration.cpp",
    REPO_ROOT / "src/frontends/systemverilog/passes/dfg_inline.cpp",
    REPO_ROOT / "src/frontends/systemverilog/passes/flop_resolve.cpp",
    REPO_ROOT / "src/frontends/systemverilog/passes/io_domains_set.cpp",
    REPO_ROOT / "src/frontends/systemverilog/passes/sync_domain_propagate.cpp",
    REPO_ROOT / "src/frontends/systemverilog/domain_facts.cpp",
    REPO_ROOT / "src/sim/simulator.cpp",
    REPO_ROOT / "src/sim/vcd_writer.cpp",
    REPO_ROOT / "src/consumers/static_analysis/static_analysis.cpp",
]

BANNED = [
    r"\.inputs\b",
    r"\.outputs\b",
    r"\.signals\b",
    r"->inputs\b",
    r"->outputs\b",
    r"->signals\b",
]

EXEMPT_LINE_PATTERNS = [
    r"\bbinding\.inputs\b",        # ModuleInstanceBinding::inputs
    r"\bbinding->inputs\b",        # ModuleInstanceBinding::inputs
    r"\bunresolved\.(inputs|outputs|signals)\b",
    r"\bunresolved->(inputs|outputs|signals)\b",
    r"resolved\.signals with qualified name",  # existing comment
    r"\bsummary\.(inputs|outputs|signals)\b",
    r"\bsummary->(inputs|outputs|signals)\b",
    r"\bselection\.(inputs|outputs|signals)\b",
    r"\bselection->(inputs|outputs|signals)\b",
    r"\.inputs\s*=\s*\{\s*\}",     # designated init for ModuleInstanceBinding
]


def main() -> int:
    errors = []
    banned_res = [re.compile(p) for p in BANNED]
    exempt_res = [re.compile(p) for p in EXEMPT_LINE_PATTERNS]

    for path in GUARDED_FILES:
        text = path.read_text(encoding="utf-8")
        for lineno, line in enumerate(text.splitlines(), start=1):
            if any(rx.search(line) for rx in exempt_res):
                continue
            for rx in banned_res:
                if rx.search(line):
                    errors.append(f"{path}:{lineno}: banned split-map usage: {line.strip()}")
                    break

    if errors:
        print("Module-node API surface guard failed:")
        for err in errors:
            print(f"  {err}")
        return 1

    print("Module-node API surface guard passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
