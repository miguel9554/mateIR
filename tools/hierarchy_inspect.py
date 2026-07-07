#!/usr/bin/env python3
"""
Hierarchy JSON inspection tool.

Usage:
  hierarchy_inspect.py <json_file> <command> [args...]

Commands:
  tree                    Print the full module hierarchy as an indented tree
  module  <path>          Show all signals/flops of a module (dot-separated instance path)
  flops   <path>          List flops of a module with clock/reset info
  inputs  <path>          List inputs of a module with sync_type domain IDs
  outputs <path>          List outputs of a module with sync_type domain IDs
  signals <path>          List internal signals of a module
  find    <name>          Find all signals/flops named <name> anywhere in the hierarchy
  clocks                  List all distinct clock domains used across the hierarchy
  stats                   Print signal/flop counts per module
  interfaces              List language-metadata interface groupings (ports/instances)
"""

import json
import sys
from pathlib import Path


# ── loading ───────────────────────────────────────────────────────────────────

def load(path):
    with open(path) as f:
        data = json.load(f)
    if "top" in data:
        top = data["top"]
        top["__clock_domains"] = data.get("clock_domains", [])
        top["__reset_domains"] = data.get("reset_domains", [])
        top["__lang_metadata"] = data.get("lang_metadata", [])
        return top
    return data


# ── hierarchy navigation ───────────────────────────────────────────────────────

def _root_label(root):
    """Return a display name for the root module (uses name when instance_name is empty)."""
    return root.get("instance_name") or root.get("name", "")


def find_module(root, dot_path):
    """
    Walk to the module identified by a dot-separated instance path.
    An empty string, or a path equal to the root's name/instance_name, returns the root.
    The leading root name component is optional — 'u_din' and 'root.u_din' both work.
    Returns None if any step is missing.
    """
    if not dot_path:
        return root
    parts = dot_path.split(".")
    # Strip a leading component that matches the root label (instance_name or name)
    if parts[0] in (root.get("instance_name", ""), root.get("name", "")):
        parts = parts[1:]
    if not parts:
        return root
    node = root
    for part in parts:
        found = None
        for sub in node.get("submodules", []):
            if sub["instance_name"] == part:
                found = sub
                break
        if found is None:
            return None
        node = found
    return node


def iter_modules(node, path=""):
    """Yield (dot_path, module) for every node in the tree (DFS)."""
    label = node.get("instance_name") or node.get("name", "")
    cur_path = (path + "." + label).lstrip(".") if label else path
    yield cur_path, node
    for sub in node.get("submodules", []):
        yield from iter_modules(sub, cur_path)


# ── formatting ─────────────────────────────────────────────────────────────────

def fmt_type(t):
    if not t:
        return "?"
    width = t.get("width", "?")
    signed = "s" if t.get("signed") else "u"
    udims = t.get("unpacked_dims", [])
    udim_str = "".join(f"[{d['left']}:{d['right']}]" for d in udims)
    return f"{width}{signed}{udim_str}"


def fmt_signal(s, category=""):
    name  = s["name"]
    typ   = fmt_type(s.get("type"))
    sync  = s.get("sync_type", {})
    sk    = s.get("sync_kind", sync.get("kind", ""))
    parts = [f"{name}  [{typ}]"]
    if sk:
        parts.append(sk)
    if "clock_domain" in sync:
        parts.append(f"clk:{sync['clock_domain']}")
    if "reset_domain" in sync:
        parts.append(f"rst:{sync['reset_domain']}")
    if sync.get("reset_domains"):
        parts.append("rst:{}".format(",".join(str(r) for r in sync["reset_domains"])))
    return "  ".join(parts)


def fmt_flop(f):
    name  = f["name"]
    typ   = fmt_type(f.get("type"))
    clk   = f.get("clock_domain")
    resets = f.get("reset_domains", [])
    rv    = f.get("reset_value")
    rst_str = f"  rst:{','.join(str(r) for r in resets)}={rv}" if resets else ""
    return f"{name}  [{typ}]  clk:{clk}{rst_str}"


# ── commands ───────────────────────────────────────────────────────────────────

def cmd_tree(root, args):
    def _print(node, depth):
        inst = node.get("instance_name") or node.get("name", "?")
        mod  = node.get("name", "")
        label = inst if inst == mod else f"{inst}  ({mod})"
        print("  " * depth + label)
        for sub in node.get("submodules", []):
            _print(sub, depth + 1)
    _print(root, 0)


def cmd_module(root, args):
    if not args:
        sys.exit("module requires a dot-separated instance path")
    m = find_module(root, args[0])
    if m is None:
        sys.exit(f"Module not found: {args[0]}")

    inst = m.get("instance_name") or m.get("name", "?")
    mod  = m.get("name", "")
    print(f"Module: {inst}  ({mod})")
    print()

    sections = [
        ("Inputs",    m.get("inputs", [])),
        ("Outputs",   m.get("outputs", [])),
        ("Signals",   m.get("signals", [])),
        ("Flops",     None),
        ("Parameters", m.get("parameters", [])),
        ("Localparams", m.get("localparams", [])),
    ]
    for label, items in sections:
        if label == "Flops":
            flops = m.get("flops", [])
            if flops:
                print(f"  {label} ({len(flops)}):")
                for f in flops:
                    print(f"    {fmt_flop(f)}")
        else:
            if items:
                print(f"  {label} ({len(items)}):")
                for s in items:
                    print(f"    {fmt_signal(s)}")

    subs = m.get("submodules", [])
    if subs:
        print(f"\n  Submodules ({len(subs)}):")
        for sub in subs:
            print(f"    {sub['instance_name']}  ({sub['name']})")


