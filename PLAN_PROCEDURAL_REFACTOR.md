# Plan: Refactor procedural-block compilation (elaboration.cpp)

Status: **Phase 1 complete; Phase 2a complete** (2026-07-08). Next: Phase 2b
(single slice-tree representation for retained/partial state) and 2c
(child-env branch elaboration + env join) — best done together in a fresh
session.
Anchor: clean `python tests/regression.py --mode verilator-dpi` run before and
after every extraction step.

Note: `PLAN.md` in this repo is the (completed) DPI Model ABI plan — an
unrelated effort. This file is the procedural-compilation refactor plan.

## Problem statement

The lowering of procedural blocks (`always` / `always_comb` / `always_ff`)
into the DFG is a recurring source of bugs and is hard to modify safely.
All of it lives in `src/frontends/systemverilog/passes/elaboration.cpp`
(~10k lines, single translation unit, public surface = two `resolveModules`
overloads in `elaboration.h`).

The intended model is simple: every assignment's RHS is a DFG with one
output; cascaded assignments in a block are merged so that ordering
semantics become mux structure (e.g. `x = 0; if (c) x = 42;` ≡
`if (c) x = 42; else x = 0;`). The implementation of that model is where
the pain is.

## Root-cause analysis (from full code read, 2026-07-08)

### Core architectural defect

Procedural blocks are elaborated by **mutating the final, shared DFG in
place**, and conditionals are handled by an
**execute → diff → rollback → re-execute → merge** protocol over that live
graph (`resolveConditionalStatementInPlace`, `resolveCaseStatementInPlace`).
"Current value of each target at this point in the block" has no
representation of its own; it is smeared across four stores that every
write site and merge site must keep in sync manually:

1. The real driver edge on the DFG node (`connectDriver`).
2. `ctx.combDrivers` — read-through map for comb blocks (also overloaded as
   storage for subroutine locals and as the function return-value channel).
3. `ctx.partial_drivers` — slice-level state for range writes, additionally
   materialized back into store 1 as a CONCAT after every update
   (`materializePartialTarget`).
4. `DriverSnapshot.visibleDrivers` — a copy taken at every if/case by
   walking every driven node in the module (`snapshotDrivers`).

### Concrete failure modes this causes

