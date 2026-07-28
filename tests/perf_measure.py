#!/usr/bin/env python3
"""Sweep TB-only simulation speed (RTL vs DPI, with/without probing) across modules.

For each module named on the command line, runs `make simulate` in
tests/<module>/work/verilator four ways:
  - TB_VARIANT=rtl-only, WAVES_ENABLE=0   (RTL,  no probing)
  - TB_VARIANT=rtl-only, WAVES_ENABLE=1   (RTL,  probing)
  - TB_VARIANT=dpi-only, WAVES_ENABLE=0   (DPI,  no probing)
  - TB_VARIANT=dpi-only, WAVES_ENABLE=1   (DPI,  probing)

Two independent speed measures are parsed from each run's output:
  - "verilator": Verilator's own end-of-run report line, e.g.
        - Verilator: $finish at 2us; walltime 0.000 s; speed 11.454 ms/s
  - "custom": tb_common.sv's own [SIM SPEED] report, e.g.
        [SIM SPEED] sim_time=5.000us  wall_time=0.004s  speed=1237.331 us/s
--metric selects which to print (default: both, as two rows per module).

For each (module, cross) pair the first of --runs repetitions does
`make clean && make simulate ...` to force a real rebuild matching that
cross's flags (TB_VARIANT/WAVES_ENABLE only take effect on a rebuild, since
the generated tb.sv is otherwise regenerated on every `make simulate` and
forces one anyway). Subsequent repetitions skip make entirely and re-exec
the already-built ./obj_dir/Vtb binary directly, which is much faster and
still exercises the exact binary produced for that cross.

Results are saved as JSON and printed as a table comparing DPI (our
generated model) against RTL (plain Verilator) in both probing modes.
"""
import argparse
import json
import re
import statistics
import subprocess
import sys
from datetime import datetime
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
RESULTS_ROOT = REPO_ROOT / "test-results" / "perf"

# (label, DPI, TB_VARIANT, WAVES_ENABLE)
CROSSES = [
    ("rtl_no_probe", 0, "rtl-only", 0),
    ("rtl_probe", 0, "rtl-only", 1),
    ("dpi_no_probe", 1, "dpi-only", 0),
    ("dpi_probe", 1, "dpi-only", 1),
]

# Verilator auto-scales the reported speed's time unit; normalize everything
# to simulated-microseconds-per-wall-second so runs are comparable.
UNIT_TO_NS = {"s": 1e9, "ms": 1e6, "us": 1e3, "ns": 1.0, "ps": 1e-3}

FINISH_RE = re.compile(
    r"Verilator:\s*\$finish at .*?;\s*walltime\s+([\d.]+)\s*s;\s*speed\s+([\d.]+)\s*(\w+)/s"
)

# tb_common.sv's own report (tb_pkg.sv:sim_speed_report), always in us/s.
SIM_SPEED_RE = re.compile(
    r"\[SIM SPEED\]\s*sim_time=([\d.]+)us\s+wall_time=([\d.]+)s\s+speed=([\d.]+)\s*us/s"
)

METRICS = ("verilator", "custom")


def parse_verilator_speed(output: str):
    match = FINISH_RE.search(output)
    if not match:
        return None
    walltime_s, speed_val, unit = match.groups()
    if unit not in UNIT_TO_NS:
        raise RuntimeError(f"Unrecognized Verilator speed unit {unit!r} in: {match.group(0)!r}")
    speed_us_per_s = float(speed_val) * UNIT_TO_NS[unit] / 1e3
    return float(walltime_s), speed_us_per_s


def parse_custom_speed(output: str):
    match = SIM_SPEED_RE.search(output)
    if not match:
        return None
    _sim_time_us, walltime_s, speed_us_per_s = match.groups()
    return float(walltime_s), float(speed_us_per_s)


def run_shell(inner_cmd: str, use_docker: bool, timeout: int):
    cmd = ["scripts/docker-run.sh", "bash", "-c", inner_cmd] if use_docker else ["bash", "-c", inner_cmd]
    proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True, timeout=timeout)
    return proc.stdout + proc.stderr, proc.returncode


