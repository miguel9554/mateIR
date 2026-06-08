#!/usr/bin/env python3
"""
Compare one signal from two VCD files on a unified, normalised timeline.

Usage:
  vcd_diff_signal.py <vcd1> <signal1> <vcd2> <signal2>

Signal paths use dot-separated scope.name notation, e.g.:
  TOP.ibex_core.cpuctrlsts_part_q.double_fault_seen

The two VCDs may have different timescales (e.g. 1ps vs 1ns) — all timestamps
are normalised to picoseconds before alignment. Divergence points are flagged.
"""

import sys
import re
from pathlib import Path


# ── timescale parsing ─────────────────────────────────────────────────────────

_UNIT_PS = {"ps": 1, "ns": 1_000, "us": 1_000_000, "ms": 1_000_000_000, "s": 1_000_000_000_000}

def _parse_timescale(ts_str):
    """Return (multiplier_ps, unit_str). E.g. '1 ns' -> (1000, '1ns')."""
    ts_str = ts_str.strip()
    m = re.match(r"(\d+)\s*(ps|ns|us|ms|s)", ts_str, re.IGNORECASE)
    if not m:
        sys.exit(f"Cannot parse timescale '{ts_str}'")
    val = int(m.group(1))
    unit = m.group(2).lower()
    return val * _UNIT_PS[unit], f"{val}{unit}"


# ── VCD parser ────────────────────────────────────────────────────────────────

class VCDSignal:
    __slots__ = ("path", "name", "width", "id_code", "changes")
    def __init__(self, path, name, width, id_code):
        self.path    = path
        self.name    = name
        self.width   = width
        self.id_code = id_code
        self.changes = []  # (time_raw, value_str)


def _tokenise(f):
    for line in f:
        yield from line.split()


def parse_vcd(path):
    """
    Parse an entire VCD file.
    Returns (meta, signals_by_code, signals_by_path).
    meta has 'timescale'.
    """
    meta = {"timescale": "1ps"}
    signals_by_code = {}
    signals_by_path = {}
    scope_stack = []
    in_defs = True
    current_time = 0

    with open(path) as f:
        tokens = _tokenise(f)
        try:
            while True:
                tok = next(tokens)

                if in_defs:
                    if tok == "$timescale":
                        parts = []
                        for t in tokens:
                            if t == "$end": break
                            parts.append(t)
                        meta["timescale"] = " ".join(parts)
                    elif tok == "$scope":
                        _kind = next(tokens)
                        name  = next(tokens)
                        next(tokens)  # $end
                        scope_stack.append(name)
                    elif tok == "$upscope":
                        next(tokens)  # $end
                        if scope_stack:
                            scope_stack.pop()
                    elif tok == "$var":
                        _type   = next(tokens)
                        width   = int(next(tokens))
                        id_code = next(tokens)
                        name    = next(tokens)
                        for t in tokens:
                            if t == "$end": break
                        scope_path = ".".join(scope_stack)
                        full_path  = (scope_path + "." + name) if scope_path else name
                        sig = VCDSignal(scope_path, name, width, id_code)
                        signals_by_code[id_code] = sig
                        signals_by_path[full_path] = sig
                    elif tok == "$enddefinitions":
                        for t in tokens:
                            if t == "$end": break
                        in_defs = False
                    continue

                # value-change section
                if tok.startswith("#"):
                    try:
                        current_time = int(tok[1:])
                    except ValueError:
                        pass
                elif tok[0] in "01xzXZ" and len(tok) > 1 and not tok[0].isalpha():
                    val     = tok[0]
                    id_code = tok[1:]
                    if id_code in signals_by_code:
                        signals_by_code[id_code].changes.append((current_time, val))
                elif tok[0] in "bBrR":
                    val     = tok[1:]
                    id_code = next(tokens)
                    if id_code in signals_by_code:
                        signals_by_code[id_code].changes.append((current_time, val))

        except StopIteration:
            pass

    return meta, signals_by_code, signals_by_path


# ── helpers ───────────────────────────────────────────────────────────────────

def _find_signal(signals_by_path, path):
    if path in signals_by_path:
        return signals_by_path[path]
    # Case-insensitive fallback
    path_l = path.lower()
    for k, v in signals_by_path.items():
        if k.lower() == path_l:
            return v
    # Suffix match — allow omitting top scopes
    suffix = "." + path
    for k, v in signals_by_path.items():
        if k.endswith(suffix):
            return v
    candidates = [k for k in signals_by_path if path_l in k.lower()]
    if candidates:
        sys.exit(
            f"Signal '{path}' not found. Possible matches:\n" +
            "\n".join(f"  {c}" for c in sorted(candidates)[:10])
        )
    sys.exit(f"Signal '{path}' not found in VCD.")


def _value_at(changes, time):
    """Binary-search: last value at or before time."""
    lo, hi = 0, len(changes) - 1
    result = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if changes[mid][0] <= time:
            result = changes[mid][1]
            lo = mid + 1
        else:
            hi = mid - 1
    return result


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(0)

    vcd1_path, sig1_path, vcd2_path, sig2_path = sys.argv[1:]

    meta1, sc1, sp1 = parse_vcd(vcd1_path)
    meta2, sc2, sp2 = parse_vcd(vcd2_path)

    sig1 = _find_signal(sp1, sig1_path)
    sig2 = _find_signal(sp2, sig2_path)

    ts1_ps, ts1_str = _parse_timescale(meta1["timescale"])
    ts2_ps, ts2_str = _parse_timescale(meta2["timescale"])

    print(f"VCD1: {vcd1_path}  timescale={ts1_str}  signal={sig1.path}.{sig1.name}  ({len(sig1.changes)} changes)")
    print(f"VCD2: {vcd2_path}  timescale={ts2_str}  signal={sig2.path}.{sig2.name}  ({len(sig2.changes)} changes)")
    print()

    # Build unified timeline in picoseconds
    times_ps = set()
    for t, _ in sig1.changes:
        times_ps.add(t * ts1_ps)
    for t, _ in sig2.changes:
        times_ps.add(t * ts2_ps)

    if not times_ps:
        print("No value changes found for either signal.")
        return

    # Header
    col1 = max(len(sig1_path), 12)
    col2 = max(len(sig2_path), 12)
    print(f"  {'Time (ps)':>14}  {sig1_path:<{col1}}  {sig2_path:<{col2}}  STATUS")
    print(f"  {'-'*14}  {'-'*col1}  {'-'*col2}  ------")

    prev1 = prev2 = None
    diverge_count = 0

    for t_ps in sorted(times_ps):
        # Find raw time in each VCD's units
        raw1 = t_ps // ts1_ps
        raw2 = t_ps // ts2_ps

        v1 = _value_at(sig1.changes, raw1)
        v2 = _value_at(sig2.changes, raw2)

        if v1 is None and prev1 is None:
            v1_str = "?"
        elif v1 is not None:
            v1_str = v1
            prev1  = v1
        else:
            v1_str = prev1  # unchanged since last event

        if v2 is None and prev2 is None:
            v2_str = "?"
        elif v2 is not None:
            v2_str = v2
            prev2  = v2
        else:
            v2_str = prev2

        match = v1_str == v2_str
        status = "OK" if match else "*** DIVERGE"
        if not match:
            diverge_count += 1
        print(f"  {t_ps:>14}  {str(v1_str):<{col1}}  {str(v2_str):<{col2}}  {status}")

    print()
    if diverge_count == 0:
        print("Signals match at all transition points.")
    else:
        print(f"{diverge_count} divergence point(s) found.")


if __name__ == "__main__":
    main()
