#!/usr/bin/env python3
"""
DFG JSON inspection tool.

Usage:
  dfg_inspect.py <json_file> <command> [args...]

Commands:
  node         <id-or-name> [--details]
  nodes        [--op OP] [--name PATTERN] [--at FILE:LINE]
  uses         <id-or-name> [--op OP] [--depth N] [--details]
  deps         <id-or-name> [--op OP] [--depth N] [--details]
  neighborhood <id-or-name> --fanin N --fanout N
  driver       <name>
  cone         <name> [--depth N]
  fanout       <id-or-name>
  mux          <name>
  mux-case     <id-or-name> <case_val>
  inputs
  outputs
  signals
  undriven
  const_driven [value]
  group        <prefix>
  search       <pattern>
  diff         <json2> [--node ID-OR-NAME] [--cone] [--details]
  path         <id1> <id2>
"""

import json
import sys


def load(path):
    with open(path) as f:
        data = json.load(f)

    nodes = {n["id"]: n for n in data["nodes"]}
    by_name = {}
    by_debug_id = {}

    def register_name(name, node_id):
        if not name:
            return
        if name not in by_name or nodes[node_id]["op"] in ("SIGNAL", "OUTPUT", "INPUT"):
            by_name[name] = node_id

    for n in data["nodes"]:
        register_name(n.get("name"), n["id"])
        register_name(n.get("full_name"), n["id"])
        if "debug_id" in n:
            by_debug_id.setdefault(n["debug_id"], []).append(n["id"])

    consumers = {n["id"]: [] for n in data["nodes"]}
    for n in data["nodes"]:
        for slot, inp in enumerate(n.get("inputs", [])):
            ref = normalize_input_ref(inp, slot)
            consumers[ref["node"]].append({
                "consumer": n["id"],
                "slot": slot,
                "role": ref["role"],
                "port": ref.get("port", 0),
            })

    return {
        "nodes": nodes,
        "by_name": by_name,
        "by_debug_id": by_debug_id,
        "consumers": consumers,
    }


def normalize_input_ref(inp, slot=None):
    if isinstance(inp, int):
        return {"node": inp, "role": str(slot if slot is not None else 0), "port": 0}
    out = dict(inp)
    out.setdefault("role", str(slot if slot is not None else 0))
    out.setdefault("port", 0)
    return out


def node_inputs(node):
    return [normalize_input_ref(inp, idx) for idx, inp in enumerate(node.get("inputs", []))]


def fmt_dims(dims):
    if not dims:
        return "[]"
    return "[" + ", ".join(f"{{left={d['left']}, right={d['right']}}}" for d in dims) + "]"


def fmt_type(t):
    if not t:
        return "<?>"
    sign = "s" if t.get("signed") else "u"
    suffix = ""
    if t.get("packed_dims"):
        suffix += f", packed_dims={fmt_dims(t['packed_dims'])}"
    if t.get("unpacked_dims"):
        suffix += f", unpacked_dims={fmt_dims(t['unpacked_dims'])}"
    kind = t.get("kind")
    kind_part = f", kind={kind}" if kind else ""
    return f"[{t['width']}{sign}{kind_part}{suffix}]"


def fmt_node(node):
    parts = [f"[{node['id']}]", f"dbg={node.get('debug_id', '?')}", node["op"]]
    full_name = node.get("full_name")
    if full_name:
        parts.append(f"'{full_name}'")
    elif node.get("name"):
        parts.append(f"'{node['name']}'")
    if "value" in node:
        parts.append(f"= {node['value']}")
    if node.get("type"):
        parts.append(fmt_type(node["type"]))
    if node.get("loc"):
        parts.append(f"@ {node['loc']}")
    return "  ".join(parts)


def print_node_details(graph, node):
    print(fmt_node(node))
    if node.get("instance_path"):
        print(f"  instance_path: {node['instance_path']}")
    if node.get("type"):
        t = node["type"]
        print(
            "  type: "
            f"kind={t.get('kind', 'integer')} width={t['width']} signed={'true' if t.get('signed') else 'false'} "
            f"packed_dims={fmt_dims(t.get('packed_dims', []))} "
            f"unpacked_dims={fmt_dims(t.get('unpacked_dims', []))}"
        )
    inputs = node_inputs(node)
    if inputs:
        print("  inputs:")
        for ref in inputs:
            port = f" port={ref['port']}" if ref.get("port", 0) else ""
            print(f"    [{ref['role']}] {fmt_node(graph['nodes'][ref['node']])}{port}")
    consumers = graph["consumers"].get(node["id"], [])
    if consumers:
        print("  consumed by:")
        for use in consumers:
            port = f" port={use['port']}" if use.get("port", 0) else ""
            print(
                f"    [{use['role']}] {fmt_node(graph['nodes'][use['consumer']])}{port}"
            )


