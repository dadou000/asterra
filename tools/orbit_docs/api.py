"""Docs operations with a uniform result envelope.

Every operation returns

    {"ok": true,  "operation": "docs.get", "request_id": "orbit-docs-7",
     "duration_ms": 0.4, "result": {...}, "warnings": [], "suggestions": []}

or, on failure,

    {"ok": false, "operation": "docs.get", "request_id": "...", "duration_ms": ...,
     "error": {"code": "DOC_NOT_FOUND", "message": "...", ...details},
     "suggestions": [...]}

The MCP server and the CLI both go through `DocsApi`, so they behave the same.
The envelope is deliberately the shape Studio RPC results should converge on.
"""

from __future__ import annotations

import itertools
import re
import time
from typing import Any, Callable

from .index import DocsError, DocsIndex, slugify

DEFAULT_MAX_CHARS = 24000
_HEADING = re.compile(r"^(#{1,6})[ \t]+(.+?)[ \t]*#*[ \t]*$")


def extract_section(body: str, section: str) -> str | None:
    """The Markdown section whose heading text or slug matches `section`."""
    wanted = slugify(section)
    lines = body.splitlines()
    in_fence = False
    start = level = None
    for number, line in enumerate(lines):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = _HEADING.match(line)
        if not match:
            continue
        depth = len(match.group(1))
        if start is None:
            if slugify(match.group(2)) == wanted:
                start, level = number, depth
        elif depth <= (level or 0):
            return "\n".join(lines[start:number])
    return "\n".join(lines[start:]) if start is not None else None


def headings(body: str) -> list[dict[str, Any]]:
    out = []
    in_fence = False
    for number, line in enumerate(body.splitlines(), start=1):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = _HEADING.match(line)
        if match and len(match.group(1)) in (2, 3):
            out.append({"level": len(match.group(1)), "title": match.group(2), "slug": slugify(match.group(2))})
    return out


class DocsApi:
    def __init__(self, index: DocsIndex) -> None:
        self.index = index
        self._ids = itertools.count(1)

    # -------------------------------------------------------------- plumbing
    def _run(self, operation: str, fn: Callable[[], tuple[Any, list[str], list[str]]]) -> dict[str, Any]:
        request_id = f"orbit-docs-{next(self._ids)}"
        started = time.perf_counter()
        try:
            reloaded = self.index.refresh()
            result, warnings, suggestions = fn()
            envelope: dict[str, Any] = {
                "ok": True,
                "operation": operation,
                "request_id": request_id,
                "duration_ms": round((time.perf_counter() - started) * 1000, 2),
                "index_reloaded": reloaded,
                "result": result,
                "warnings": warnings,
                "suggestions": suggestions,
            }
        except DocsError as error:
            details = dict(error.details)
            suggestions = []
            if "did_you_mean" in details and details["did_you_mean"]:
                suggestions.append("Did you mean: " + ", ".join(details["did_you_mean"]))
            if "hint" in details:
                suggestions.append(str(details.pop("hint")))
            envelope = {
                "ok": False,
                "operation": operation,
                "request_id": request_id,
                "duration_ms": round((time.perf_counter() - started) * 1000, 2),
                "error": {"code": error.code, "message": error.message, **details},
                "suggestions": suggestions,
            }
        return envelope

    # ------------------------------------------------------------ operations
    def root(self) -> dict[str, Any]:
        def go() -> tuple[Any, list[str], list[str]]:
            block = self.index.get_block("/")
            result = self.index.node(block)
            result["stats"] = self.index.stats()
            result.pop("body", None)
            return result, [], [
                "docs.for_task(\"what you are about to do\") for a ready-made starting packet.",
                "docs.children(path) / docs.get(path) to descend; docs.search(query) if unsure.",
            ]

        return self._run("docs.root", go)

    def get(
        self,
        path: str,
        section: str | None = None,
        offset: int = 0,
        max_chars: int = DEFAULT_MAX_CHARS,
    ) -> dict[str, Any]:
        def go() -> tuple[Any, list[str], list[str]]:
            block = self.index.get_block(path)
            node = self.index.node(block)
            body: str = node.get("body", "")
            warnings: list[str] = []
            node["headings"] = headings(body)
            if section:
                extracted = extract_section(body, section)
                if extracted is None:
                    raise DocsError(
                        "SECTION_NOT_FOUND",
                        f"{block.path} has no section {section!r}.",
                        available=[h["slug"] for h in node["headings"]],
                    )
                body = extracted
                node["section"] = slugify(section)
            total = len(body)
            end = min(total, max(0, offset) + max(1, max_chars))
            node["body"] = body[max(0, offset) : end]
            node["body_chars"] = {"offset": max(0, offset), "returned": end - max(0, offset), "total": total}
            suggestions: list[str] = []
            if end < total:
                node["truncated"] = True
                suggestions.append(f"Body truncated; call docs.get with offset={end} for the rest.")
            if block.legacy:
                warnings.append("Unstructured document: no invariants/sources recorded; verify against code.")
            freshness = node.get("freshness")
            if freshness and freshness.get("possibly_stale"):
                warnings.append(
                    "Source files changed since this block was verified: "
                    + ", ".join(freshness["sources_changed_since"][:5])
                )
            return node, warnings, suggestions

        return self._run("docs.get", go)

    def children(self, path: str) -> dict[str, Any]:
        def go() -> tuple[Any, list[str], list[str]]:
            block = self.index.get_block(path)
            return (
                {
                    "path": block.path,
                    "title": block.title,
                    "breadcrumbs": self.index.breadcrumbs(block.path),
                    "children": [self.index.card(c) for c in self.index.children(block.path)],
                },
                [],
                [],
            )

        return self._run("docs.children", go)

    def search(self, query: str, limit: int = 8, include_unstructured: bool = True) -> dict[str, Any]:
        def go() -> tuple[Any, list[str], list[str]]:
            hits = self.index.search(query, limit=max(1, min(limit, 30)), include_legacy=include_unstructured)
            suggestions = [] if hits else ["No matches; try fewer or more general words, or docs.root."]
            return {"query": query, "results": hits}, [], suggestions

        return self._run("docs.search", go)

    def for_target(self, target: str) -> dict[str, Any]:
        def go() -> tuple[Any, list[str], list[str]]:
            result = self.index.for_target(target)
            warnings = [result["note"]] if result.get("note") else []
            return result, warnings, ["docs.get(path) for any block listed, then follow /rules/placement."]

        return self._run("docs.for_target", go)

    def for_task(self, task: str, limit: int = 4) -> dict[str, Any]:
        def go() -> tuple[Any, list[str], list[str]]:
            return self.index.for_task(task, limit=max(1, min(limit, 10))), [], []

        return self._run("docs.for_task", go)

    def coverage(self) -> dict[str, Any]:
        def go() -> tuple[Any, list[str], list[str]]:
            report = self.index.coverage()
            suggestions = []
            if report["modules_uncovered"]:
                suggestions.append("Draft a card with `python tools/orbit_docs_cli.py scaffold <module>`.")
            return report, [], suggestions

        return self._run("docs.coverage", go)

    def check(self) -> dict[str, Any]:
        def go() -> tuple[Any, list[str], list[str]]:
            report = self.index.check()
            report["ok"] = not report["errors"]
            report["stats"] = self.index.stats()
            warnings = [f"{w['path']}: {w['message']}" for w in report["warnings"][:20]]
            return report, warnings, []

        return self._run("docs.check", go)