def _list_category(root, args, category):
    if not args:
        sys.exit(f"{category} requires a dot-separated instance path")
    m = find_module(root, args[0])
    if m is None:
        sys.exit(f"Module not found: {args[0]}")
    items = m.get(category, [])
    if not items:
        print(f"(no {category})")
        return
    for item in items:
        if category == "flops":
            print(fmt_flop(item))
        else:
            print(fmt_signal(item))


def cmd_flops(root, args):   _list_category(root, args, "flops")
def cmd_inputs(root, args):  _list_category(root, args, "inputs")
def cmd_outputs(root, args): _list_category(root, args, "outputs")
def cmd_signals(root, args): _list_category(root, args, "signals")


def cmd_find(root, args):
    if not args:
        sys.exit("find requires a signal name")
    target = args[0]
    found = False
    for path, m in iter_modules(root):
        for cat in ("inputs", "outputs", "signals"):
            for s in m.get(cat, []):
                if s["name"] == target:
                    print(f"{path}  [{cat[:-1]}]  {fmt_signal(s)}")
                    found = True
        for f in m.get("flops", []):
            if f["name"] == target:
                print(f"{path}  [flop]  {fmt_flop(f)}")
                found = True
    if not found:
        print(f"'{target}' not found anywhere in the hierarchy")


def cmd_clocks(root, args):
    registry = {d.get("id"): d for d in root.get("__clock_domains", [])}
    clocks = {}  # clock_id -> set of module paths
    for path, m in iter_modules(root):
        for cat in ("inputs", "outputs", "signals"):
            for s in m.get(cat, []):
                cd = s.get("sync_type", {}).get("clock_domain")
                if cd is not None:
                    clocks.setdefault(cd, set()).add(path)
        for f in m.get("flops", []):
            clk = f.get("clock_domain")
            if clk is not None:
                clocks.setdefault(clk, set()).add(path)
    if not clocks:
        print("No clock domains found")
        return
    for clk, paths in sorted(clocks.items()):
        domain = registry.get(clk, {})
        label = domain.get("display_name", f"clock_{clk}")
        edge = domain.get("edge", "")
        print(f"{clk}: {label} {edge}  (used in {len(paths)} module(s))")
        for p in sorted(paths):
            print(f"    {p}")


def cmd_stats(root, args):
    col_w = max(len(p) for p, _ in iter_modules(root))
    header = f"{'Module':<{col_w}}  {'In':>4}  {'Out':>4}  {'Sig':>4}  {'Flops':>5}  {'Subs':>4}"
    print(header)
    print("-" * len(header))
    for path, m in iter_modules(root):
        ni = len(m.get("inputs", []))
        no = len(m.get("outputs", []))
        ns = len(m.get("signals", []))
        nf = len(m.get("flops", []))
        nb = len(m.get("submodules", []))
        print(f"{path:<{col_w}}  {ni:>4}  {no:>4}  {ns:>4}  {nf:>5}  {nb:>4}")


def cmd_interfaces(root, args):
    records = root.get("__lang_metadata", [])
    iface_records = [r for r in records
                     if r.get("kind") in ("sv.interface_port", "sv.interface_instance")]
    if not iface_records:
        print("No interface records in lang_metadata.")
        return
    for r in iface_records:
        attrs = r.get("attrs", {})
        kind = "port" if r["kind"] == "sv.interface_port" else "instance"
        bindings = r.get("bindings", [])
        module_path = bindings[0]["anchor"]["module_path"] if bindings else ""
        where = module_path or "<top>"
        modport = f" modport={attrs['modport']}" if "modport" in attrs else ""
        print(f"{where}: {r['name']}  ({kind} of {attrs.get('interface', '?')}{modport})")
        for b in bindings:
            anchor = b["anchor"]
            print(f"    {b['role']:<20} -> {anchor['entity']} {anchor['name']}")


# ── main ──────────────────────────────────────────────────────────────────────

COMMANDS = {
    "tree":    cmd_tree,
    "module":  cmd_module,
    "flops":   cmd_flops,
    "inputs":  cmd_inputs,
    "outputs": cmd_outputs,
    "signals": cmd_signals,
    "find":    cmd_find,
    "clocks":  cmd_clocks,
    "stats":   cmd_stats,
    "interfaces": cmd_interfaces,
}

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(0)

    json_file = sys.argv[1]
    command   = sys.argv[2]
    rest      = sys.argv[3:]

    if command not in COMMANDS:
        sys.exit(f"Unknown command '{command}'. Available: {', '.join(COMMANDS)}")

    root = load(json_file)
    COMMANDS[command](root, rest)