def resolve_name(name_or_id, graph):
    nodes = graph["nodes"]
    by_name = graph["by_name"]
    by_debug_id = graph["by_debug_id"]

    try:
        num = int(name_or_id)
    except ValueError:
        num = None

    if num is not None:
        if num in nodes:
            return num
        matches = by_debug_id.get(num, [])
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            sys.exit(f"Ambiguous debug_id {num}: node ids {matches}")
        sys.exit(f"No node with id/debug_id {num}")

    if name_or_id not in by_name:
        candidates = sorted(
            name for name in by_name.keys()
            if name_or_id.lower() in name.lower()
        )
        if candidates:
            sys.exit(
                f"No exact node match for '{name_or_id}'. Candidates:\n" +
                "\n".join(f"  {name}" for name in candidates[:12])
            )
        sys.exit(f"No node named '{name_or_id}'")
    return by_name[name_or_id]


def consume_flag(args, flag, has_value=False, default=None):
    if flag not in args:
        return default, args
    idx = args.index(flag)
    if has_value:
        if idx + 1 >= len(args):
            sys.exit(f"{flag} requires a value")
        value = args[idx + 1]
        rest = args[:idx] + args[idx + 2:]
        return value, rest
    rest = args[:idx] + args[idx + 1:]
    return True, rest


def filter_nodes(graph, op=None, name_pattern=None, loc_pattern=None):
    result = list(graph["nodes"].values())
    if op:
        result = [n for n in result if n["op"] == op.upper()]
    if name_pattern:
        pat = name_pattern.lower()
        result = [
            n for n in result
            if pat in n.get("name", "").lower() or pat in n.get("full_name", "").lower()
        ]
    if loc_pattern:
        result = [n for n in result if loc_pattern in n.get("loc", "")]
    return sorted(result, key=lambda n: n["id"])


def cmd_node(graph, args):
    details, args = consume_flag(args, "--details", has_value=False, default=False)
    if not args:
        sys.exit("node requires <id-or-name>")
    node = graph["nodes"][resolve_name(args[0], graph)]
    if details:
        print_node_details(graph, node)
    else:
        print(fmt_node(node))


def cmd_nodes(graph, args):
    op, args = consume_flag(args, "--op", has_value=True)
    name_pat, args = consume_flag(args, "--name", has_value=True)
    loc_pat, args = consume_flag(args, "--at", has_value=True)
    if args:
        sys.exit(f"Unexpected trailing arguments: {' '.join(args)}")
    found = filter_nodes(graph, op=op, name_pattern=name_pat, loc_pattern=loc_pat)
    if not found:
        print("No matching nodes.")
        return
    for node in found:
        print(fmt_node(node))


def _walk_deps(graph, nid, visited, depth, max_depth, op_filter, details):
    if nid in visited or depth > max_depth:
        return
    visited.add(nid)
    node = graph["nodes"][nid]
    indent = "  " * depth
    print(f"{indent}{fmt_node(node)}")
    if details and node.get("inputs"):
        for ref in node_inputs(node):
            child = graph["nodes"][ref["node"]]
            if op_filter and child["op"] != op_filter:
                continue
            print(f"{indent}  [{ref['role']}] {fmt_node(child)}")
    for ref in node_inputs(node):
        if op_filter and graph["nodes"][ref["node"]]["op"] != op_filter:
            continue
        _walk_deps(graph, ref["node"], visited, depth + 1, max_depth, op_filter, details)


def cmd_deps(graph, args):
    op, args = consume_flag(args, "--op", has_value=True)
    depth, args = consume_flag(args, "--depth", has_value=True, default="30")
    details, args = consume_flag(args, "--details", has_value=False, default=False)
    if not args:
        sys.exit("deps requires <id-or-name>")
    nid = resolve_name(args[0], graph)
    _walk_deps(graph, nid, set(), 0, int(depth), op.upper() if op else None, details)


