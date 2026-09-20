#!/usr/bin/env python3
"""Import a validated Orbit V0.0.4 M30 performance capture into the repo."""

from __future__ import annotations

import csv
import hashlib
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

from validate_v004_m30_performance import EXPECTED, RESOLUTION, validate

REPO_ROOT = Path(__file__).resolve().parents[1]
CANONICAL_CSV = REPO_ROOT / "docs" / "research" / "v004-m30-performance.csv"
SUMMARY_MD = REPO_ROOT / "docs" / "V0.0.4_M30_PERFORMANCE_CAPTURE.md"
PROVENANCE_TOML = REPO_ROOT / "docs" / "research" / "v004-m30-performance.meta.toml"
M30_DOC = REPO_ROOT / "docs" / "V0.0.4_M30_VALIDATION_REGRESSION.md"
PROGRESS_DOC = REPO_ROOT / "docs" / "V0.0.4_PROGRESS.md"


def fail(message: str) -> None:
    raise ValueError(message)


def replace_single_line(
    text: str,
    prefix: str,
    replacement: str,
    description: str,
) -> str:
    lines = text.splitlines()
    indices = [index for index, line in enumerate(lines) if line.startswith(prefix)]

    if len(indices) != 1:
        fail(
            f"{description}: expected exactly one line starting with "
            f"{prefix!r}, found {len(indices)}"
        )

    lines[indices[0]] = replacement
    return "\n".join(lines) + ("\n" if text.endswith("\n") else "")


