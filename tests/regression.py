#!/usr/bin/env python3
"""Run validation for all test cases and report results."""
import subprocess
import sys
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent

GREEN = "\033[32m"
RED = "\033[31m"
RESET = "\033[0m"


def find_test_cases():
    """Find all test directories that have a work/validate/Makefile."""
    cases = []
    for d in sorted(TESTS_DIR.iterdir()):
        if d.is_dir() and (d / "work" / "validate" / "Makefile").exists():
            cases.append(d.name)
    return cases


def run_clean(name):
    """Run make clean for a test case. Returns nothing."""
    work_dir = TESTS_DIR / name / "work" / "validate"
    subprocess.run(
        ["make", "clean"],
        cwd=work_dir,
        capture_output=True,
        text=True,
    )


def run_validate(name):
    """Run make validate for a test case. Returns (success, output)."""
    work_dir = TESTS_DIR / name / "work" / "validate"
    result = subprocess.run(
        ["make", "validate"],
        cwd=work_dir,
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    return result.returncode == 0, output


def main():
    cases = find_test_cases()
    if not cases:
        print("No test cases with work/validate found.")
        sys.exit(1)

    results = {}
    for name in cases:
        print(f"Running {name}...", flush=True)
        # run_clean(name)
        ok, output = run_validate(name)
        results[name] = (ok, output)
        if ok:
            print(f"  {GREEN}PASS{RESET}")
        else:
            print(f"  {RED}FAIL{RESET}")

    # Print failure details first
    failures = [(n, out) for n, (ok, out) in results.items() if not ok]
    if failures:
        print("\n--- Failure details ---")
        for name, output in failures:
            print(f"\n=== {name} ===")
            lines = output.strip().splitlines()
            for line in lines[-30:]:
                print(f"  {line}")

    # Print summary table last
    max_name = max(len(n) for n in cases)
    sep = "+" + "-" * (max_name + 2) + "+" + "-" * 8 + "+"
    print()
    print(sep)
    print(f"| {'Test'.ljust(max_name)} | Result |")
    print(sep)
    for name in cases:
        ok = results[name][0]
        if ok:
            status = f" {GREEN}PASS{RESET} "
        else:
            status = f" {RED}FAIL{RESET} "
        print(f"| {name.ljust(max_name)} |{status}|")
    print(sep)

    passed = sum(1 for ok, _ in results.values() if ok)
    total = len(results)
    print(f"\n{passed}/{total} passed")

    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
