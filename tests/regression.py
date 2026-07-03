#!/usr/bin/env python3
"""Run regression tests defined in a plain-text manifest."""
import argparse
import os
import shlex
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
MANIFEST = TESTS_DIR / "regression_tests.txt"
DFG_API_GUARD = TESTS_DIR / "check_dfg_api_surface.py"
MODULE_NODE_API_GUARD = TESTS_DIR / "check_module_node_api_surface.py"
FIXED_VALUE_DIFF_TEST = "mate-fixed-value-diff-test"

GREEN = "\033[32m"
RED = "\033[31m"
RESET = "\033[0m"

RUN_MODE_VALIDATE = "validate"
RUN_MODE_VERILATOR_DPI = "verilator-dpi"


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


def filter_cases_by_name(cases, names):
    """Filter cases to the requested test names."""
    if not names:
        return cases

    requested = set(names)
    selected = [case for case in cases if case["name"] in requested]
    found = {case["name"] for case in selected}
    missing = sorted(requested - found)
    if missing:
        raise RuntimeError(f"unknown test name(s): {', '.join(missing)}")
    return selected


def select_cases_for_mode(cases, run_mode):
    """Return the cases that can be run by the requested mode."""
    if run_mode == RUN_MODE_VALIDATE:
        return cases
    if run_mode != RUN_MODE_VERILATOR_DPI:
        raise RuntimeError(f"unknown run mode '{run_mode}'")
    return [
        case
        for case in cases
        if case["kind"] != "validate"
        or (TESTS_DIR / case["name"] / "work" / "verilator").is_dir()
    ]


def run_clean(name, run_mode):
    """Run make clean for a test case in the work dir used by run_mode."""
    if run_mode == RUN_MODE_VERILATOR_DPI:
        work_dir = TESTS_DIR / name / "work" / "verilator"
        # DPI=1 selects the clean recipe that also removes the generated DPI dir.
        clean_cmd = ["make", "clean", "DPI=1"]
    else:
        work_dir = TESTS_DIR / name / "work" / "validate"
        clean_cmd = ["make", "clean"]
    subprocess.run(
        clean_cmd,
        cwd=work_dir,
        capture_output=True,
        text=True,
    )


def run_validate(name, build_target, run_mode):
    """Run a PASS test's specialized script or default validation recipe."""
    if run_mode == RUN_MODE_VERILATOR_DPI:
        work_dir = TESTS_DIR / name / "work" / "verilator"
        result = subprocess.run(
            ["make", "simulate", "DPI=1", "DPI_BUILD_TARGET=noop"],
            cwd=work_dir,
            capture_output=True,
            text=True,
        )
        output = result.stdout + result.stderr
        # The Verilator DPI harness reports a value mismatch via $fatal(1) in a
        # `final` block, which does NOT set a non-zero process exit code, so the
        # `make` return code only reflects build/elaboration failures. Judge the
        # actual DPI-vs-RTL comparison by the checker's sentinel line instead.
        if result.returncode != 0:
            return False, output
        if "DPI and RTL mismatched" in output:
            return False, output
        if "PASS: 100% match" not in output:
            return False, output + (
                "\n[regression] DPI checker PASS sentinel not found; "
                "treating as failure.\n"
            )
        return True, output

    static_work_dir = TESTS_DIR / name / "work" / "static"
    regression_script = static_work_dir / "regression.sh"
    if regression_script.exists():
        env = os.environ.copy()
        # The simulator is built once upfront; avoid racing cmake in parallel tests.
        env["STATIC_BUILD_TARGET"] = "noop"
        env.setdefault("VCD_COMPARE_BUILD_TARGET", build_target)
        result = subprocess.run(
            ["bash", str(regression_script)],
            cwd=static_work_dir,
            capture_output=True,
            text=True,
            env=env,
        )
        output = result.stdout + result.stderr
        return result.returncode == 0, output

    work_dir = TESTS_DIR / name / "work" / "validate"
    result = subprocess.run(
        # SIM_BUILD_TARGET=noop prevents each per-test sub-make from re-invoking
        # cmake on the shared build directory, which would race when tests run in
        # parallel. The simulator is built once upfront before this point.
        ["make", "validate", "SIM_BUILD_TARGET=noop"],
        cwd=work_dir,
        capture_output=True,
        text=True,
        env={**os.environ, "VCD_COMPARE_BUILD_TARGET": build_target},
    )
    output = result.stdout + result.stderr
    return result.returncode == 0, output


