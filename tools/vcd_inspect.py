#!/usr/bin/env python3
"""
VCD file inspection tool.

Usage:
  vcd_inspect.py <vcd_file> <command> [args...]

Commands:
  header                   Show timescale, date, version
  scopes                   List all scopes as an indented tree
  signals  [scope]         List all signals (optionally filtered to a dot-path scope)
  changes  <signal_path>   Show all value changes for a signal (dot-separated scope.name)
  at       <signal_path> <time>  Show value of a signal at a given timestamp
  stats                    Count signals and timestamps per scope
"""

import sys
import re
from pathlib import Path


# ── VCD parser ────────────────────────────────────────────────────────────────

class VCDSignal:
    __slots__ = ("path", "name", "width", "id_code", "changes")
    def __init__(self, path, name, width, id_code):
        self.path     = path       # dot-separated scope path
        self.name     = name
        self.width    = width
        self.id_code  = id_code
        self.changes  = []         # list of (time, value_str)


def _tokenise(f):
    """Yield whitespace-separated tokens from an open file."""
    for line in f:
        yield from line.split()


def parse_header(path):
    """
    Parse the VCD header (up to $enddefinitions).
    Returns (meta, signals_by_code, signals_by_path).

    meta: dict with keys date, version, timescale
    signals_by_code: {id_code -> VCDSignal}
    signals_by_path: {dot.path.name -> VCDSignal}
    """
    meta = {"date": "", "version": "", "timescale": ""}
    signals_by_code = {}
    signals_by_path = {}
    scope_stack = []

    with open(path) as f:
        tokens = _tokenise(f)
        try:
            while True:
                tok = next(tokens)
                if tok == "$date":
                    parts = []
                    for t in tokens:
                        if t == "$end": break
                        parts.append(t)
                    meta["date"] = " ".join(parts)
                elif tok == "$version":
                    parts = []
                    for t in tokens:
                        if t == "$end": break
                        parts.append(t)
                    meta["version"] = " ".join(parts)
                elif tok == "$timescale":
                    parts = []
                    for t in tokens:
                        if t == "$end": break
                        parts.append(t)
                    meta["timescale"] = " ".join(parts)
                elif tok == "$scope":
                    _kind = next(tokens)   # module / begin / fork / task
                    name  = next(tokens)
                    _end  = next(tokens)   # $end
                    scope_stack.append(name)
                elif tok == "$upscope":
                    _end = next(tokens)    # $end
                    if scope_stack:
                        scope_stack.pop()
                elif tok == "$var":
                    _type    = next(tokens)   # wire / reg / ...
                    width    = int(next(tokens))
                    id_code  = next(tokens)
                    name     = next(tokens)
                    # consume optional bit-select and $end
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
                    break
        except StopIteration:
            pass

    return meta, signals_by_code, signals_by_path


def parse_changes(path, wanted_codes):
    """
    Stream the value-change section and collect changes for the given id_codes.
    Returns {id_code -> [(time, value_str), ...]}.
    """
    results = {c: [] for c in wanted_codes}
    current_time = 0
    in_defs = True

    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            # Skip header section
            if in_defs:
                if "$enddefinitions" in line:
                    in_defs = False
                continue

            if line.startswith("#"):
                try:
                    current_time = int(line[1:])
                except ValueError:
                    pass
                continue

            # Scalar: 0/1/x/z followed immediately by id_code (no space)
            if line[0] in "01xzXZ" and len(line) > 1 and not line[0].isalpha():
                val     = line[0]
                id_code = line[1:]
                if id_code in results:
                    results[id_code].append((current_time, val))
                continue

            # Vector: b/B/r/R <value> <id_code>
            if line[0] in "bBrR":
                parts = line.split()
                if len(parts) >= 2:
                    val     = parts[0][1:]   # strip leading b/r
                    id_code = parts[1]
                    if id_code in results:
                        results[id_code].append((current_time, val))

    return results


# ── scope tree helpers ─────────────────────────────────────────────────────────

def build_scope_tree(signals_by_path):
    """Return set of all unique scope paths."""
    scopes = set()
    for sig in signals_by_path.values():
        path = sig.path
        while path:
            scopes.add(path)
            last_dot = path.rfind(".")
            path = path[:last_dot] if last_dot >= 0 else ""
    return scopes


def signals_in_scope(signals_by_path, scope):
    """Return signals whose path == scope (direct children only)."""
    return [s for s in signals_by_path.values() if s.path == scope]


# ── commands ───────────────────────────────────────────────────────────────────

def cmd_header(vcd_path, meta, _sc, _sp, args):
    print(f"File:       {vcd_path}")
    print(f"Timescale:  {meta['timescale']}")
    print(f"Date:       {meta['date']}")
    print(f"Version:    {meta['version']}")


