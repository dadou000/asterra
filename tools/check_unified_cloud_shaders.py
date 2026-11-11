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
import struct
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "engine" / "celestial_clouds" / "src" / "UnifiedCloudRenderer.cpp"
# Fragmented to avoid MSVC C2026; the tool must compile the identical
# shader that the runtime assembles (never a different hand-maintained copy).
PATTERN = re.compile(
    r'constexpr const char\* (kVertex|kMediumPart[1-4]|kComposite) = R"\((.*?)\)";',
    re.S,
)


def main() -> int:
    validator = shutil.which("glslangValidator")
    if not validator:
        print("glslangValidator is required (Vulkan SDK or glslang-tools)", file=sys.stderr)
        return 2
    text = SOURCE.read_text(encoding="utf-8")
    blocks = dict(PATTERN.findall(text))
    required = {"kVertex", "kComposite"} | {
        f"kMediumPart{i}" for i in range(1, 5)
    }
    if set(blocks) != required:
        print(f"Could not extract B shader strings: expected {required}, got {set(blocks)}",
              file=sys.stderr)
        return 1
    chunks = [blocks[f"kMediumPart{i}"] for i in range(1, 5)]
    if any(len(part.encode("utf-8")) >= 12000 for part in chunks):
        print("System B shader fragment exceeds MSVC-safe 12 KiB limit",
              file=sys.stderr)
        return 1
    blocks["kMedium"] = "".join(chunks)
    variants = [
        ("vertex", "kVertex", "vert", "main"),
        ("march", "kMedium", "frag", "March"),
        ("shadow", "kMedium", "frag", "ReceiverShadow"),
        ("sun-cache", "kMedium", "comp", "UpdateSunCache"),
        ("sun-clear", "kMedium", "comp", "ClearSunCache"),
        ("composite", "kComposite", "frag", "main"),
    ]
    failures = 0
    with tempfile.TemporaryDirectory(prefix="orbit-unified-cloud-") as directory:
        temporary = pathlib.Path(directory)
        for label, key, stage, entry in variants:
            shader = temporary / f"{label}.hlsl"
            # Mirror the exact B shader entrypoint renaming and verify
            # SPIR-V, not just that the HLSL compiles.
            prefix = ""
            if label in ("sun-cache", "sun-clear"):
                prefix += "#define B3_WRITE_CACHE 1\n"
            if entry != "main":
                prefix += f"#define {entry} main\n"
            shader.write_text(prefix + blocks[key], encoding="utf-8")
            output = temporary / f"{label}.spv"
            command = [
                validator, "-D", "-V", "-S", stage, "-e", "main",
                "--target-env", "vulkan1.3", "-o", str(output), str(shader)
            ]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            ok = result.returncode == 0
            if ok:
                data = output.read_bytes()
                ok = len(data) >= 20 and len(data) % 4 == 0
                if ok:
                    words = struct.unpack(f"<{len(data)//4}I", data)
                    ok = words[0] == 0x07230203
                    index = 5
                    entries = []
                    while ok and index < len(words):
                        word = words[index]
                        count, opcode = word >> 16, word & 0xFFFF
                        if count == 0 or index + count > len(words):
                            ok = False
                            break
                        if opcode == 15 and count >= 4:  # OpEntryPoint
                            raw = struct.pack(
                                f"<{count-3}I", *words[index+3:index+count]
                            )
                            entries.append(raw.split(b"\0", 1)[0])
                        index += count
                    if entries != [b"main"]:
                        ok = False
                        print(f"{label}: unexpected SPIR-V entrypoints: {entries}",
                              file=sys.stderr)
            print(f"{label:12s} {'ok' if ok else 'FAILED'}")
            if not ok:
                failures += 1
                print((result.stdout + "\n" + result.stderr)[-4000:],
                      file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
