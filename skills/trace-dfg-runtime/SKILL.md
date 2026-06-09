---
name: trace-dfg-runtime
description: Trace runtime DFG evaluation in custom simulation. Use this when static DFG artifacts are not enough and you need to observe how specific nodes, cones, or operation classes evaluated over time, including operation-specific decisions emitted into output/dfg_trace.jsonl.
---

Use this skill after `read-vcd` or `read-dfg` has already narrowed the problem to a signal, node, cone, or operation family.

Trace flags:
- `--trace-dfg-node <a,b,...>` traces specific node ids or names
- `--trace-dfg-cone <a,b,...>` traces the backward cones of specific node ids or names
- `--trace-dfg-op <OP1,OP2,...>` traces all nodes of selected DFG operation kinds

Trace artifact:
- `tests/<name>/work/custom-sim/output/dfg_trace.jsonl`

Typical workflow:
1. Reproduce the issue with `custom-sim`.
2. If you know the bad signal, resolve its node with `read-dfg` and trace that node first.
3. If the issue may come from an upstream computation, trace the cone instead of the single node.
4. If the bug appears tied to one lowering class, trace by op kind.
5. Read `dfg_trace.jsonl` and correlate `id`, `debug_id`, `name`, `op`, `value`, and `decisions` back to `read-dfg node --details`.

When to use each flag:
- `--trace-dfg-node`: smallest and best default when one signal or node is already known
- `--trace-dfg-cone`: best when a wrong value is observed but the bad producer is unknown
- `--trace-dfg-op`: best for broad audits of one lowering family, but easiest to overload with noise

Interpretation tips:
- Trace events are structured JSON lines, not ad hoc logs.
- `decisions` carries operation-specific runtime facts such as chosen MUX arm or resolved slice range.
- State and input-like nodes may appear as passive snapshots so you can correlate causes and effects in one timeline.
- Prefer narrowing the trace filter before adding more debug output.

Example command shape:
- `scripts/docker-run.sh make -C tests/<name>/work/custom-sim simulate SIM_BUILD_TARGET=debug EXTRA_ARGS='--trace-dfg-node foo.bar'`
- `scripts/docker-run.sh make -C tests/<name>/work/custom-sim simulate SIM_BUILD_TARGET=debug EXTRA_ARGS='--trace-dfg-cone foo.bar'`
- `scripts/docker-run.sh make -C tests/<name>/work/custom-sim simulate SIM_BUILD_TARGET=debug EXTRA_ARGS='--trace-dfg-op MUX,SLICE'`

Escalation rule:
- If the trace still looks wrong but the corresponding DFG JSON is structurally correct, the bug is likely in simulation semantics or shared op-contract logic rather than lowering shape alone.
