#!/usr/bin/env python3
"""Run validation for all test cases and report results."""
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
SIMULATOR = REPO_ROOT / "build" / "diagnostic-relwithdebinfo" / "custom_hdl_compiler"

GREEN = "\033[32m"
RED = "\033[31m"
RESET = "\033[0m"


def find_test_cases():
    """Find all validate and expected-failure test cases."""
    cases = []
    for d in sorted(TESTS_DIR.iterdir()):
        if not d.is_dir():
            continue
        if (d / "expected_failure.txt").exists():
            cases.append((d.name, "expected_failure"))
        elif (d / "work" / "validate" / "Makefile").exists():
            cases.append((d.name, "validate"))
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


def ensure_simulator():
    """Build the diagnostic simulator used by expected-failure tests."""
    result = subprocess.run(
        ["make", "-C", str(REPO_ROOT), "diagnostic-build"],
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    return result.returncode == 0, output


def run_expected_failure(name):
    """Run an expected-failure compile test. Returns (success, output)."""
    test_dir = TESTS_DIR / name
    expected = (test_dir / "expected_failure.txt").read_text().strip()
    extra_args_path = test_dir / "custom-sim.args"
    extra_args = shlex.split(extra_args_path.read_text()) if extra_args_path.exists() else []

    rtl_sources = sorted(str(p) for p in test_dir.joinpath("rtl").glob("*.v"))
    rtl_sources += sorted(str(p) for p in test_dir.joinpath("rtl").glob("*.sv"))
    domain_files = sorted(str(p) for p in test_dir.joinpath("rtl").glob("*.domains.yaml"))

    with tempfile.TemporaryDirectory(prefix=f"{name}_stimuli_") as stimuli_dir, \
         tempfile.TemporaryDirectory(prefix=f"{name}_output_") as output_dir:
        cmd = [
            str(SIMULATOR),
            "--simulate",
            "--top",
            name,
            "--inputs-dir",
            stimuli_dir,
            "--output-dir",
            output_dir,
            "--flops-initial",
            "zeros",
        ]
        if domain_files:
            cmd.append("--domains")
            cmd.extend(domain_files)
        cmd.extend(extra_args)
        cmd.extend(rtl_sources)

        result = subprocess.run(
            cmd,
            cwd=test_dir,
            capture_output=True,
            text=True,
        )
    output = result.stdout + result.stderr
    ok = result.returncode != 0 and expected in output
    return ok, output


def main():
    cases = find_test_cases()
    if not cases:
        print("No test cases found.")
        sys.exit(1)

    has_expected_failure = any(kind == "expected_failure" for _, kind in cases)
    if has_expected_failure:
        print("Building diagnostic simulator for expected-failure tests...", flush=True)
        ok, output = ensure_simulator()
        if not ok:
            print(f"{RED}Failed to build diagnostic simulator{RESET}")
            print(output)
            sys.exit(1)

    results = {}
    for name, kind in cases:
        print(f"Running {name}...", flush=True)
        if kind == "validate":
            # run_clean(name)
            ok, output = run_validate(name)
        else:
            ok, output = run_expected_failure(name)
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
    case_names = [name for name, _ in cases]
    max_name = max(len(n) for n in case_names)
    sep = "+" + "-" * (max_name + 2) + "+" + "-" * 8 + "+"
    print()
    print(sep)
    print(f"| {'Test'.ljust(max_name)} | Result |")
    print(sep)
    for name in case_names:
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
