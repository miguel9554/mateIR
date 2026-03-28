#!/usr/bin/env python3
"""
DFG JSON inspection tool.

Usage:
  dfg_inspect.py <json_file> <command> [args...]

Commands:
  driver  <name>      Show what drives a named signal/output (one level)
  cone    <name>      Show the full backward cone (all transitive drivers)
  node    <id>        Show details of a node by numeric ID
  op      <opname>    List all nodes with a given op (e.g. MUX, INDEX, CONCAT)
  fanout  <id>        Show all nodes that consume node <id>
  mux     <name>      Show the MUX tree rooted at the driver of <name>
  inputs              List all INPUT nodes with types
  outputs             List all OUTPUT nodes and their drivers
  signals             List all SIGNAL nodes and their drivers
  undriven            Find all signals/outputs with no driver
  diff    <json2>     Compare two DFGs: show driver changes for each signal
  path    <id1> <id2> Check if there is a path from node id1 to node id2
"""

import json
import sys
from pathlib import Path


def load(path):
    with open(path) as f:
        data = json.load(f)
    nodes = {n["id"]: n for n in data["nodes"]}
    # Build name -> id map (prefer SIGNAL/OUTPUT/INPUT over anonymous nodes)
    by_name = {}
    for n in data["nodes"]:
        name = n.get("name")
        if name:
            if name not in by_name:
                by_name[name] = n["id"]
            else:
                # Prefer SIGNAL/OUTPUT/INPUT
                existing = nodes[by_name[name]]
                if n["op"] in ("SIGNAL", "OUTPUT", "INPUT"):
                    by_name[name] = n["id"]
    # Build reverse map: id -> list of (consumer_id, slot)
    consumers = {n["id"]: [] for n in data["nodes"]}
    for n in data["nodes"]:
        for slot, inp in enumerate(n.get("inputs", [])):
            consumers[inp].append((n["id"], slot))
    return nodes, by_name, consumers


def fmt_node(n):
    parts = [f"[{n['id']}]", n["op"]]
    if n.get("name"):
        parts.append(f"'{n['name']}'")
    if "value" in n:
        parts.append(f"= {n['value']}")
    if n.get("type"):
        t = n["type"]
        sign = "s" if t.get("signed") else "u"
        parts.append(f"[{t['width']}{sign}]")
    if n.get("loc"):
        parts.append(f"@ {n['loc']}")
    return "  ".join(parts)


def resolve_name(name_or_id, nodes, by_name):
    """Return node id given a name or numeric id string."""
    try:
        nid = int(name_or_id)
        if nid not in nodes:
            sys.exit(f"No node with id {nid}")
        return nid
    except ValueError:
        if name_or_id not in by_name:
            sys.exit(f"No node named '{name_or_id}'")
        return by_name[name_or_id]


# ── commands ──────────────────────────────────────────────────────────────────

def cmd_node(nodes, by_name, consumers, args):
    nid = resolve_name(args[0], nodes, by_name)
    n = nodes[nid]
    print(fmt_node(n))
    if n.get("inputs"):
        print("  inputs:")
        for slot, inp in enumerate(n["inputs"]):
            label = ""
            if n["op"] == "MUX":
                label = ["sel", "T", "F"][slot] if slot < 3 else str(slot)
            elif n["op"] == "INDEX":
                label = ["src", "hi", "lo"][slot] if slot < 3 else str(slot)
            else:
                label = str(slot)
            print(f"    [{label}] {fmt_node(nodes[inp])}")
    fan = consumers[nid]
    if fan:
        print("  consumed by:")
        for cid, slot in fan:
            print(f"    {fmt_node(nodes[cid])}  (slot {slot})")


def cmd_driver(nodes, by_name, consumers, args):
    nid = resolve_name(args[0], nodes, by_name)
    n = nodes[nid]
    print(f"Signal: {fmt_node(n)}")
    inps = n.get("inputs", [])
    if not inps:
        print("  (no driver)")
        return
    for slot, inp in enumerate(inps):
        d = nodes[inp]
        label = ""
        if n["op"] == "MUX":
            label = ["sel", "T", "F"][slot] if slot < 3 else str(slot)
        print(f"  driver[{label or slot}]: {fmt_node(d)}")


def _cone(nid, nodes, visited, depth, max_depth):
    if nid in visited or depth > max_depth:
        return
    visited.add(nid)
    n = nodes[nid]
    indent = "  " * depth
    print(f"{indent}{fmt_node(n)}")
    for inp in n.get("inputs", []):
        _cone(inp, nodes, visited, depth + 1, max_depth)


def cmd_cone(nodes, by_name, consumers, args):
    nid = resolve_name(args[0], nodes, by_name)
    max_depth = int(args[1]) if len(args) > 1 else 30
    _cone(nid, nodes, set(), 0, max_depth)


def cmd_op(nodes, by_name, consumers, args):
    opname = args[0].upper()
    found = [n for n in nodes.values() if n["op"] == opname]
    if not found:
        print(f"No nodes with op '{opname}'")
        return
    for n in found:
        print(fmt_node(n))
        for slot, inp in enumerate(n.get("inputs", [])):
            label = ""
            if opname == "MUX":
                label = ["sel", "T", "F"][slot] if slot < 3 else str(slot)
            elif opname == "INDEX":
                label = ["src", "hi", "lo"][slot] if slot < 3 else str(slot)
            else:
                label = str(slot)
            print(f"    [{label}] {fmt_node(nodes[inp])}")


def cmd_fanout(nodes, by_name, consumers, args):
    nid = resolve_name(args[0], nodes, by_name)
    print(f"Fanout of: {fmt_node(nodes[nid])}")
    fan = consumers[nid]
    if not fan:
        print("  (no consumers)")
        return
    for cid, slot in fan:
        print(f"  slot {slot} of {fmt_node(nodes[cid])}")