def _walk_uses(graph, nid, visited, depth, max_depth, op_filter, details):
    if nid in visited or depth > max_depth:
        return
    visited.add(nid)
    node = graph["nodes"][nid]
    indent = "  " * depth
    print(f"{indent}{fmt_node(node)}")
    for use in graph["consumers"].get(nid, []):
        consumer = graph["nodes"][use["consumer"]]
        if op_filter and consumer["op"] != op_filter:
            continue
        print(f"{indent}  [{use['role']}] {fmt_node(consumer)}")
        if details:
            for ref in node_inputs(consumer):
                print(f"{indent}    [{ref['role']}] {fmt_node(graph['nodes'][ref['node']])}")
        _walk_uses(graph, consumer["id"], visited, depth + 1, max_depth, op_filter, details)


def cmd_uses(graph, args):
    op, args = consume_flag(args, "--op", has_value=True)
    depth, args = consume_flag(args, "--depth", has_value=True, default="30")
    details, args = consume_flag(args, "--details", has_value=False, default=False)
    if not args:
        sys.exit("uses requires <id-or-name>")
    nid = resolve_name(args[0], graph)
    _walk_uses(graph, nid, set(), 0, int(depth), op.upper() if op else None, details)


def cmd_neighborhood(graph, args):
    fanin, args = consume_flag(args, "--fanin", has_value=True, default="1")
    fanout, args = consume_flag(args, "--fanout", has_value=True, default="1")
    if not args:
        sys.exit("neighborhood requires <id-or-name>")
    nid = resolve_name(args[0], graph)
    print("Fanin:")
    _walk_deps(graph, nid, set(), 0, int(fanin), None, details=False)
    print("Fanout:")
    _walk_uses(graph, nid, set(), 0, int(fanout), None, details=False)


def cmd_driver(graph, args):
    nid = resolve_name(args[0], graph)
    n = graph["nodes"][nid]
    print(f"Signal: {fmt_node(n)}")
    inputs = node_inputs(n)
    if not inputs:
        print("  (no driver)")
        return
    for ref in inputs:
        print(f"  driver[{ref['role']}]: {fmt_node(graph['nodes'][ref['node']])}")


def cmd_cone(graph, args):
    depth, args = consume_flag(args, "--depth", has_value=True, default="30")
    if len(args) > 1:
        depth = args[1]
        args = args[:1]
    nid = resolve_name(args[0], graph)
    _walk_deps(graph, nid, set(), 0, int(depth), None, details=False)


def cmd_fanout(graph, args):
    nid = resolve_name(args[0], graph)
    print(f"Fanout of: {fmt_node(graph['nodes'][nid])}")
    fan = graph["consumers"].get(nid, [])
    if not fan:
        print("  (no consumers)")
        return
    for use in fan:
        print(f"  [{use['role']}] {fmt_node(graph['nodes'][use['consumer']])}")


def _mux_tree(graph, nid, visited, depth):
    if nid in visited:
        return
    visited.add(nid)
    n = graph["nodes"][nid]
    indent = "  " * depth
    if n["op"] == "MUX":
        print(f"{indent}MUX [{n['id']}] dbg={n.get('debug_id', '?')} @ {n.get('loc', '?')}")
        for ref in node_inputs(n):
            child = graph["nodes"][ref["node"]]
            print(f"{indent}  {ref['role']}: {fmt_node(child)}")
    else:
        print(f"{indent}{fmt_node(n)}")


def cmd_mux(graph, args):
    nid = resolve_name(args[0], graph)
    n = graph["nodes"][nid]
    inputs = node_inputs(n)
    if not inputs:
        print(f"{fmt_node(n)}: (no driver)")
        return
    print(f"MUX tree for: {fmt_node(n)}")
    _mux_tree(graph, inputs[0]["node"], set(), 1)


def cmd_inputs(graph, _args):
    for n in graph["nodes"].values():
        if n["op"] == "INPUT":
            print(fmt_node(n))


def cmd_outputs(graph, _args):
    for n in graph["nodes"].values():
        if n["op"] == "OUTPUT":
            inputs = node_inputs(n)
            driver = fmt_node(graph["nodes"][inputs[0]["node"]]) if inputs else "(no driver)"
            print(fmt_node(n))
            print(f"    <- {driver}")