- The sync ritual (`recordFullWrite` + `connectNode` +
  `if (!is_sequential) combDrivers[...] = ...`) is repeated ~8 times inside
  `resolveAssignExpression`; missing one spot is a silent wrong-graph bug.
  A past bug of exactly this class is documented in a comment near the
  whole-write-over-partial-state path in `resolveAssignExpression`
  ("the isPackedAggregateTarget check was too narrow ... stale partial that
  restoreDrivers re-materialises").
- Branch diffing is pointer-identity guesswork (`modifiedDriversSince`,
  `partialStatesEqual`): "was assigned in this branch" is reverse-engineered
  from graph state instead of recorded at write time.
- `restoreDrivers` is not the inverse of execution: it re-materializes
  partial states, minting fresh SLICE/CONCAT nodes per restore, leaving
  abandoned nodes for DCE and undermining the pointer comparisons the
  diffing relies on.
- `makeWholeDriverState` un-lowers the graph: it pattern-matches CONCAT
  drivers to reconstruct slice structure that was known (and discarded) at
  write time.
- No exception safety: a `CompilerError` mid-branch escapes with the shared
  graph half-mutated (`executeConditionalBranch` catches only
  `ReturnValue`).
- Stringly-typed target identity (`"base[3].field.d"`); the `.d`/`.q`
  strip-and-swap is re-implemented in at least 5 places
  (`getRetainedDriver`, `lookupTargetTypeOrThrow`, `buildMergedDriver`,
  `mergeCaseBranches`, `currentWholeDriverForTarget`, dynamic-bit path);
  `canonicalTargetKey` (generate-scope prefixing) applies to `write_states`
  but not to `partial_drivers`/`combDrivers`.
- Near-duplicate helpers with subtly different lookup orders:
  `lookupLeafNode` / `lookupTargetNode` / `lookupNamedNodeInModule` /
  `lookupDrivenNodeInModule`; module-level `connectDriver` vs the
  `connectNode` lambda in `resolveAssignExpression`; `getRetainedDriver` vs
  `currentWholeDriverForTarget` (these two also encode two different
  priority orders for ".d driver vs .q" reads in sequential blocks).
- `mergeIfBranches` / `mergeCaseBranches` duplicate the
  retained/fallback/partial logic (plus two identical
  `collectAssignedSignals` overloads; `mergeCaseBranches` fakes a
  `ConditionalBranch{nullptr, ...}` to reuse a helper); case lowering
  enumerates all 2^selector-width codes (guarded at width 63, but memory is
  2^width per assigned signal).
- One `ResolutionContext` god-struct is shared across all procedural
  blocks, continuous assigns, generate scopes, and submodule hookup
  (see the member loops at the bottom of `resolveModule`); block-scoped and
  module-scoped state are commingled, with manual `combDrivers.clear()` at
  some (not all) boundaries and `partial_drivers`/`write_states` persisting
  module-wide.

### File-size problem

`elaboration.cpp` is ~6 components in one TU:

1. Constant evaluation (`evaluateConstantExpr`/`evaluateConstantValue`,
   casez patterns, string-literal lowering, width computation).
2. Type & registry resolution (`resolveType`, `ResolveDimensions`,
   struct/enum/typedef/package registries).
3. Expression → DFG building (`buildExprValue`/`buildExprDFG`, selectors,
   aggregates, assignment patterns).
4. Procedural statement elaboration + driver/merge machinery.
5. Hierarchy & instantiation (ports, interfaces, submodules,
   pre-population).
6. Generate blocks + the `resolveModule` orchestrator.

Symptoms of single-TU pressure: mid-file forward decls (`evaluateStepExpr`),
duplicate declarations (`collectNBAFromStatement` declared twice), helpers
duplicated because siblings are thousands of lines away, the partial-driver
machinery scattered across three stretches of the file.

## Target design

### Block environment (the actual fix)

Give the block elaborator a **value environment**: a per-block map
`TargetRef → value`, where the value is a slice-tree that natively covers
whole and partial writes. Rules:

- Assignments update only the environment. The real DFG nodes are not
  touched while elaborating a block.
- Branches (`if`/`case` arms) elaborate into child environments; merging is
  a pure environment join that inserts muxes. No snapshot, no diff, no
  rollback, no re-materialization.
- Reads resolve through the environment first, falling back to the bound
  node (`.q` for flops in sequential blocks) — one query path, one place
  encoding blocking-read semantics.
- When the block finishes, a single commit function connects the final
  environment to module nodes (`connectDriver`), runs fully-driven
  validation for partial targets, and records write origins / multi-driver
  checks.
- `TargetRef` is a real type (base name, selectors, field path, d/q role),
  replacing string keys and the suffix parsers.

This deletes stores 2–4 above, collapses the two merge functions into one
env-join, turns the 8-fold sync ritual into "write the env", and gives
exception safety for free (abandoned child envs are just dropped).

### File split

- `constant_eval.{h,cpp}` — component 1.
- `type_resolve.{h,cpp}` — component 2.
- `expr_build.{h,cpp}` — component 3.
- `procedural_elaboration.{h,cpp}` + `block_env.h` — component 4, written
  new in Phase 2 (old machinery deleted, not moved).
- Components 5–6 extracted opportunistically later.
- `ResolutionContext` splits into a read-only module environment
  (registries, params, interface views, module pointer) and mutable
  block/elaboration state; shared definitions move to an internal header
  (e.g. `elaboration_internal.h`, not part of the public surface).

## Why this order (split first, refactor second)

- Phase 1 is mechanical and verifiable per step (DPI regression must stay
  clean after each extraction); it shrinks the blast radius for Phase 2.
- The `ResolutionContext` split forced by Phase 1 *is* the first concrete
  step of the block-env design (separating module facts from block state).
- Refactoring first inside the monolith would bury the redesign in
  unrelated code and churn every new line twice when the split happens.
- Splitting the driver machinery as-is (without the refactor) would
  enshrine the four-store design behind module boundaries — hence Phase 2
  writes the new component fresh instead of moving old code.

## Phases

### Phase 1 — extract leaf components (mechanical; IN PROGRESS)

Steps (regression-clean after each, one commit each):

1. DONE — Baseline: 151/151 verilator-dpi on untouched tree (commit 8cf899d).
2. DONE — `elaboration_internal.h` created; after step 5 it holds the shared
   registries, `PartialSliceDriver`/`PartialTargetState`/`PartialDriverMap`,
   `IfacePortView`/`IfaceInstanceView`, `ResolutionContext`, the inline
   interface-view helpers, `ExprValue`, `ReturnValue`, and cross-TU decls
   (`resolveStatementInPlace`, `inlineSubroutineCall`, `connectDriver`,
   `lookupNamedNodeInModule`).
3. DONE — constant_eval.{h,cpp} extracted (commit 801f4ec, 151/151).
   extractPortExpr/isPackageScopedName moved to syntax_helpers.
4. DONE — type_resolve.{h,cpp} extracted (commit 28c1684, 151/151).
   getFuncName moved to syntax_helpers.
5. DONE — expr_build.{h,cpp} extracted (~2800 lines): expression→DFG
   building, selectors, aggregates, assignment patterns, coercion, name/type
   lookup (resolveIdentifier, lookupDeclaredType[WithSuffix], lookupLeafNode,
   aggregate leaf lookups). The anonymous namespace in elaboration.cpp was
   removed (its non-static functions have unique names; statics stay
   static). elaboration.cpp is now ~5,450 lines and contains procedural
   statement elaboration + driver machinery, hierarchy/instantiation,
   generate handling, and the resolveModule orchestrator.
   Note: `isPackedAggregateTarget` in expr_build.cpp is pre-existing dead
   code (only a comment references it) — delete during Phase 2.
6. DONE — DPI regression after step 5: 151/151 (commit e75ef31).
   Secondary `validate`-mode regression: 147/151 in the parallel run;
   3 of the 4 failures (func_test, cdc_sidecar_cross_clock_pass, cordic)
   were a build race on the shared vcd-compare tool ("Text file busy" /
   "Permission denied" while parallel tests compiled it concurrently) and
   pass when re-run; the 4th, `global_domains_opposite_edges`, fails
   identically on the untouched baseline snapshot (verified in a worktree
   at 8cf899d): "Mate ABI: inactive edge on clock domain 'clk' cannot
   sample inputs" in custom-sim. PRE-EXISTING, out of scope — not fixed.

**Phase 1 result:** elaboration.cpp 10,014 → 5,463 lines. New TUs:
constant_eval.cpp (1,002), type_resolve.cpp (488), expr_build.cpp (2,939),
plus elaboration_internal.h (ResolutionContext & shared types),
constant_eval.h, type_resolve.h, expr_build.h; extractPortExpr,
isPackageScopedName, getFuncName moved to syntax_helpers. DPI regression
151/151 at every step. Phase 1 complete.

Constraints:
- Pure code motion: no behavior changes, no renames beyond what linkage
  requires, no drive-by cleanups (per project working rules).
- Keep `elaboration.h`'s public surface unchanged.

### Phase 2 — block-environment refactor of procedural compilation

Executed as regression-clean subphases (DPI 151/151 after each):

**2a (DONE, commit 483e80c) — block environment replaces the DFG-as-scratch.**
Verified: verilator-dpi 151/151; validate mode 150/151 (sole failure is the
pre-existing `global_domains_opposite_edges`, see Verification anchor).
`ResolutionContext` gains `in_procedural_block` + `block_drivers`
(target name → driver node). While a procedural block elaborates, all
target writes (`connectDriver`, the `connectNode` lambda, partial-state
materialization, subroutine output copy-back) land in the environment; the
shared DFG is untouched until `commitBlockDrivers` connects the final
environment once at block end. Mid-block driver reads go through
`envDriver` (env first, then committed node driver). Branch baselines
(`DriverSnapshot`) become plain map copies of {block_drivers,
partial_drivers, combDrivers}; `restoreDrivers` is a map assignment — no
graph clearing, no re-materialization, no module-wide node walks
(`forEachVisibleDriverTarget` and `clearVisibleDrivers` deleted).
For-loop per-iteration contexts and `inlineSubroutineCall` sub-contexts
propagate the environment in and back out. This kills: the
restore-re-materialization node churn, the graph-corruption-on-throw
hazard, and the whole-module snapshot walks. Still present (later
subphases): string target keys, combDrivers as separate read cache,
execute/diff/merge branch protocol, `makeWholeDriverState` un-lowering,
2^width case enumeration.

**2b (DONE) — retained-state without un-lowering.**
`makeWholeDriverState` no longer decomposes CONCAT drivers to reconstruct
slice structure from the lowered graph; a retained whole driver is one
full-range slice and sub-interval reads slice into it. (Deferred from the
original 2b scope: `partialStatesEqual` still backs
`modifiedPartialDriversSince`; replacing the diff with direct
assigned-in-branch recording folds into 2d/2e.)

**2c (DONE) — one merge engine for if and case.**
`BranchDelta` (whole writes + partial state per arm), shared
`collectAssignedSignals` / `isPartialMergeTarget` /
`applyMergedPartialTarget` / `applyMergedWholeTarget`, and a
`forEachMergedTarget` walk; `mergeIfBranches` / `mergeCaseBranches` are
thin adapters providing only the shape-specific combiners (mux chain vs
selector table). `stripDQSuffix` introduced (partial 2d). The
2^selector-width enumeration remains, contained in `mergeCaseBranches`'
combiners; removing it means changing MUX lowering and is its own effort.
2b+2c verified: verilator-dpi 151/151.

**2d (DONE, revised scope) — target-name construction/parsing centralized.**
Original idea was a typed `TargetRef` replacing string keys everywhere.
Revised: the string names *are* the module's canonical leaf addressing
(bindings, flop leaves), so a wrapper type would add churn without deleting
complexity. What actually hurt was scattered construction/parsing, now
centralized: `stripDQSuffix` (2c), `qNameForTarget` +
`lookupSequentialQNode` (the three inline `.d`→`.q` swap/lookup copies),
`seqWriteName` (the five inline "flop check + `.d` append" copies), and
`writeTarget` (the repeated recordFullWrite + connectNode + comb-cache
ritual — the "miss one sync spot" bug class from the original analysis).
`resolveAssignExpression`'s write sites now each construct the name once
and call one write path. Revisit a real `TargetRef` only if
`canonicalTargetKey` inconsistency (write_states vs other maps) produces
an actual bug.

**2e (DONE, revised scope) — dead code removed; subroutine env deferred.**
Dead `isPackedAggregateTarget` deleted (build is warning-clean). Moving
subroutine locals off `combDrivers` into a separate map was assessed and
deferred: it relocates the special cases (the `subroutine_locals`
membership checks remain to decide routing) without removing them, and
`writeTarget` already hides the local-vs-module routing behind one call.
The real fix is a scoped value environment pushed per function inline —
worth doing only alongside a rework of `inlineSubroutineCall`.

### Phase 3 — optional extractions

Hierarchy/instantiation, generate handling, orchestrator — extract when
next touched; not blocking.

## Verification anchor

`scripts/docker-run.sh python tests/regression.py --mode verilator-dpi`,
judged by the `PASS: 100% match` sentinel per project rules (not the make
exit code). Default `validate`-mode regression as secondary check. Any
pre-existing failures found in the baseline run are recorded below and
treated as out of scope (compare against baseline; do not fix unrelated
failures without asking).

Baseline (2026-07-08, commit 8cf899d): **151/151 passed**
(`make dev` + `python tests/regression.py --mode verilator-dpi` via
`scripts/docker-run.sh`). No pre-existing failures.

## Repo state note (2026-07-08)

This checkout was an orphaned git worktree; its parent repo
(`~/dev/asic/compiler`) no longer exists on this machine. A fresh git repo
was initialized in place on branch `refactor-procedural-compilation` with a
baseline snapshot commit (8cf899d). The old worktree pointer is preserved
as `.git.broken-worktree-pointer` in case the original repo is restored and
history needs re-linking.
