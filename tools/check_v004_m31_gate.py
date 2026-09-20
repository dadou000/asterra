#!/usr/bin/env python3
"""Check whether the Orbit V0.0.4 M31 integration gate may open."""

from __future__ import annotations

import re
import sys
from pathlib import Path

from validate_v004_m30_performance import EXPECTED, validate

REPO_ROOT = Path(__file__).resolve().parents[1]
CANONICAL_CSV = REPO_ROOT / "docs" / "research" / "v004-m30-performance.csv"
SUMMARY_MD = REPO_ROOT / "docs" / "V0.0.4_M30_PERFORMANCE_CAPTURE.md"
PROVENANCE_TOML = REPO_ROOT / "docs" / "research" / "v004-m30-performance.meta.toml"
M30_DOC = REPO_ROOT / "docs" / "V0.0.4_M30_VALIDATION_REGRESSION.md"
PROGRESS_DOC = REPO_ROOT / "docs" / "V0.0.4_PROGRESS.md"
VALIDATION_CPP = REPO_ROOT / "tests" / "V004TerrainValidationTests.cpp"
PERFORMANCE_CPP = REPO_ROOT / "tests" / "V004TerrainPerformance.cpp"


def fail(message: str) -> None:
    raise ValueError(message)


def require_file(path: Path, description: str) -> str:
    if not path.is_file():
        fail(f"{description} is missing: {path.relative_to(REPO_ROOT)}")
    return path.read_text(encoding="utf-8")


def main() -> int:
    try:
        adapter, _ = validate(CANONICAL_CSV)

        require_file(SUMMARY_MD, "M30 performance summary")
        provenance = require_file(PROVENANCE_TOML, "M30 performance provenance")
        if 'source_commit = "' not in provenance:
            fail("M30 performance provenance is missing source commit")
        if 'build_config = "' not in provenance:
            fail("M30 performance provenance is missing build configuration")
        if f'adapter = "{adapter.replace(chr(34), chr(39))}"' not in provenance:
            fail("M30 performance provenance adapter does not match validated CSV")

        m30 = require_file(M30_DOC, "M30 validation document")
        progress = require_file(PROGRESS_DOC, "V0.0.4 progress ledger")
        validation = require_file(
            VALIDATION_CPP,
            "M30 deterministic validation source",
        )
        performance = require_file(
            PERFORMANCE_CPP,
            "M30 performance diagnostic source",
        )

        status_line = next(
            (line for line in m30.splitlines() if line.startswith("Status:")),
            None,
        )
        if status_line is None or "Complete" not in status_line:
            fail("M30 validation document is not marked Complete")

        m30_row = next(
            (
                line
                for line in progress.splitlines()
                if line.startswith("| M30 — Validation and regression suite |")
            ),
            None,
        )
        if m30_row is None or "| **Complete** |" not in m30_row:
            fail("M30 progress row is not Complete")

        m31_row = next(
            (
                line
                for line in progress.splitlines()
                if line.startswith("| M31 — Final integration gate |")
            ),
            None,
        )
        if m31_row is None or (
            "| **Ready** |" not in m31_row
            and "| **In progress" not in m31_row
            and "| **Complete** |" not in m31_row
        ):
            fail("M31 progress row is not Ready/In progress/Complete")

        main_match = re.search(
            r"int\s+main\s*\(\s*\)\s*\{(?P<body>.*?)\n\}",
            validation,
            flags=re.DOTALL,
        )
        if main_match is None:
            fail("could not locate M30 deterministic main()")

        main_body = main_match.group("body")

        missing_cases = [
            index
            for index in range(1, 21)
            if f"Test{index:02d}" not in main_body
        ]
        if missing_cases:
            fail(
                "M30 deterministic main() is missing registered cases: "
                + ", ".join(str(index) for index in missing_cases)
            )

        if "20/20 deterministic cases passed" not in main_body:
            fail("M30 deterministic executable does not report 20/20")

        missing_metrics = [
            metric for metric in EXPECTED if metric not in performance
        ]
        if missing_metrics:
            fail(
                "M30 performance executable does not emit required metrics: "
                + ", ".join(missing_metrics)
            )

        if "drainageBuilder.Dispatch" not in performance:
            fail("M30 performance diagnostic is missing production M09 drainage")

        if "scatterBuilder.Dispatch" not in performance:
            fail("M30 performance diagnostic is missing production M22 scatter")

    except ValueError as exc:
        print(f"M31 gate CLOSED: {exc}", file=sys.stderr)
        return 1

    print("M31 gate OPEN")
    print(f"M30 performance adapter: {adapter}")
    print("deterministic cases: 20/20 registered")
    print(f"performance metrics: {len(EXPECTED)}/{len(EXPECTED)} captured")
    print("M30 ledger: COMPLETE")
    print("M31 ledger: READY")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