def cmd_scopes(vcd_path, meta, signals_by_code, signals_by_path, args):
    scopes = build_scope_tree(signals_by_path)
    if not scopes:
        print("(no scopes found)")
        return
    # Print as indented tree
    printed = set()

    def _print_children(parent, depth):
        children = sorted(
            s for s in scopes
            if s.count(".") == (parent.count(".") + 1 if parent else 0)
            and (s.startswith(parent + ".") if parent else "." not in s)
        )
        for child in children:
            name = child.split(".")[-1]
            nsig = len(signals_in_scope(signals_by_path, child))
            print("  " * depth + f"{name}  ({nsig} signals)")
            printed.add(child)
            _print_children(child, depth + 1)

    # Find roots (scopes with no dot or whose parent isn't in scopes)
    roots = sorted(s for s in scopes if "." not in s or s.rsplit(".", 1)[0] not in scopes)
    for root in roots:
        name = root.split(".")[-1]
        nsig = len(signals_in_scope(signals_by_path, root))
        print(f"{name}  ({nsig} signals)")
        _print_children(root, 1)


def cmd_signals(vcd_path, meta, signals_by_code, signals_by_path, args):
    scope_filter = args[0] if args else None
    sigs = sorted(signals_by_path.values(), key=lambda s: (s.path, s.name))
    if scope_filter:
        sigs = [s for s in sigs
                if s.path == scope_filter or s.path.startswith(scope_filter + ".")]
    if not sigs:
        print(f"No signals found" + (f" under '{scope_filter}'" if scope_filter else ""))
        return
    for s in sigs:
        full = (s.path + "." + s.name) if s.path else s.name
        print(f"{full}  [{s.width}b]  id:{s.id_code}")


def cmd_changes(vcd_path, meta, signals_by_code, signals_by_path, args):
    if not args:
        sys.exit("changes requires a signal path (scope.name)")
    sig_path = args[0]
    sig = signals_by_path.get(sig_path)
    if sig is None:
        sys.exit(f"Signal '{sig_path}' not found. Use 'signals' command to list available signals.")

    results = parse_changes(vcd_path, {sig.id_code})
    changes = results[sig.id_code]
    if not changes:
        print(f"No changes recorded for '{sig_path}'")
        return
    ts_unit = meta.get("timescale", "")
    print(f"Signal: {sig_path}  [{sig.width}b]  ({len(changes)} transitions)  timescale: {ts_unit}")
    for time, val in changes:
        print(f"  #{time:<12}  {val}")


def cmd_at(vcd_path, meta, signals_by_code, signals_by_path, args):
    if len(args) < 2:
        sys.exit("at requires <signal_path> <time>")
    sig_path = args[0]
    try:
        query_time = int(args[1])
    except ValueError:
        sys.exit(f"time must be an integer, got: {args[1]}")

    sig = signals_by_path.get(sig_path)
    if sig is None:
        sys.exit(f"Signal '{sig_path}' not found.")

    results = parse_changes(vcd_path, {sig.id_code})
    changes = results[sig.id_code]

    # Find the last change at or before query_time
    value = None
    for time, val in changes:
        if time <= query_time:
            value = val
        else:
            break

    ts_unit = meta.get("timescale", "")
    if value is None:
        print(f"{sig_path} @ #{query_time} {ts_unit}:  (no value — before first transition)")
    else:
        print(f"{sig_path} @ #{query_time} {ts_unit}:  {value}")


def cmd_stats(vcd_path, meta, signals_by_code, signals_by_path, args):
    scopes = build_scope_tree(signals_by_path)
    all_scopes = sorted(scopes)

    # Count timestamps (stream file once)
    ts_count = 0
    in_defs = True
    with open(vcd_path) as f:
        for line in f:
            if in_defs:
                if "$enddefinitions" in line:
                    in_defs = False
                continue
            if line.startswith("#"):
                ts_count += 1

    total_sigs = len(signals_by_path)
    print(f"Total signals:    {total_sigs}")
    print(f"Total scopes:     {len(scopes)}")
    print(f"Total timestamps: {ts_count}")
    print(f"Timescale:        {meta['timescale']}")
    print()

    col_w = max((len(s) for s in all_scopes), default=10)
    print(f"{'Scope':<{col_w}}  {'Signals':>7}")
    print("-" * (col_w + 10))
    for scope in all_scopes:
        nsig = len(signals_in_scope(signals_by_path, scope))
        print(f"{scope:<{col_w}}  {nsig:>7}")


# ── main ──────────────────────────────────────────────────────────────────────

COMMANDS = {
    "header":  cmd_header,
    "scopes":  cmd_scopes,
    "signals": cmd_signals,
    "changes": cmd_changes,
    "at":      cmd_at,
    "stats":   cmd_stats,
}

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(0)

    vcd_file = sys.argv[1]
    command  = sys.argv[2]
    rest     = sys.argv[3:]

    if command not in COMMANDS:
        sys.exit(f"Unknown command '{command}'. Available: {', '.join(COMMANDS)}")

    meta, signals_by_code, signals_by_path = parse_header(vcd_file)
    COMMANDS[command](vcd_file, meta, signals_by_code, signals_by_path, rest)
