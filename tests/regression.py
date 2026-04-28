#!/usr/bin/env python3
"""Run regression tests defined in a plain-text manifest."""
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
SIMULATOR = REPO_ROOT / "build" / "sanitized" / "mate"
MANIFEST = TESTS_DIR / "regression_tests.txt"

GREEN = "\033[32m"
RED = "\033[31m"
RESET = "\033[0m"


def load_test_cases():
    """Load test cases from the manifest.

    Manifest format:
      - blank lines and lines starting with '#' are ignored
      - PASS entries: <test-name> PASS
      - FAIL entries: <test-name> FAIL <expected-substring>
    """
    if not MANIFEST.exists():
        raise RuntimeError(f"Manifest not found: {MANIFEST}")

    cases = []
    seen = set()

    for lineno, raw_line in enumerate(MANIFEST.read_text().splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        parts = line.split(maxsplit=2)
        if len(parts) < 2:
            raise RuntimeError(
                f"{MANIFEST}:{lineno}: expected '<test-name> PASS' or "
                f"'<test-name> FAIL <expected-substring>'"
            )

        name, expected = parts[0], parts[1]
        test_dir = TESTS_DIR / name

        if name in seen:
            raise RuntimeError(f"{MANIFEST}:{lineno}: duplicate test entry for '{name}'")
        if not test_dir.is_dir():
            raise RuntimeError(f"{MANIFEST}:{lineno}: unknown test directory '{name}'")
        if expected not in {"PASS", "FAIL"}:
            raise RuntimeError(
                f"{MANIFEST}:{lineno}: invalid expected result '{expected}', expected PASS or FAIL"
            )

        if expected == "PASS":
            if len(parts) != 2:
                raise RuntimeError(f"{MANIFEST}:{lineno}: PASS entries cannot include extra fields")
            if not (test_dir / "work" / "validate" / "Makefile").exists():
                raise RuntimeError(
                    f"{MANIFEST}:{lineno}: PASS test '{name}' is missing work/validate/Makefile"
                )
            cases.append({"name": name, "kind": "validate"})
        else:
            if len(parts) != 3 or not parts[2].strip():
                raise RuntimeError(
                    f"{MANIFEST}:{lineno}: FAIL entries require an expected error substring"
                )
            cases.append(
                {
                    "name": name,
                    "kind": "expected_failure",
                    "expected_error": parts[2].strip(),
                }
            )

        seen.add(name)

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
    """Build the sanitized simulator used by expected-failure tests."""
    result = subprocess.run(
        ["make", "-C", str(REPO_ROOT), "sanitized"],
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    return result.returncode == 0, output


def run_expected_failure(name, expected):
    """Run an expected-failure compile test. Returns (success, output)."""
    test_dir = TESTS_DIR / name
    extra_args_path = test_dir / "custom-sim.args"
    extra_args = shlex.split(extra_args_path.read_text()) if extra_args_path.exists() else []

    rtl_sources = sorted(str(p) for p in test_dir.joinpath("rtl").glob("*.v"))
    rtl_sources += sorted(str(p) for p in test_dir.joinpath("rtl").glob("*.sv"))
    domain_file = test_dir / "rtl" / f"{name}.domains.yaml"

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
        if domain_file.exists():
            cmd.append("--domains")
            cmd.append(str(domain_file))
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
    try:
        cases = load_test_cases()
    except RuntimeError as exc:
        print(f"{RED}{exc}{RESET}")
        sys.exit(1)

    if not cases:
        print("No test cases found.")
        sys.exit(1)

    has_expected_failure = any(case["kind"] == "expected_failure" for case in cases)
    if has_expected_failure:
        print("Building diagnostic simulator for expected-failure tests...", flush=True)
        ok, output = ensure_simulator()
        if not ok:
            print(f"{RED}Failed to build diagnostic simulator{RESET}")
            print(output)
            sys.exit(1)

    results = {}
    for case in cases:
        name = case["name"]
        kind = case["kind"]
        print(f"Running {name}...", flush=True)
        if kind == "validate":
            # run_clean(name)
            ok, output = run_validate(name)
        else:
            ok, output = run_expected_failure(name, case["expected_error"])
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
    case_names = [case["name"] for case in cases]
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
