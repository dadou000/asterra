# Visual review rubric v1

Used by `python tools/orbit_ab.py council` (see `/tools/ab-capture`). Scores are 0 to 10, anchored below, per variant and per
category, with a confidence (low, medium, high) and a one-line reason. A category that a still image cannot judge is `null` (N/A):
stills cannot prove temporal stability, and exposure or reference mismatch can hide or fake a defect. Subjective scores are kept
apart from measured values (mean difference, changed pixels, frame time), which the harness records in `review.json -> run`.

| Score | Anchor |
| --- | --- |
| 0 | broken: missing, black, NaN-like or wrong geometry |
| 3 | poor: wrong at a glance (flat, banded, haloed, obviously synthetic) |
| 5 | acceptable with obvious flaws a player would notice |
| 7 | good: flaws only on close inspection |
| 9 | near photographic for this scene and exposure |
| 10 | indistinguishable from a reference |

Categories:

- `shape_fidelity`: silhouettes, edges, internal structure, identity of the object across variants.
- `lighting`: direction, falloff, shadowing, scattering and colour of the light on the subject.
- `colour_exposure`: tone, contrast, saturation, clipping, horizon and sky gradient.
- `artifacts`: 10 means none. Banding, grain, ghosting, seams, popping, halos.
- `scale_depth`: believable size, parallax cues, aerial perspective.

Not scored, measured: frame time, VRAM, shader cost. A cheaper variant must justify any lost score; the Minimalist can block an
expensive variant whose measured gain is negligible, and the Artist cannot demand unlimited cost.

Review ids are `R-<date>-<time>-<commit>` and immutable. `docs/reviews/index.jsonl` is the searchable index (one line per review).
Full-resolution captures stay outside git; each scene's PNG hash is in the review so a later run can be matched to it.