def ensure_simulator(build_target):
    """Build the requested simulator before running tests."""
    result = subprocess.run(
        ["make", "-C", str(REPO_ROOT), build_target],
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    return result.returncode == 0, output


def run_fixed_value_diff_test(build_target):
    """Run the FixedValue-vs-SimValue differential test binary."""
    test_binary = REPO_ROOT / "build" / build_target / FIXED_VALUE_DIFF_TEST
    result = subprocess.run(
        [str(test_binary)],
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    return result.returncode == 0, output


def run_dfg_api_guard():
    """Run source-level API guard checks before build/test."""
    result = subprocess.run(
        [sys.executable, str(DFG_API_GUARD)],
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    return result.returncode == 0, output

def run_module_node_api_guard():
    """Run module-node source-level API guard checks before build/test."""
    result = subprocess.run(
        [sys.executable, str(MODULE_NODE_API_GUARD)],
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    return result.returncode == 0, output


def run_expected_failure(name, expected, simulator):
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
            str(simulator),
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


def run_case(case, build_target, simulator, run_mode):
    """Run a single test case. Returns (name, ok, output)."""
    name = case["name"]
    if case["kind"] == "validate":
        if run_mode not in {RUN_MODE_VALIDATE, RUN_MODE_VERILATOR_DPI}:
            raise RuntimeError(f"unknown run mode '{run_mode}'")
        run_clean(name, run_mode)
        ok, output = run_validate(name, build_target, run_mode)
    else:
        ok, output = run_expected_failure(name, case["expected_error"], simulator)
    return name, ok, output


def main():
    parser = argparse.ArgumentParser(description="Run regression tests.")
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=os.cpu_count() or 1,
        help="Number of tests to run in parallel (default: number of CPUs)",
    )
    parser.add_argument(
        "--build",
        choices=("dev", "sanitized"),
        default="dev",
        help="Build preset to use for mate and helper tools (default: dev)",
    )
    parser.add_argument(
        "--mode",
        choices=(RUN_MODE_VALIDATE, RUN_MODE_VERILATOR_DPI),
        default=RUN_MODE_VALIDATE,
        help=(
            "Per-PASS-test command mode. 'validate' runs the validate target; "
            "'verilator-dpi' runs make simulate DPI=1 in "
            "tests/<test>/work/verilator/ (default: validate)"
        ),
    )
    parser.add_argument(
        "--test",
        action="append",
        dest="tests",
        help="Run only the named manifest test. May be passed more than once.",
    )
    args = parser.parse_args()
    simulator = REPO_ROOT / "build" / args.build / "mate"

    try:
        cases = load_test_cases()
    except RuntimeError as exc:
        print(f"{RED}{exc}{RESET}")
        sys.exit(1)
    try:
        cases = filter_cases_by_name(cases, args.tests)
        cases = select_cases_for_mode(cases, args.mode)
    except RuntimeError as exc:
        print(f"{RED}{exc}{RESET}")
        sys.exit(1)

    if not cases:
        print("No test cases found.")
        sys.exit(1)

    print("Checking DFG API guardrails...", flush=True)
    ok, output = run_dfg_api_guard()
    if not ok:
        print(f"{RED}DFG API guard failed{RESET}")
        print(output)
        sys.exit(1)

    print("Checking module-node API guardrails...", flush=True)
    ok, output = run_module_node_api_guard()
    if not ok:
        print(f"{RED}Module-node API guard failed{RESET}")
        print(output)
        sys.exit(1)

    print("Building simulator...", flush=True)
    ok, output = ensure_simulator(args.build)
    if not ok:
        print(f"{RED}Failed to build simulator{RESET}")
        print(output)
        sys.exit(1)

    print("Running FixedValue differential test...", flush=True)
    ok, output = run_fixed_value_diff_test(args.build)
    if not ok:
        print(f"{RED}FixedValue differential test failed{RESET}")
        print(output)
        sys.exit(1)

    jobs = max(1, args.jobs)
    print(f"Running {len(cases)} tests with {jobs} worker(s)...", flush=True)

    results = {}
    completed = 0
    total = len(cases)
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {
            executor.submit(run_case, case, args.build, simulator, args.mode): case
            for case in cases
        }
        for future in as_completed(futures):
            name, ok, output = future.result()
            results[name] = (ok, output)
            completed += 1
            status = f"{GREEN}PASS{RESET}" if ok else f"{RED}FAIL{RESET}"
            print(f"  [{completed}/{total}] {status}  {name}", flush=True)

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