def extract_sample(output: str, rc: int, context: str):
    verilator = parse_verilator_speed(output)
    if verilator is None:
        raise RuntimeError(
            f"Could not find Verilator speed report for {context} "
            f"(exit={rc}); tail of output:\n" + "\n".join(output.splitlines()[-30:])
        )
    custom = parse_custom_speed(output)
    if custom is None:
        raise RuntimeError(
            f"Could not find [SIM SPEED] report for {context} "
            f"(exit={rc}); tail of output:\n" + "\n".join(output.splitlines()[-30:])
        )
    verilator_walltime_s, verilator_speed_us_per_s = verilator
    custom_walltime_s, custom_speed_us_per_s = custom
    return {
        "verilator": {"walltime_s": verilator_walltime_s, "speed_us_per_s": verilator_speed_us_per_s},
        "custom": {"walltime_s": custom_walltime_s, "speed_us_per_s": custom_speed_us_per_s},
    }


def run_build_and_measure(module: str, dpi: int, tb_variant: str, waves_enable: int, use_docker: bool, timeout: int):
    """Full `make clean && make simulate` — the only way to guarantee the
    binary actually reflects this cross's TB_VARIANT/WAVES_ENABLE flags."""
    work_dir = REPO_ROOT / "tests" / module / "work" / "verilator"
    if not work_dir.exists():
        raise RuntimeError(f"No work/verilator dir for module {module!r} (expected {work_dir})")

    inner_cmd = (
        f"make -C {work_dir} clean && "
        f"make -C {work_dir} simulate DPI={dpi} TB_VARIANT={tb_variant} WAVES_ENABLE={waves_enable}"
    )
    output, rc = run_shell(inner_cmd, use_docker, timeout)
    context = f"module={module} DPI={dpi} TB_VARIANT={tb_variant} WAVES_ENABLE={waves_enable} (build)"
    return extract_sample(output, rc, context)


def run_repeat(module: str, waves_enable: int, use_docker: bool, timeout: int):
    """Re-execute the binary already built by run_build_and_measure for this
    cross, skipping make (and its forced tb.sv/obj_dir rebuild) entirely."""
    work_dir = REPO_ROOT / "tests" / module / "work" / "verilator"
    waves_arg = "+WAVES=waves.vcd" if waves_enable else ""
    inner_cmd = f"cd {work_dir} && ./obj_dir/Vtb {waves_arg}"
    output, rc = run_shell(inner_cmd, use_docker, timeout)
    context = f"module={module} WAVES_ENABLE={waves_enable} (repeat)"
    return extract_sample(output, rc, context)


def sweep(modules, runs: int, use_docker: bool, timeout: int):
    results = {}
    errors = []
    for module in modules:
        print(f"== {module} ==")
        results[module] = {}
        for label, dpi, tb_variant, waves_enable in CROSSES:
            samples = []
            for i in range(runs):
                tag = f"{module}/{label}" + (f" run {i + 1}/{runs}" if runs > 1 else "")
                print(f"  {tag} ...", end=" ", flush=True)
                try:
                    if i == 0:
                        sample = run_build_and_measure(module, dpi, tb_variant, waves_enable, use_docker, timeout)
                    else:
                        sample = run_repeat(module, waves_enable, use_docker, timeout)
                except Exception as exc:
                    print("ERROR")
                    print(f"    {exc}", file=sys.stderr)
                    errors.append({"module": module, "cross": label, "run": i + 1, "error": str(exc)})
                    continue
                samples.append(sample)
                print(f"verilator={sample['verilator']['speed_us_per_s']:.3f} us/s  "
                      f"custom={sample['custom']['speed_us_per_s']:.3f} us/s")

            cross_result = {"samples": samples}
            for metric in METRICS:
                speeds = [s[metric]["speed_us_per_s"] for s in samples]
                walltimes = [s[metric]["walltime_s"] for s in samples]
                cross_result[metric] = {
                    "mean_speed_us_per_s": statistics.mean(speeds) if speeds else None,
                    "stdev_speed_us_per_s": statistics.stdev(speeds) if len(speeds) > 1 else 0.0,
                    "mean_walltime_s": statistics.mean(walltimes) if walltimes else None,
                }
            results[module][label] = cross_result
    return results, errors


def speed_factor(rtl_mean, dpi_mean):
    if rtl_mean is None or dpi_mean is None or dpi_mean == 0:
        return None
    return rtl_mean / dpi_mean


def fmt_speed_ms(mean_us, stdev_us):
    if mean_us is None:
        return "n/a"
    return f"{mean_us / 1e3:.3f} ± {stdev_us / 1e3:.3f}"


def fmt_factor(v):
    return f"{v:.3f}x" if v is not None else "n/a"


