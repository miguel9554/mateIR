#!/usr/bin/env python3
"""
Flatten Mate grouped VCD category scopes.

This preserves real module / generate hierarchy and removes only the synthetic
Mate grouping scopes used for readability: params, inputs, signals, flops, and
outputs. Value identifiers and value-change sections are copied unchanged.
"""

from __future__ import annotations

import argparse
import os
import re
import sys


FAKE_SCOPES = {"params", "inputs", "signals", "flops", "outputs"}
SCOPE_RE = re.compile(r"^(\s*)\$scope\s+(\S+)\s+(\S+)\s+\$end(\s*)$")
UPSCOPE_RE = re.compile(r"^\s*\$upscope\s+\$end\s*$")
ENDDEFINITIONS_RE = re.compile(r"^\s*\$enddefinitions\s+\$end\s*$")


def flatten_vcd(input_path: str, output_path: str) -> None:
    scope_stack = []
    in_header = True

    tmp_path = f"{output_path}.tmp"
    try:
        with open(input_path, "r", encoding="utf-8") as src, \
                open(tmp_path, "w", encoding="utf-8") as dst:
            for line in src:
                if in_header:
                    scope_match = SCOPE_RE.match(line)
                    if scope_match:
                        is_fake = scope_match.group(3) in FAKE_SCOPES
                        scope_stack.append(is_fake)
                        if not is_fake:
                            dst.write(line)
                        continue

                    if UPSCOPE_RE.match(line):
                        if not scope_stack:
                            raise RuntimeError("VCD has $upscope without matching $scope")
                        was_fake = scope_stack.pop()
                        if not was_fake:
                            dst.write(line)
                        continue

                    dst.write(line)
                    if ENDDEFINITIONS_RE.match(line):
                        if scope_stack:
                            raise RuntimeError("VCD header ended with unclosed $scope entries")
                        in_header = False
                    continue

                dst.write(line)

        os.replace(tmp_path, output_path)
    except Exception:
        try:
            os.unlink(tmp_path)
        except FileNotFoundError:
            pass
        raise


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Remove Mate grouped VCD category scopes while preserving real hierarchy.")
    parser.add_argument("input", help="grouped input VCD")
    parser.add_argument("output", help="flattened output VCD")
    args = parser.parse_args(argv)

    flatten_vcd(args.input, args.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except Exception as exc:
        print(f"vcd_flatten.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
