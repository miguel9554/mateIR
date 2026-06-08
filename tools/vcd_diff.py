#!/usr/bin/env python3
"""
Unified VCD diff tool.

Usage:
  vcd_diff.py signal <vcd1> <signal1> <vcd2> <signal2>
  vcd_diff.py signal <vcd1> <vcd2> --signal <suffix>
  vcd_diff.py scope  <vcd1> <scope1> <vcd2> <scope2> [--signals]
"""

import sys
import re


_UNIT_PS = {"ps": 1, "ns": 1_000, "us": 1_000_000, "ms": 1_000_000_000, "s": 1_000_000_000_000}


def _parse_timescale(ts_str):
    ts_str = ts_str.strip()
    match = re.match(r"(\d+)\s*(ps|ns|us|ms|s)", ts_str, re.IGNORECASE)
    if not match:
        sys.exit(f"Cannot parse timescale '{ts_str}'")
    value = int(match.group(1))
    unit = match.group(2).lower()
    return value * _UNIT_PS[unit], f"{value}{unit}"


class VCDSignal:
    __slots__ = ("path", "name", "width", "id_code", "changes")

    def __init__(self, path, name, width, id_code):
        self.path = path
        self.name = name
        self.width = width
        self.id_code = id_code
        self.changes = []

    @property
    def full_name(self):
        return f"{self.path}.{self.name}" if self.path else self.name


def _tokenise(handle):
    for line in handle:
        yield from line.split()


def parse_vcd(path):
    meta = {"timescale": "1ps"}
    signals_by_code = {}
    signals_by_path = {}
    scope_stack = []
    current_time = 0
    in_defs = True

    with open(path) as handle:
        tokens = _tokenise(handle)
        try:
            while True:
                token = next(tokens)
                if in_defs:
                    if token == "$timescale":
                        parts = []
                        for inner in tokens:
                            if inner == "$end":
                                break
                            parts.append(inner)
                        meta["timescale"] = " ".join(parts)
                    elif token == "$scope":
                        next(tokens)
                        name = next(tokens)
                        next(tokens)
                        scope_stack.append(name)
                    elif token == "$upscope":
                        next(tokens)
                        if scope_stack:
                            scope_stack.pop()
                    elif token == "$var":
                        next(tokens)
                        width = int(next(tokens))
                        id_code = next(tokens)
                        name = next(tokens)
                        for inner in tokens:
                            if inner == "$end":
                                break
                        scope = ".".join(scope_stack)
                        signal = VCDSignal(scope, name, width, id_code)
                        signals_by_code[id_code] = signal
                        signals_by_path[signal.full_name] = signal
                    elif token == "$enddefinitions":
                        for inner in tokens:
                            if inner == "$end":
                                break
                        in_defs = False
                    continue

                if token.startswith("#"):
                    current_time = int(token[1:])
                elif token[0] in "01xzXZ" and len(token) > 1 and not token[0].isalpha():
                    value = token[0]
                    id_code = token[1:]
                    if id_code in signals_by_code:
                        signals_by_code[id_code].changes.append((current_time, value))
                elif token[0] in "bBrR":
                    value = token[1:]
                    id_code = next(tokens)
                    if id_code in signals_by_code:
                        signals_by_code[id_code].changes.append((current_time, value))
        except StopIteration:
            pass

    return meta, signals_by_path


def _value_at(changes, time):
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


def _resolve_signal(signals_by_path, path):
    if path in signals_by_path:
        return signals_by_path[path]
    path_lower = path.lower()
    exact = [sig for name, sig in signals_by_path.items() if name.lower() == path_lower]
    if len(exact) == 1:
        return exact[0]
    suffix = "." + path
    suffix_matches = [sig for name, sig in signals_by_path.items() if name.endswith(suffix)]
    if len(suffix_matches) == 1:
        return suffix_matches[0]
    candidates = sorted(name for name in signals_by_path if path_lower in name.lower())
    if candidates:
        sys.exit(
            f"Signal '{path}' is ambiguous or missing. Candidates:\n" +
            "\n".join(f"  {name}" for name in candidates[:12])
        )
    sys.exit(f"Signal '{path}' not found in VCD.")


def _resolve_unique_suffix(signals_by_path, suffix):
    matches = [sig for name, sig in signals_by_path.items()
               if name == suffix or name.endswith("." + suffix)]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        sys.exit(f"Signal suffix '{suffix}' not found.")
    sys.exit(
        f"Signal suffix '{suffix}' is ambiguous:\n" +
        "\n".join(f"  {sig.full_name}" for sig in sorted(matches, key=lambda s: s.full_name)[:12])
    )


def _build_timeline(sig1, ps1, sig2, ps2):
    times_ps = {time * ps1 for time, _ in sig1.changes}
    times_ps |= {time * ps2 for time, _ in sig2.changes}
    return sorted(times_ps)


def _signal_value_at_ps(signal, ps_per_tick, time_ps):
    raw_time = time_ps // ps_per_tick
    return _value_at(signal.changes, raw_time)