def _mux_tree(nid, nodes, visited, depth):
    if nid in visited:
        return
    visited.add(nid)
    n = nodes[nid]
    indent = "  " * depth
    if n["op"] == "MUX":
        print(f"{indent}MUX [{n['id']}] @ {n.get('loc','?')}")
        inps = n.get("inputs", [])
        if len(inps) >= 1:
            sel = nodes[inps[0]]
            print(f"{indent}  sel: {fmt_node(sel)}")
        if len(inps) >= 2:
            print(f"{indent}  T:")
            _mux_tree(inps[1], nodes, visited, depth + 2)
        if len(inps) >= 3:
            print(f"{indent}  F:")
            _mux_tree(inps[2], nodes, visited, depth + 2)
    else:
        print(f"{indent}{fmt_node(n)}")


def cmd_mux(nodes, by_name, consumers, args):
    nid = resolve_name(args[0], nodes, by_name)
    n = nodes[nid]
    inps = n.get("inputs", [])
    if not inps:
        print(f"{fmt_node(n)}: (no driver)")
        return
    print(f"MUX tree for: {fmt_node(n)}")
    _mux_tree(inps[0], nodes, set(), 1)


def cmd_inputs(nodes, by_name, consumers, args):
    for n in nodes.values():
        if n["op"] == "INPUT":
            print(fmt_node(n))


def cmd_outputs(nodes, by_name, consumers, args):
    for n in nodes.values():
        if n["op"] == "OUTPUT":
            inps = n.get("inputs", [])
            driver = fmt_node(nodes[inps[0]]) if inps else "(no driver)"
            print(f"{fmt_node(n)}")
            print(f"    <- {driver}")


def cmd_signals(nodes, by_name, consumers, args):
    for n in nodes.values():
        if n["op"] == "SIGNAL":
            inps = n.get("inputs", [])
            if len(inps) == 1:
                driver = fmt_node(nodes[inps[0]])
                print(f"{fmt_node(n)}")
                print(f"    <- {driver}")
            elif len(inps) == 0:
                print(f"{fmt_node(n)}  (undriven)")
            else:
                print(f"{fmt_node(n)}  ({len(inps)} inputs — aggregate)")


def cmd_undriven(nodes, by_name, consumers, args):
    found = False
    for n in nodes.values():
        if n["op"] in ("SIGNAL", "OUTPUT") and not n.get("inputs"):
            print(fmt_node(n))
            found = True
    if not found:
        print("All signals and outputs are driven.")


def cmd_diff(nodes, by_name, consumers, args):
    if not args:
        sys.exit("diff requires a second JSON file")
    nodes2, by_name2, _ = load(args[0])

    # Compare drivers of all named SIGNAL/OUTPUT nodes present in both
    all_names = set(by_name.keys()) | set(by_name2.keys())
    for name in sorted(all_names):
        id1 = by_name.get(name)
        id2 = by_name2.get(name)
        if id1 is None or id2 is None:
            status = "only in file1" if id2 is None else "only in file2"
            print(f"  {name}: {status}")
            continue
        n1 = nodes[id1]
        n2 = nodes2[id2]
        inps1 = n1.get("inputs", [])
        inps2 = n2.get("inputs", [])
        d1 = fmt_node(nodes[inps1[0]]) if inps1 else "(undriven)"
        d2 = fmt_node(nodes2[inps2[0]]) if inps2 else "(undriven)"
        # Compare by op+name+value of driver, not id (ids shift between files)
        def driver_sig(n):
            return (n["op"], n.get("name", ""), n.get("value", None))
        if not inps1 and not inps2:
            continue
        sig1 = driver_sig(nodes[inps1[0]]) if inps1 else None
        sig2 = driver_sig(nodes2[inps2[0]]) if inps2 else None
        if sig1 != sig2:
            print(f"CHANGED  {name}")
            print(f"  file1 driver: {d1}")
            print(f"  file2 driver: {d2}")


def cmd_path(nodes, by_name, consumers, args):
    if len(args) < 2:
        sys.exit("path requires two node ids")
    src = resolve_name(args[0], nodes, by_name)
    dst = resolve_name(args[1], nodes, by_name)
    # BFS forward from src through inputs (backward in dataflow = toward inputs)
    # Use fanout (consumers) for forward dataflow
    visited = set()
    queue = [([src], src)]
    while queue:
        path, cur = queue.pop(0)
        if cur == dst:
            print("Path found:")
            for nid in path:
                print(f"  {fmt_node(nodes[nid])}")
            return
        if cur in visited:
            continue
        visited.add(cur)
        for cid, _ in consumers[cur]:
            queue.append((path + [cid], cid))
    print(f"No path from {src} to {dst}")


# ── main ──────────────────────────────────────────────────────────────────────

COMMANDS = {
    "driver":   cmd_driver,
    "cone":     cmd_cone,
    "node":     cmd_node,
    "op":       cmd_op,
    "fanout":   cmd_fanout,
    "mux":      cmd_mux,
    "inputs":   cmd_inputs,
    "outputs":  cmd_outputs,
    "signals":  cmd_signals,
    "undriven": cmd_undriven,
    "diff":     cmd_diff,
    "path":     cmd_path,
}

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(0)

    json_file = sys.argv[1]
    command = sys.argv[2]
    rest = sys.argv[3:]

    if command not in COMMANDS:
        sys.exit(f"Unknown command '{command}'. Available: {', '.join(COMMANDS)}")

    nodes, by_name, consumers = load(json_file)
    COMMANDS[command](nodes, by_name, consumers, rest)
