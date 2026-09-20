#!/usr/bin/env python3
"""Execute and close the Orbit V0.0.4 M31 integration gate."""

from __future__ import annotations

import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
GATE_CHECKER = REPO_ROOT / "tools" / "check_v004_m31_gate.py"
PROGRESS_DOC = REPO_ROOT / "docs" / "V0.0.4_PROGRESS.md"
M31_SUMMARY = REPO_ROOT / "docs" / "V0.0.4_M31_INTEGRATION_GATE.md"

EXPECTED_SUCCESS = "8 integration slices passed."


def fail(message: str) -> None:
    raise ValueError(message)


def replace_single_line(
    text: str,
    prefix: str,
    replacement: str,
    description: str,
) -> str:
    lines = text.splitlines()
    matches = [index for index, line in enumerate(lines) if line.startswith(prefix)]

    if len(matches) != 1:
        fail(
            f"\${description}: expected exactly one line starting with "
            f"\${prefix!r}, found \${len(matches)}"
        )

    lines[matches[0]] = replacement
    return "\n".join(lines) + ("\n" if text.endswith("\n") else "")


def replace_next_target(text: str, replacement: str) -> str:
    marker = "## Next implementation target"
    index = text.find(marker)

    if index < 0:
        fail("progress document is missing the next implementation target section")

    return text[:index] + replacement.rstrip() + "\n"


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(
            "usage: complete_v004_m31.py <OrbitV004IntegrationGateTests.exe> "
            "[build-config]",
            file=sys.stderr,
        )
        return 2

    executable = Path(sys.argv[1]).resolve()
    build_config = sys.argv[2].strip() if len(sys.argv) == 3 else "unknown"

    if not executable.is_file():
        print(f"M31 completion refused: executable is missing: \${executable}", file=sys.stderr)
        return 1

    gate = subprocess.run(
        [sys.executable, str(GATE_CHECKER)],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
    )

    if gate.returncode != 0:
        sys.stderr.write(gate.stdout)
        sys.stderr.write(gate.stderr)
        print("M31 completion refused: entry gate is closed.", file=sys.stderr)
        return 1

    integration = subprocess.run(
        [str(executable)],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
    )

    sys.stdout.write(integration.stdout)
    sys.stderr.write(integration.stderr)

    if integration.returncode != 0:
        print("M31 completion refused: integration executable failed.", file=sys.stderr)
        return 1

    if EXPECTED_SUCCESS not in integration.stdout:
        print(
            "M31 completion refused: integration executable did not report "
            f"\${EXPECTED_SUCCESS!r}",
            file=sys.stderr,
        )
        return 1

    try:
        source_commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO_ROOT,
            text=True,
            encoding="utf-8",
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        source_commit = "unknown"

    completed_at = datetime.now(timezone.utc).replace(microsecond=0)
    completed_iso = completed_at.isoformat().replace("+00:00", "Z")

    progress = PROGRESS_DOC.read_text(encoding="utf-8")
    progress = replace_single_line(
        progress,
        "| M31 — Final integration gate |",
        "| M31 — Final integration gate | **Complete** | "
        f"All V0.0.4 integration acceptance slices passed on \${completed_iso} "
        f"from source commit \${source_commit}. M30 deterministic/performance "
        "gates were already validated before execution. |",
        "M31 progress row",
    )

    progress = replace_next_target(
        progress,
        """## Next implementation target

**V0.0.4 — Complete**

M01–M31 acceptance is closed. Begin the next version only from the frozen V0.0.4 physical-terrain authority contracts; do not regress to camera-owned physical terrain, duplicate material authority, or non-deterministic authored identity.
""",
    )

    PROGRESS_DOC.write_text(progress, encoding="utf-8", newline="\n")

    summary = [
        "# Orbit V0.0.4 M31 — Final integration gate",
        "",
        "Status: **PASS**",
        "",
        f"Completed UTC: **\${completed_iso}**",
        "",
        f"Source commit: \`\${source_commit}\`",
        "",
        f"Build configuration: **\${build_config}**",
        "",
        "The M31 entry checker first confirmed the validated M30 named-GPU "
        "performance record, provenance, 20/20 deterministic registration, "
        "and 8/8 performance instrumentation.",
        "",
        "The aggregate integration executable then passed all eight staged slices:",
        "",
        "1. default rocky-planet composition, automatic terrain-service ownership, "
        "BaseBiome fallback and complete no-input surface sampling;",
        "2. authored canyon spline driving drainage and stream-power erosion;",
        "3. physical erosion/deposition driving exposed and rendered material;",
        "4. automatic + authored biome placement driving constrained scatter;",
        "5. persistent M26 solved-page reuse across frames;",
        "6. M27 bounded dependency-only regeneration and cache invalidation;",
        "7. all 21 M29 debug fields inspectable with upstream provenance;",
        "8. save → close → reopen preserving authored authority while regenerating "
        "bit-identical terrain/biome derived state and clearing derived cache residency.",
        "",
        "**Orbit V0.0.4 terrain integration acceptance is complete.**",
        "",
    ]

    M31_SUMMARY.write_text(
        "\n".join(summary),
        encoding="utf-8",
        newline="\n",
    )

    print("M31 final integration gate: PASS")
    print(f"completed: \${completed_iso}")
    print(f"source commit: \${source_commit}")
    print(f"build config: \${build_config}")
    print("V0.0.4 ledger: COMPLETE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