def print_table(results, modules, metrics):
    group_headers = ["", "", "no probe", "probe"]
    sub_headers = ["module", "metric", "dpi", "rtl", "factor", "dpi", "rtl", "factor"]
    rows = []
    for module in modules:
        m = results.get(module, {})
        for metric in metrics:
            rtl_np = m.get("rtl_no_probe", {}).get(metric, {})
            dpi_np = m.get("dpi_no_probe", {}).get(metric, {})
            rtl_p = m.get("rtl_probe", {}).get(metric, {})
            dpi_p = m.get("dpi_probe", {}).get(metric, {})
            rows.append([
                module, metric,
                fmt_speed_ms(dpi_np.get("mean_speed_us_per_s"), dpi_np.get("stdev_speed_us_per_s")),
                fmt_speed_ms(rtl_np.get("mean_speed_us_per_s"), rtl_np.get("stdev_speed_us_per_s")),
                fmt_factor(speed_factor(rtl_np.get("mean_speed_us_per_s"), dpi_np.get("mean_speed_us_per_s"))),
                fmt_speed_ms(dpi_p.get("mean_speed_us_per_s"), dpi_p.get("stdev_speed_us_per_s")),
                fmt_speed_ms(rtl_p.get("mean_speed_us_per_s"), rtl_p.get("stdev_speed_us_per_s")),
                fmt_factor(speed_factor(rtl_p.get("mean_speed_us_per_s"), dpi_p.get("mean_speed_us_per_s"))),
            ])

    widths = [max(len(h), *(len(r[i]) for r in rows)) if rows else len(h) for i, h in enumerate(sub_headers)]
    def fmt_row(cells):
        return "  ".join(c.ljust(w) for c, w in zip(cells, widths))

    # Group header spans columns [2,3,4] and [5,6,7]; center it over that span.
    group_row_cells = [group_headers[0], group_headers[1]]
    for start, group in ((2, group_headers[2]), (5, group_headers[3])):
        span_width = sum(widths[start:start + 3]) + 2 * 2  # 2 separators of width 2 inside the span
        group_row_cells.append(group.center(span_width))

    print()
    print("Speed in simulated-milliseconds per wall-clock second (higher is faster).")
    print("verilator = Verilator's own end-of-run report; custom = tb_common.sv's [SIM SPEED] report.")
    print("factor = rtl/dpi; factor > 1 means our sim is slower than Verilator RTL.")
    print()
    print("  ".join([
        group_row_cells[0].ljust(widths[0]), group_row_cells[1].ljust(widths[1]),
        group_row_cells[2], group_row_cells[3],
    ]))
    print(fmt_row(sub_headers))
    print(fmt_row(["-" * w for w in widths]))
    for row in rows:
        print(fmt_row(row))
    print()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("modules", nargs="+",
                         help="Module names under tests/ to sweep, e.g. ibex_core apb_gpio uart")
    parser.add_argument("--runs", type=int, default=1,
                         help="Number of times to repeat each (module, cross) run and average (default: 1)")
    parser.add_argument("--output", type=Path, default=None,
                         help="Where to save results JSON (default: test-results/perf/<timestamp>.json)")
    parser.add_argument("--no-docker", action="store_true",
                         help="Run make directly instead of through scripts/docker-run.sh")
    parser.add_argument("--timeout", type=int, default=1800,
                         help="Per-run timeout in seconds (default: 1800)")
    parser.add_argument("--metric", choices=["both", "verilator", "custom"], default="both",
                         help="Which speed measure to print: Verilator's own report, tb_common.sv's "
                              "[SIM SPEED] report, or both as two rows per module (default: both)")
    args = parser.parse_args()

    if args.runs < 1:
        raise RuntimeError("--runs must be >= 1")

    modules = args.modules

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    output_path = args.output or (RESULTS_ROOT / f"{stamp}.json")

    results, errors = sweep(modules, args.runs, not args.no_docker, args.timeout)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "timestamp": stamp,
        "runs": args.runs,
        "modules": modules,
        "results": results,
        "errors": errors,
    }
    output_path.write_text(json.dumps(payload, indent=2))
    print(f"Saved results to {output_path}")

    metrics = list(METRICS) if args.metric == "both" else [args.metric]
    print_table(results, modules, metrics)

    if errors:
        print(f"{len(errors)} run(s) failed; see errors in {output_path}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
