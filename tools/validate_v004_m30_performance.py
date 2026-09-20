#!/usr/bin/env python3
"""Validate a captured Orbit V0.0.4 M30 terrain performance CSV."""

from __future__ import annotations

import csv
import math
import sys
from pathlib import Path

EXPECTED = {
    "page_generation_gpu_time": ("ms", 7),
    "peak_transient_memory": ("bytes", 1),
    "persistent_page_memory": ("bytes", 1),
    "cache_hit_rate": ("percent", 256),
    "hydraulic_iteration_gpu_time": ("ms", 7),
    "aeolian_iteration_gpu_time": ("ms", 7),
    "drainage_build_gpu_time": ("ms", 7),
    "scatter_generation_gpu_time": ("ms", 7),
}

REQUIRED_COLUMNS = {
    "schema",
    "adapter",
    "resolution",
    "metric",
    "value",
    "unit",
    "samples",
}

SCHEMA = "orbit_v004_m30"
RESOLUTION = 129


def fail(message: str) -> None:
    raise ValueError(message)


def parse_positive_float(text: str, metric: str) -> float:
    try:
        value = float(text)
    except ValueError as exc:
        fail(f"{metric}: value is not numeric: {text!r}")
        raise AssertionError from exc

    if not math.isfinite(value) or value <= 0.0:
        fail(f"{metric}: value must be finite and > 0, got {value!r}")

    return value


def validate(path: Path) -> tuple[str, dict[str, float]]:
    if not path.is_file():
        fail(f"performance CSV does not exist: {path}")

    with path.open("r", newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)

        if reader.fieldnames is None:
            fail("performance CSV has no header")

        columns = set(reader.fieldnames)
        if columns != REQUIRED_COLUMNS:
            missing = sorted(REQUIRED_COLUMNS - columns)
            extra = sorted(columns - REQUIRED_COLUMNS)
            fail(
                "unexpected CSV columns; "
                f"missing={missing or 'none'}, extra={extra or 'none'}"
            )

        rows = list(reader)

    if len(rows) != len(EXPECTED):
        fail(
            f"expected exactly {len(EXPECTED)} metric rows, "
            f"found {len(rows)}"
        )

    adapter: str | None = None
    values: dict[str, float] = {}

    for row_number, row in enumerate(rows, start=2):
        metric = row["metric"].strip()

        if metric not in EXPECTED:
            fail(f"row {row_number}: unknown metric {metric!r}")

        if metric in values:
            fail(f"row {row_number}: duplicate metric {metric!r}")

        if row["schema"].strip() != SCHEMA:
            fail(
                f"row {row_number}: schema must be {SCHEMA!r}, "
                f"got {row['schema']!r}"
            )

        row_adapter = row["adapter"].strip()
        if not row_adapter:
            fail(f"row {row_number}: adapter name is empty")

        if adapter is None:
            adapter = row_adapter
        elif row_adapter != adapter:
            fail(
                f"row {row_number}: adapter changed from "
                f"{adapter!r} to {row_adapter!r}"
            )

        try:
            resolution = int(row["resolution"])
        except ValueError:
            fail(
                f"row {row_number}: resolution is not an integer: "
                f"{row['resolution']!r}"
            )

        if resolution != RESOLUTION:
            fail(
                f"row {row_number}: resolution must be {RESOLUTION}, "
                f"got {resolution}"
            )

        expected_unit, expected_samples = EXPECTED[metric]

        if row["unit"].strip() != expected_unit:
            fail(
                f"{metric}: unit must be {expected_unit!r}, "
                f"got {row['unit']!r}"
            )

        try:
            samples = int(row["samples"])
        except ValueError:
            fail(f"{metric}: samples is not an integer: {row['samples']!r}")

        if samples != expected_samples:
            fail(
                f"{metric}: samples must be {expected_samples}, "
                f"got {samples}"
            )

        value = parse_positive_float(row["value"], metric)

        if metric == "cache_hit_rate":
            expected_rate = 255.0 / 256.0 * 100.0

            if not math.isclose(
                value,
                expected_rate,
                rel_tol=0.0,
                abs_tol=1.0e-6,
            ):
                fail(
                    "cache_hit_rate must match the controlled "
                    f"255/256 workload ({expected_rate:.6f}%), "
                    f"got {value:.6f}%"
                )

        values[metric] = value

    missing_metrics = sorted(set(EXPECTED) - set(values))
    if missing_metrics:
        fail(f"missing required metrics: {missing_metrics}")

    assert adapter is not None
    return adapter, values


def main() -> int:
    if len(sys.argv) != 2:
        print(
            "usage: validate_v004_m30_performance.py <performance.csv>",
            file=sys.stderr,
        )
        return 2

    path = Path(sys.argv[1])

    try:
        adapter, values = validate(path)
    except ValueError as exc:
        print(f"M30 performance record INVALID: {exc}", file=sys.stderr)
        return 1

    print("M30 performance record VALID")
    print(f"adapter: {adapter}")
    print(f"resolution: {RESOLUTION}")
    print(f"metrics: {len(values)}/{len(EXPECTED)}")

    for metric in EXPECTED:
        print(f"{metric}: {values[metric]}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
