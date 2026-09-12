# Asterra Planning

<!-- ASTERRA_PLANNING_VERSION: 1 -->

Working notes and durable execution plans that are not code and not shipped docs.

- **PROBLEMS.md** — running tracker of known issues to recheck later. Each entry
  has an ID (`P-NNN`), a status (`OPEN` / `FIX-UNVERIFIED` / `RESOLVED`), where it
  lives, what it is, and a `RECHECK:` line for anything fixed-but-unconfirmed.
  Add new problems at the top of **Open**; move solved ones to **Resolved** with
  the fix and how it was verified rather than deleting them.
- **Execution plans** — durable plans for work that spans multiple files, systems,
  or validation passes. See the status convention and active-plans table below.

## Status convention

Every active plan uses both human-readable checkboxes and machine-readable HTML markers.

- `[ ]` — not started
- `[-]` — in progress
- `[x]` — complete and acceptance criteria passed
- `[!]` — blocked; the blocker must be documented in the plan
- `[~]` — intentionally deferred / optional

Machine-readable plan status values are:

- `NOT_STARTED`
- `IN_PROGRESS`
- `BLOCKED`
- `COMPLETE`

A task is **not complete** merely because code was written. A plan may be changed to `COMPLETE` only after every mandatory acceptance gate and validation gate in that plan has passed.

## Active plans

| Plan | Status | Completion marker |
| --- | --- | --- |
| [Long-range sparse hydrology / Planet Studio water sources](WATER_LONG_RANGE_HYDROLOGY.md) | NOT_STARTED | `ASTERRA_WATER_LONG_RANGE_TASK_COMPLETE: false` |

## Execution rules

1. Work from the plan in phase order unless a phase explicitly says it can run in parallel.
2. Change a phase marker to `IN_PROGRESS` when implementation of that phase begins.
3. Change it to `COMPLETE` only after that phase's acceptance gate passes.
4. Record blockers next to the affected phase; do not silently skip mandatory work.
5. Keep validation results, relevant commit SHAs, and known limitations in the plan so another session can resume without chat history.
6. Never set a plan's final `TASK_COMPLETE` marker to `true` until all mandatory completion gates are checked.
