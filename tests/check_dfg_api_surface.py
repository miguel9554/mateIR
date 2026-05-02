#!/usr/bin/env python3
"""Guardrails for DFG public API surface and usage patterns."""

from pathlib import Path
import re
import sys

TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
SRC_DIR = REPO_ROOT / "src"
DFG_HEADER = SRC_DIR / "mateir" / "dfg.h"

BANNED_IN_SRC = [
    r"\bgetInputsMap\s*\(",
    r"\bgetOutputsMap\s*\(",
    r"\bgetSignalsMap\s*\(",
    r"\bgetSignalNode\s*\(",
    r"\bhasSignal\s*\(",
    r"\bconnectSignal\s*\(",
]

BANNED_DFG_API_DECLS = [
    r"\bDFGNode\*\s+input\s*\(",
    r"\bDFGNode\*\s+outputPlaceholder\s*\(",
]

BANNED_CALLS_IN_SRC = [
    r"\b(?:ctx\.)?graph\.input\s*\(",
    r"\b(?:ctx\.)?graph\.outputPlaceholder\s*\(",
    r"\b->input\s*\(",
    r"\b->outputPlaceholder\s*\(",
    r"\b\.input\s*\(",
    r"\b\.outputPlaceholder\s*\(",
]


def iter_source_files(root: Path):
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix not in {".h", ".hpp", ".cpp", ".cc", ".cxx"}:
            continue
        yield path


def find_matches(path: Path, pattern: str):
    regex = re.compile(pattern)
    text = path.read_text(encoding="utf-8")
    hits = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if regex.search(line):
            hits.append((lineno, line.strip()))
    return hits


def main() -> int:
    errors = []

    for src_file in iter_source_files(SRC_DIR):
        for pattern in BANNED_IN_SRC:
            for lineno, line in find_matches(src_file, pattern):
                errors.append(f"{src_file}:{lineno}: banned symbol in src: {line}")

    for pattern in BANNED_CALLS_IN_SRC:
        for src_file in iter_source_files(SRC_DIR):
            for lineno, line in find_matches(src_file, pattern):
                errors.append(f"{src_file}:{lineno}: banned wrapper call: {line}")

    for pattern in BANNED_DFG_API_DECLS:
        for lineno, line in find_matches(DFG_HEADER, pattern):
            errors.append(f"{DFG_HEADER}:{lineno}: banned DFG API declaration: {line}")

    if errors:
        print("DFG API surface guard failed:")
        for err in errors:
            print(f"  {err}")
        return 1

    print("DFG API surface guard passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