def cmd_signals(graph, _args):
    for n in graph["nodes"].values():
        if n["op"] == "SIGNAL":
            inputs = node_inputs(n)
            if len(inputs) == 1:
                print(fmt_node(n))
                print(f"    <- {fmt_node(graph['nodes'][inputs[0]['node']])}")
            elif not inputs:
                print(f"{fmt_node(n)}  (undriven)")
            else:
                print(f"{fmt_node(n)}  ({len(inputs)} inputs)")


def cmd_undriven(graph, _args):
    found = False
    for n in graph["nodes"].values():
        if n["op"] in ("SIGNAL", "OUTPUT") and not node_inputs(n):
            print(fmt_node(n))
            found = True
    if not found:
        print("All signals and outputs are driven.")


def cmd_const_driven(graph, args):
    filter_val = int(args[0], 0) if args else None
    found = False
    for n in graph["nodes"].values():
        if n["op"] not in ("SIGNAL", "OUTPUT"):
            continue
        inputs = node_inputs(n)
        if len(inputs) != 1:
            continue
        driver = graph["nodes"][inputs[0]["node"]]
        if driver["op"] != "CONST":
            continue
        if filter_val is not None and driver.get("value") != filter_val:
            continue
        print(fmt_node(n))
        print(f"    <- {fmt_node(driver)}")
        found = True
    if not found:
        print("No matching CONST-driven SIGNAL/OUTPUT nodes.")


def cmd_group(graph, args):
    if not args:
        sys.exit("group requires a prefix argument")
    prefix = args[0]
    matches = []
    for n in graph["nodes"].values():
        name = n.get("full_name") or n.get("name", "")
        if name.startswith(prefix) and n["op"] in ("SIGNAL", "OUTPUT", "INPUT"):
            matches.append(n)
    if not matches:
        print(f"No signals/outputs/inputs with prefix '{prefix}'")
        return
    matches.sort(key=lambda n: n.get("full_name") or n.get("name", ""))
    for n in matches:
        inputs = node_inputs(n)
        driver_str = fmt_node(graph["nodes"][inputs[0]["node"]]) if inputs else "(undriven)"
        print(f"{fmt_node(n)}")
        print(f"    <- {driver_str}")


def cmd_mux_case(graph, args):
    if len(args) < 2:
        sys.exit("mux-case requires <node_id> <case_value>")
    nid = resolve_name(args[0], graph)
    target = int(args[1], 0)
    n = graph["nodes"][nid]
    if n["op"] != "MUX":
        sys.exit(f"Node {nid} is {n['op']}, not MUX")
    selector_values = n.get("mux_selector_values", [])
    inputs = node_inputs(n)
    if not selector_values:
        sys.exit(f"MUX [{nid}] has no mux_selector_values")
    for i, value in enumerate(selector_values):
        if value == target:
            arm = graph["nodes"][inputs[i + 1]["node"]]
            print(f"MUX [{nid}] case {target} (0x{target:x}):")
            print(f"  selector: {fmt_node(graph['nodes'][inputs[0]['node']])}")
            print(f"  arm:      {fmt_node(arm)}")
            return
    print(f"MUX [{nid}] has no case {target} (0x{target:x}).")


def cmd_search(graph, args):
    if not args:
        sys.exit("search requires a pattern argument")
    pat = args[0].lower()
    matches = []
    for n in graph["nodes"].values():
        name = (n.get("full_name") or n.get("name") or "").lower()
        if pat in name and n["op"] in ("SIGNAL", "OUTPUT", "INPUT"):
            matches.append(n)
    if not matches:
        print(f"No nodes matching '{pat}'")
        return
    for n in sorted(matches, key=lambda n: n["id"]):
        print(fmt_node(n))


def node_signature(graph, node):
    return {
        "op": node["op"],
        "name": node.get("name"),
        "full_name": node.get("full_name"),
        "instance_path": node.get("instance_path"),
        "type": node.get("type"),
        "loc": node.get("loc"),
        "value": node.get("value"),
        "mux_selector_values": node.get("mux_selector_values"),
        "inputs": [
            {
                "role": ref["role"],
                "debug_id": graph["nodes"][ref["node"]].get("debug_id"),
                "name": graph["nodes"][ref["node"]].get("full_name") or graph["nodes"][ref["node"]].get("name"),
                "port": ref.get("port", 0),
            }
            for ref in node_inputs(node)
        ],
    }


