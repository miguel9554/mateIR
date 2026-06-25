---
name: multi-phase-plan
description: >
  Use this skill whenever the user wants to plan a large refactor, migration, or multi-step
  code change that will be executed across multiple agent sessions with fresh context.
  Triggers include "plan this in phases", "create a phase plan", "break this into agent phases",
  "I want to do this step by step across sessions", or any time the user describes a large change
  and asks Claude to structure it for incremental execution.
  Produces a PLAN.md and a get_prompt.py script in a plans/ folder.
---

# Multi-Phase Plan Skill

When a user wants to plan a large code change across multiple agent sessions, produce two artifacts in a `plans/` folder at the project root:

1. **`plans/PLAN.md`** — the full plan document
2. **`plans/get_prompt.py`** — a script to retrieve the prompt for any given phase

Each phase agent, once it finishes executing, will also write a **phase report** to `plans/reports/phase-<N>.md`. This is not something you create upfront — it's produced by the agent at the end of each phase. The phase prompt must instruct the agent to do this.

---

## Step 1: Understand the Change

Before writing anything, make sure you understand:
- What is being changed and why
- What the end state looks like
- Any constraints (don't break X, preserve Y, keep Z working throughout)
- How the codebase is currently structured (explore if needed)

Ask clarifying questions if the goal is ambiguous.

---

## Step 2: Design the Phases

Break the work into phases such that:
- **Each phase is self-contained**: an agent with only PLAN.md and the phase prompt can execute it without needing prior session context
- **Phases are ordered**: later phases can depend on earlier ones being complete, but not on shared memory
- **Each phase has a clear completion criteria**: the agent knows when it's done
- **Phases are reasonably sized**: not too large to do in one session, not so small they're trivial

---

## Step 3: Write PLAN.md

PLAN.md is the shared reference document that every phase agent will read first. It must contain enough detail that a fresh agent — with no memory of prior sessions or the original conversation — can fully understand the project, the decisions made, and their phase's role in the whole.

**Do not write thin phase entries, but do not write implementation plans either.** The fresh agent that executes each phase will open in plan mode and figure out the concrete steps itself. Your job in PLAN.md is to give it enough *intent and context* that it plans correctly — not to pre-decide the how.

Write as much as needed about *what* and *why*. Be sparing about *how* — only include implementation hints when there's a specific decision that was already made and must be respected (e.g. "use the adapter pattern here, not a direct rewrite"), or when there's a non-obvious constraint the agent would otherwise miss.

```markdown
# Plan: <Plan Name>

## Overview
<What is being done and why. Include enough context that someone unfamiliar with
the codebase understands the motivation and the approach.>

## End State
<What the codebase will look like when all phases are complete. Be concrete:
mention new patterns, key interfaces, what gets deleted, what changes shape.>

## Constraints
<Anything that must remain true throughout all phases: no breaking API changes,
tests must pass after each phase, feature flags to use, etc.>

## Key Decisions
<Architectural or approach decisions made during planning, and why. This prevents
future agents from re-litigating choices that were already thought through.>

## Phases

### Phase 1: <Phase Name>
**Intent**: <What this phase is trying to achieve and why it comes first.>

**Expected outcome**: <What will be true when this phase is done — observable,
verifiable state of the codebase. Not a list of steps, but the end condition.
E.g. "All route handlers delegate business logic to a service layer. The service
interfaces exist and are typed. No behavior has changed.">

**Scope**: <Which parts of the codebase this phase operates on. Helps the agent
know where to look and what's out of bounds for this phase.>

**Constraints for this phase**: <Anything the agent must not do, or must preserve,
within this phase specifically.>

**Notes**: <Non-obvious context, prior decisions to respect, known gotchas —
but not step-by-step instructions.>

### Phase 2: <Phase Name>
...

## Notes
<Cross-cutting concerns, shared utilities, anything that applies across phases.>
```

---

## Step 4: Write get_prompt.py

The script takes two arguments: the plan name and the phase number. It prints the full prompt for that phase to stdout.

Every phase prompt must:
1. Instruct the agent to read `plans/PLAN.md` first
2. Describe the phase intent and expected outcome
3. Tell the agent to explore before planning, plan before executing
4. Instruct the agent to write a phase report to `plans/reports/phase-<N>.md` when done

```python
#!/usr/bin/env python3
"""
Usage: python get_prompt.py <plan_name> <phase_number>
Prints the agent prompt for the given phase of the given plan.
"""

import sys

PLANS = {
    "<plan_name>": {
        1: """You are executing Phase 1 of the "<Plan Name>" plan.

Read plans/PLAN.md first to understand the full context, goals, constraints, and
how this phase fits into the whole.

## Your Task: <Phase Name>

<Intent and expected outcome for this phase, drawn from PLAN.md. Written as a
clear goal, not a list of steps.>

## Approach

Start by exploring the codebase to understand the current state. Then produce a
concrete plan for this phase before making any changes. Once you're confident in
the plan, execute it.

## Scope
<Which parts of the codebase are in play for this phase.>

## Done When
<The verifiable end condition for this phase.>

## Constraints
<Anything that must be preserved or avoided in this phase.>

## Phase Report

When you are done, write a brief report to `plans/reports/phase-1.md` with the following structure:

```markdown
# Phase 1 Report: <Phase Name>

## Outcome
<Did the phase complete as expected? One short paragraph.>

## Deviations from Plan
<Any meaningful differences between what PLAN.md described and what was actually
done — different files touched, different approach taken, scope that grew or
shrank, decisions that had to be made on the fly. If everything went as planned,
write "None.">

## Implications for Remaining Phases
<Anything the next phase agent should know as a result of how this phase went —
e.g. "The UserService interface ended up needing an extra method; phase 2 should
account for this." If none, write "None.">
```

This report is the handoff to the next session. Be honest and specific — it exists
to keep the high-level plan accurate as reality unfolds.
""",
        2: """You are executing Phase 2 of the "<Plan Name>" plan.

Read plans/PLAN.md first to understand the full context, goals, constraints, and
how this phase fits into the whole.

Also read plans/reports/phase-1.md to understand any deviations or implications
from the previous phase before you begin.

## Your Task: <Phase Name>

...

## Phase Report

When you are done, write a brief report to `plans/reports/phase-2.md` covering:
- Outcome (did the phase complete as expected?)
- Deviations from the plan in PLAN.md (if any)
- Implications for remaining phases (if any)
""",
    }
}

def main():
    if len(sys.argv) != 3:
        print("Usage: python get_prompt.py <plan_name> <phase_number>", file=sys.stderr)
        sys.exit(1)

    plan_name = sys.argv[1]
    try:
        phase = int(sys.argv[2])
    except ValueError:
        print(f"Error: phase_number must be an integer, got '{sys.argv[2]}'", file=sys.stderr)
        sys.exit(1)

    if plan_name not in PLANS:
        available = ", ".join(PLANS.keys())
        print(f"Error: unknown plan '{plan_name}'. Available: {available}", file=sys.stderr)
        sys.exit(1)

    if phase not in PLANS[plan_name]:
        available = ", ".join(str(p) for p in sorted(PLANS[plan_name].keys()))
        print(f"Error: unknown phase {phase} for plan '{plan_name}'. Available: {available}", file=sys.stderr)
        sys.exit(1)

    print(PLANS[plan_name][phase])

if __name__ == "__main__":
    main()
```

**Important**: if `plans/get_prompt.py` already exists, add the new plan as a new key in `PLANS` rather than overwriting the file. Preserve all existing plans.

Note that from Phase 2 onwards, each prompt should also instruct the agent to read all prior phase reports before starting, so it has a full picture of how execution has diverged from the original plan.

---

## Step 5: Output Summary

After creating the files, tell the user:
- How many phases were created and what each one does
- How to run the script: `python plans/get_prompt.py <plan_name> <phase_number>`
- That each phase agent will write a report to `plans/reports/phase-<N>.md` when done, and they should review these between phases to decide if PLAN.md needs updating
- Remind them to read through PLAN.md and adjust if anything looks off before kicking off Phase 1

---

## Conventions

- Plan names: lowercase with hyphens (e.g. `auth-refactor`, `migrate-to-postgres`)
- Phases: always numbered from 1
- Every phase prompt must begin with an instruction to read `plans/PLAN.md`
- Phase 2+ prompts must also instruct the agent to read all prior phase reports
- Prompts should be written as direct instructions to an agent, not descriptions of what will happen
- Phase reports live in `plans/reports/phase-<N>.md` and are written by the executing agent, not pre-created