def signal_mode(args):
    if len(args) == 4:
        vcd1_path, sig1_name, vcd2_path, sig2_name = args
        meta1, signals1 = parse_vcd(vcd1_path)
        meta2, signals2 = parse_vcd(vcd2_path)
        sig1 = _resolve_signal(signals1, sig1_name)
        sig2 = _resolve_signal(signals2, sig2_name)
    elif len(args) == 4 and args[2] == "--signal":
        vcd1_path, vcd2_path, _, suffix = args
        meta1, signals1 = parse_vcd(vcd1_path)
        meta2, signals2 = parse_vcd(vcd2_path)
        sig1 = _resolve_unique_suffix(signals1, suffix)
        sig2 = _resolve_unique_suffix(signals2, suffix)
        print(f"Resolved: {sig1.full_name}  <->  {sig2.full_name}")
    else:
        sys.exit(__doc__)

    ps1, ts1 = _parse_timescale(meta1["timescale"])
    ps2, ts2 = _parse_timescale(meta2["timescale"])
    print(f"VCD1: {vcd1_path}  timescale={ts1}  signal={sig1.full_name}  ({len(sig1.changes)} changes)")
    print(f"VCD2: {vcd2_path}  timescale={ts2}  signal={sig2.full_name}  ({len(sig2.changes)} changes)")
    print()

    timeline = _build_timeline(sig1, ps1, sig2, ps2)
    if not timeline:
        print("No value changes found for either signal.")
        return

    col1 = max(len(sig1.full_name), 12)
    col2 = max(len(sig2.full_name), 12)
    print(f"  {'Time (ps)':>14}  {sig1.full_name:<{col1}}  {sig2.full_name:<{col2}}  STATUS")
    print(f"  {'-'*14}  {'-'*col1}  {'-'*col2}  ------")

    prev1 = prev2 = None
    diverges = 0
    for time_ps in timeline:
        value1 = _signal_value_at_ps(sig1, ps1, time_ps)
        value2 = _signal_value_at_ps(sig2, ps2, time_ps)
        if value1 is None:
            value1 = prev1 if prev1 is not None else "?"
        else:
            prev1 = value1
        if value2 is None:
            value2 = prev2 if prev2 is not None else "?"
        else:
            prev2 = value2
        status = "OK" if value1 == value2 else "*** DIVERGE"
        if status != "OK":
            diverges += 1
        print(f"  {time_ps:>14}  {value1:<{col1}}  {value2:<{col2}}  {status}")

    print()
    print("Signals match at all transition points." if diverges == 0
          else f"{diverges} divergence point(s) found.")


def scope_mode(args):
    show_signals = False
    if "--signals" in args:
        show_signals = True
        args = [arg for arg in args if arg != "--signals"]
    if len(args) != 4:
        sys.exit(__doc__)

    vcd1_path, scope1, vcd2_path, scope2 = args
    meta1, signals1 = parse_vcd(vcd1_path)
    meta2, signals2 = parse_vcd(vcd2_path)
    ps1, ts1 = _parse_timescale(meta1["timescale"])
    ps2, ts2 = _parse_timescale(meta2["timescale"])

    scoped1 = {sig.name: sig for sig in signals1.values() if sig.path == scope1}
    scoped2 = {sig.name: sig for sig in signals2.values() if sig.path == scope2}
    common = sorted(set(scoped1) & set(scoped2))
    if not common:
        sys.exit("No matching direct child signals found between the two scopes.")

    rows = []
    for name in common:
        sig1 = scoped1[name]
        sig2 = scoped2[name]
        timeline = _build_timeline(sig1, ps1, sig2, ps2)
        prev1 = prev2 = None
        first_diverge = None
        diverge_v1 = diverge_v2 = None
        for time_ps in timeline:
            value1 = _signal_value_at_ps(sig1, ps1, time_ps)
            value2 = _signal_value_at_ps(sig2, ps2, time_ps)
            if value1 is None:
                value1 = prev1 if prev1 is not None else "?"
            else:
                prev1 = value1
            if value2 is None:
                value2 = prev2 if prev2 is not None else "?"
            else:
                prev2 = value2
            if value1 != value2:
                first_diverge = time_ps
                diverge_v1 = value1
                diverge_v2 = value2
                break
        if first_diverge is not None:
            rows.append((first_diverge, name, diverge_v1, diverge_v2, sig1.full_name, sig2.full_name))

    rows.sort()
    print(f"VCD1: {vcd1_path}  timescale={ts1}  scope={scope1}")
    print(f"VCD2: {vcd2_path}  timescale={ts2}  scope={scope2}")
    print()
    if not rows:
        print("No divergences found in matching scope signals.")
        return

    label_width = max(24, max(len(name if not show_signals else f"{full1} <-> {full2}")
                              for _, name, _, _, full1, full2 in rows))
    print(f"{'Signal':<{label_width}} {'First divergence (ps)':>22}  {'VCD1':<16}  {'VCD2':<16}")
    print(f"{'-'*label_width} {'-'*22}  {'-'*16}  {'-'*16}")
    for time_ps, name, value1, value2, full1, full2 in rows:
        label = name if not show_signals else f"{full1} <-> {full2}"
        print(f"{label:<{label_width}} {time_ps:>22}  {value1:<16}  {value2:<16}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    mode = sys.argv[1]
    args = sys.argv[2:]
    if mode == "signal":
        signal_mode(args)
    elif mode == "scope":
        scope_mode(args)
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