def collect_backcone_debug_ids(graph, nid):
    out = set()
    queue = [nid]
    while queue:
        cur = queue.pop()
        node = graph["nodes"][cur]
        out.add(node.get("debug_id", cur))
        for ref in node_inputs(node):
            queue.append(ref["node"])
    return out


def cmd_diff(graph1, args):
    if not args:
        sys.exit("diff requires a second JSON file")
    graph2 = load(args[0])
    rest = args[1:]
    node_filter, rest = consume_flag(rest, "--node", has_value=True)
    details, rest = consume_flag(rest, "--details", has_value=False, default=False)
    cone, rest = consume_flag(rest, "--cone", has_value=False, default=False)
    if rest:
        sys.exit(f"Unexpected trailing arguments: {' '.join(rest)}")

    wanted = None
    if node_filter:
        nid = resolve_name(node_filter, graph1)
        if cone:
            wanted = collect_backcone_debug_ids(graph1, nid)
        else:
            wanted = {graph1["nodes"][nid].get("debug_id", nid)}

    second_by_debug = {
        n.get("debug_id", n["id"]): n for n in graph2["nodes"].values()
    }
    first_by_debug = {
        n.get("debug_id", n["id"]): n for n in graph1["nodes"].values()
    }
    all_keys = sorted(set(first_by_debug) | set(second_by_debug))

    changed = 0
    for key in all_keys:
        if wanted is not None and key not in wanted:
            continue
        n1 = first_by_debug.get(key)
        n2 = second_by_debug.get(key)
        if n1 is None:
            print(f"ADDED   {fmt_node(n2)}")
            changed += 1
            continue
        if n2 is None:
            print(f"REMOVED {fmt_node(n1)}")
            changed += 1
            continue
        sig1 = node_signature(graph1, n1)
        sig2 = node_signature(graph2, n2)
        if sig1 != sig2:
            changed += 1
            print(f"CHANGED dbg={key}")
            print(f"  before: {fmt_node(n1)}")
            print(f"  after:  {fmt_node(n2)}")
            if details:
                before_keys = set(sig1.keys())
                after_keys = set(sig2.keys())
                for field in sorted(before_keys | after_keys):
                    if sig1.get(field) != sig2.get(field):
                        print(f"    {field}:")
                        print(f"      before: {sig1.get(field)}")
                        print(f"      after:  {sig2.get(field)}")
    if changed == 0:
        print("No node changes found.")


def cmd_path(graph, args):
    if len(args) < 2:
        sys.exit("path requires two node ids/names")
    src = resolve_name(args[0], graph)
    dst = resolve_name(args[1], graph)
    visited = set()
    queue = [([src], src)]
    while queue:
        path, cur = queue.pop(0)
        if cur == dst:
            print("Path found:")
            for nid in path:
                print(f"  {fmt_node(graph['nodes'][nid])}")
            return
        if cur in visited:
            continue
        visited.add(cur)
        for use in graph["consumers"].get(cur, []):
            queue.append((path + [use["consumer"]], use["consumer"]))
    print(f"No path from {src} to {dst}")


COMMANDS = {
    "node": cmd_node,
    "nodes": cmd_nodes,
    "uses": cmd_uses,
    "deps": cmd_deps,
    "neighborhood": cmd_neighborhood,
    "driver": cmd_driver,
    "cone": cmd_cone,
    "fanout": cmd_fanout,
    "mux": cmd_mux,
    "mux-case": cmd_mux_case,
    "inputs": cmd_inputs,
    "outputs": cmd_outputs,
    "signals": cmd_signals,
    "undriven": cmd_undriven,
    "const_driven": cmd_const_driven,
    "group": cmd_group,
    "search": cmd_search,
    "diff": cmd_diff,
    "path": cmd_path,
}


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(0)

    graph = load(sys.argv[1])
    command = sys.argv[2]
    if command not in COMMANDS:
        sys.exit(f"Unknown command '{command}'. Available: {', '.join(COMMANDS)}")
    COMMANDS[command](graph, sys.argv[3:])
