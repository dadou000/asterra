#!/usr/bin/env python3
"""Compile-check independent System B cloud shader entrypoints.

    python tools/check_unified_cloud_shaders.py

Requires glslangValidator in PATH. This validates HLSL->SPIR-V syntax and
resource declarations, not Vulkan pipeline compatibility or rendered output.
The source is extracted directly from UnifiedCloudRenderer.cpp; no bundled
proprietary shaders are used.
"""
from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "engine" / "celestial_clouds" / "src" / "UnifiedCloudRenderer.cpp"
PATTERN = re.compile(r'constexpr const char\* (kVertex|kMedium|kComposite) = R"\((.*?)\)";', re.S)


def main() -> int:
    validator = shutil.which("glslangValidator")
    if not validator:
        print("glslangValidator is required (Vulkan SDK or glslang-tools)", file=sys.stderr)
        return 2
    text = SOURCE.read_text(encoding="utf-8")
    blocks = dict(PATTERN.findall(text))
    required = {"kVertex", "kMedium", "kComposite"}
    if set(blocks) != required:
        print(f"Could not extract B shader strings: expected {required}, got {set(blocks)}",
              file=sys.stderr)
        return 1
    variants = [
        ("vertex", "kVertex", "vert", "main"),
        ("march", "kMedium", "frag", "March"),
        ("shadow", "kMedium", "frag", "ReceiverShadow"),
        ("sun-cache", "kMedium", "comp", "UpdateSunCache"),
        ("composite", "kComposite", "frag", "main"),
    ]
    failures = 0
    with tempfile.TemporaryDirectory(prefix="orbit-unified-cloud-") as directory:
        temporary = pathlib.Path(directory)
        for label, key, stage, entry in variants:
            shader = temporary / f"{label}.hlsl"
            prefix = "#define B3_WRITE_CACHE 1\n" if label == "sun-cache" else ""
            shader.write_text(prefix + blocks[key], encoding="utf-8")
            command = [
                validator, "-D", "-V", "-S", stage,
                "-e", entry, "--target-env", "vulkan1.3",
                "-o", str(temporary / f"{label}.spv"), str(shader)
            ]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            print(f"{label:12s} {'ok' if result.returncode == 0 else 'FAILED'}")
            if result.returncode:
                failures += 1
                print((result.stdout + "\n" + result.stderr)[-4000:], file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
