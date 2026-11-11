+++
path = "/history"
title = "History: version specs, milestone records and notes"
kind = "section"
status = "historical"
summary = """
The version series that built Orbit (V0.0.3 to V0.0.7), research notes and older notes. The V0.0.3-V0.0.6 specs, execution ledgers and milestone records were retired in 0.0.9 and remain in git history; the V0.0.7 records are still here. \
These are records, not normative rules: the normative rules are under /rules and the current behaviour is in the module cards."""
keywords = ["history", "versions", "milestones", "spec", "progress", "ledger", "roadmap", "release", "v0.0.3", "v0.0.7"]
related = ["/rules"]

[routes]
"V0.0.3 celestial authoring scaffold" = "v0-0-3"
"V0.0.4 planet surface authority, terrain, WaterService, milestones M00-M31" = "v0-0-4"
"V0.0.5 Studio terrain validation" = "v0-0-5"
"V0.0.6 celestial systems, research notes" = "v0-0-6"
"V0.0.7 dynamic lighting, GI, HDR, volumetrics" = "v0-0-7"
"problem tracker, known issues, primitives, patch and synthesis notes" = "notes"
+++

| Release | State | Record |
| --- | --- | --- |
| V0.0.3 | complete (2026-09-18) | `/history/v0-0-3` |
| V0.0.4 | complete (2026-09-20), final gate PASS | `/history/v0-0-4` |
| V0.0.5 | complete (2026-09-20), gate PASS | `/history/v0-0-5` |
| V0.0.6 | implemented through M33, validation/gate in progress (spec and ledger retired in 0.0.9, see git history) | `/history/v0-0-6` |
| V0.0.7 | M00-M46 implemented, release validation open | `/history/v0-0-7` |

When a spec and its ledger disagree, trust the ledger and then the code; when a document and the code disagree, trust the code and fix the document.