def replace_next_target(text: str, replacement: str) -> str:
    marker = "## Next implementation target"
    index = text.find(marker)

    if index < 0:
        fail("progress document is missing the next implementation target section")

    head = text[:index]
    return head + replacement.rstrip() + "\n"


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def format_value(value: float, unit: str) -> str:
    if unit == "bytes":
        return f"{int(round(value)):,} B"
    if unit == "percent":
        return f"{value:.6f}%"
    if unit == "ms":
        return f"{value:.6f} ms"
    return f"{value} {unit}"


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(
            "usage: import_v004_m30_performance.py "
            "<validated-performance.csv> [build-config]",
            file=sys.stderr,
        )
        return 2

    source = Path(sys.argv[1]).resolve()
    build_config = sys.argv[2].strip() if len(sys.argv) == 3 else "unknown"

    try:
        adapter, values = validate(source)
    except ValueError as exc:
        print(f"M30 performance import refused: {exc}", file=sys.stderr)
        return 1

    rows = read_csv_rows(source)
    rows_by_metric = {row["metric"].strip(): row for row in rows}

    CANONICAL_CSV.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, CANONICAL_CSV)

    captured_at = datetime.now(timezone.utc).replace(microsecond=0)
    captured_iso = captured_at.isoformat().replace("+00:00", "Z")

    try:
        source_commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO_ROOT,
            text=True,
            encoding="utf-8",
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        source_commit = "unknown"

    csv_sha256 = hashlib.sha256(
        CANONICAL_CSV.read_bytes()
    ).hexdigest()

    provenance_lines = [
        'schema = "orbit_v004_m30_capture_v1"',
        f'captured_utc = "{captured_iso}"',
        f'adapter = "{adapter.replace(chr(34), chr(39))}"',
        f'build_config = "{build_config.replace(chr(34), chr(39))}"',
        f'source_commit = "{source_commit}"',
        f'csv_sha256 = "{csv_sha256}"',
        f'resolution = {RESOLUTION}',
        f'metric_count = {len(EXPECTED)}',
        "",
    ]

    PROVENANCE_TOML.write_text(
        "\n".join(provenance_lines),
        encoding="utf-8",
        newline="\n",
    )

    summary_lines = [
        "# Orbit V0.0.4 M30 — Captured performance record",
        "",
        f"Captured: **{captured_iso}**",
        "",
        f"Adapter: **{adapter}**",
        "",
        f"Build configuration: **{build_config}**",
        "",
        f"Source commit: `{source_commit}`",
        "",
        f"CSV SHA-256: `{csv_sha256}`",
        "",
        f"Physical page / planting-grid resolution: **{RESOLUTION}×{RESOLUTION}**",
        "",
        "This file is generated only from a CSV that passes "
        "tools/validate_v004_m30_performance.py.",
        "",
        "| Metric | Captured value | Samples |",
        "| --- | ---: | ---: |",
    ]

    for metric, (_, expected_samples) in EXPECTED.items():
        row = rows_by_metric[metric]
        summary_lines.append(
            f"| {metric} | {format_value(values[metric], row['unit'])} "
            f"| {expected_samples} |"
        )

    summary_lines += [
        "",
        "## Gate result",
        "",
        "**M30 performance capture: VALID.**",
        "",
        "The deterministic M30 suite must also pass locally before M31 opens. "
        "run_m30_performance.bat performs that deterministic run before "
        "capturing/importing this record.",
        "",
        "Canonical machine-readable sources:",
        "",
        "docs/research/v004-m30-performance.csv",
        "",
        "docs/research/v004-m30-performance.meta.toml",
        "",
    ]

    SUMMARY_MD.write_text(
        "\n".join(summary_lines),
        encoding="utf-8",
        newline="\n",
    )

    m30 = M30_DOC.read_text(encoding="utf-8")
    m30 = replace_single_line(
        m30,
        "Status:",
        "Status: **Complete — 20/20 deterministic cases + 8/8 validated performance records captured**",
        "M30 validation document status",
    )

    capture_note = (
        "\n## Captured performance gate\n\n"
        f"A named-adapter capture has been validated and imported from "
        f"**{adapter}** at **{captured_iso}**. The canonical CSV is "
        "docs/research/v004-m30-performance.csv and the human-readable "
        "summary is docs/V0.0.4_M30_PERFORMANCE_CAPTURE.md.\n"
    )

    existing_marker = "\n## Captured performance gate\n"
    if existing_marker in m30:
        m30 = m30[: m30.index(existing_marker)] + capture_note
    else:
        m30 = m30.rstrip() + capture_note

    M30_DOC.write_text(m30, encoding="utf-8", newline="\n")

    progress = PROGRESS_DOC.read_text(encoding="utf-8")
    progress = replace_single_line(
        progress,
        "| M30 — Validation and regression suite |",
        "| M30 — Validation and regression suite | **Complete** | "
        f"All 20 deterministic acceptance cases are implemented and the "
        f"8/8 performance record was captured on {adapter} at {captured_iso}. "
        "The canonical CSV passed the strict M30 validator; M31 may begin. |",
        "M30 progress row",
    )

    progress = replace_single_line(
        progress,
        "| M31 — Final integration gate |",
        "| M31 — Final integration gate | **Ready** | "
        "M30 deterministic and performance gates are satisfied. Execute the "
        "final V0.0.4 end-to-end integration acceptance against the production "
        "authority chain. |",
        "M31 progress row",
    )

    progress = replace_next_target(
        progress,
        """## Next implementation target

**M31 — Final integration gate**

M30 is complete. Run the V0.0.4 end-to-end integration acceptance: automatic rocky-planet services/default surface, authored canyon → drainage/erosion response, physical exposure/burial → rendered material, automatic/authored biome placement, constrained scatter, persistent GPU-cache reuse, dependency-only regeneration, debug-field inspection, and authored save/load equivalence.
""",
    )

    PROGRESS_DOC.write_text(progress, encoding="utf-8", newline="\n")

    print("M30 performance capture imported")
    print(f"adapter: {adapter}")
    print(f"captured: {captured_iso}")
    print(f"csv: {CANONICAL_CSV.relative_to(REPO_ROOT)}")
    print(f"summary: {SUMMARY_MD.relative_to(REPO_ROOT)}")
    print(f"provenance: {PROVENANCE_TOML.relative_to(REPO_ROOT)}")
    print(f"source commit: {source_commit}")
    print(f"build config: {build_config}")
    print("M30 ledger: COMPLETE")
    print("M31 ledger: READY")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
