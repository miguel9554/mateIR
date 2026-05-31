---
name: read-hierarchy
description: Inspect hierarchy.json produced by the compiler. Use this when debugging module structure, ports, flops, clock or reset domain usage, or finding a signal or flop across hierarchy. Prefer tools/hierarchy_inspect.py.
---

Use `tools/hierarchy_inspect.py`.

Location:
- `tests/<name>/work/*/debug_output/<top>/hierarchy.json`

Useful commands:
- `python3 tools/hierarchy_inspect.py <file> tree`
- `python3 tools/hierarchy_inspect.py <file> module <path>`
- `python3 tools/hierarchy_inspect.py <file> flops <path>`
- `python3 tools/hierarchy_inspect.py <file> inputs <path>`
- `python3 tools/hierarchy_inspect.py <file> outputs <path>`
- `python3 tools/hierarchy_inspect.py <file> signals <path>`
- `python3 tools/hierarchy_inspect.py <file> find <name>`
- `python3 tools/hierarchy_inspect.py <file> clocks`
- `python3 tools/hierarchy_inspect.py <file> stats`

Path rules:
- use dot-separated instance paths
- the root name is optional

Use this skill before reading raw hierarchy JSON directly.
