"""Draft a module card from facts in the repository.

    python tools/orbit_docs_cli.py scaffold engine/my_module --path /rendering/my-module

The draft is deliberately unfinished: its summary starts with `TODO(docs)`, and
`orbit_docs_cli.py check` rejects any block that still contains that marker, so a
scaffold cannot be committed as-is. Facts (public headers, top-level types, CMake
dependencies mapped to existing cards, registered test names) come from the code;
the summary, keywords and invariants must be written from reading it.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from .index import DocsIndex

TODO_MARKER = "TODO(docs)"


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _snake(name: str) -> str:
    s = re.sub(r"(?<!^)(?=[A-Z][a-z])", "_", name)
    s = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", s)
    return s.lower()


def _card_path(index: DocsIndex, module: str) -> str | None:
    prefix = module + "/"
    for block in sorted(index.blocks.values(), key=lambda b: b.path):
        if block.legacy:
            continue
        if block.module_dir == module or any(src.startswith(prefix) for src in block.sources):
            return block.path
    return None


def scaffold(index: DocsIndex, module: str, path: str | None = None) -> str:
    root = index.root
    base = root / module
    if not base.is_dir():
        raise ValueError(f"{module} is not a directory under the repository root")
    headers = sorted(
        h.relative_to(root).as_posix()
        for pattern in ("include/**/*.hpp", "include/**/*.inl")
        for h in base.glob(pattern)
    )
    symbols: list[str] = []
    for header in headers:
        for line in _read(root / header).splitlines():
            m = re.match(r"^(?:class|struct)\s+(?:\[\[[^\]]*\]\]\s+)?(\w+)\s*(?:final\s*)?(?:[:{]|$)", line)
            if m and m.group(1) not in symbols and not m.group(1).endswith("Tag"):
                symbols.append(m.group(1))
                break
    cmake = _read(base / "CMakeLists.txt")
    owner = (re.findall(r"add_library\((\w+)", cmake) or [""])[0]
    tests = sorted(set(re.findall(r"NAME\s+(Orbit\.\w+)", cmake)))
    deps: list[str] = []
    for m in re.finditer(r"target_link_libraries\([^)]*?\)", cmake, re.S):
        for target in re.findall(r"Orbit::(\w+)", m.group(0)):
            candidate = f"engine/{_snake(target)}"
            if candidate != module and (root / candidate).is_dir():
                card = _card_path(index, candidate)
                if card and card not in deps:
                    deps.append(card)
    slug = module.split("/")[-1].replace("_", "-")
    node = path or f"/rendering/{slug}"
    name = slug.replace("-", " ").title()
    sources = headers[:8] + [f"{module}/CMakeLists.txt"]
    lines = [
        "+++",
        f"path = {json.dumps(node)}",
        f"title = {json.dumps(name)}",
        'kind = "reference"      # use "subsystem" once invariants are written',
        'status = "stable"',
        f'summary = "{TODO_MARKER}: one or two sentences that stand alone (what it owns, what it does not)."',
    ]
    if owner:
        lines.append(f"owner_module = {json.dumps(owner)}")
    lines += [
        f'keywords = ["{slug}"]    # words people will search with',
        f"sources = {json.dumps(sources, indent=2)}",
        f"symbols = {json.dumps(symbols[:8])}",
        "invariants = []          # what must stay true; only what you verified in code or specs",
    ]
    if deps:
        lines.append(f"depends_on = {json.dumps(deps)}")
    lines.append(
        "verify = "
        + json.dumps([f"ctest -R {t}" for t in tests] or ["No test is registered for this module under its own name."], indent=2)
    )
    lines += ['verified = ""            # commit you checked this block against', "+++", "", f"{TODO_MARKER}: body (200-800 words; routes, diagnose steps and invariants where they exist).", ""]
    return "\n".join(lines)
