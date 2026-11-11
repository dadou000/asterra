#!/usr/bin/env python3
"""Command-line access to the Orbit documentation tree.

    python tools/orbit_docs_cli.py check            # validate (exit 1 on errors)
    python tools/orbit_docs_cli.py tree [path]      # print the tree
    python tools/orbit_docs_cli.py get /rendering   # one node
    python tools/orbit_docs_cli.py search "terrain moves with camera"
    python tools/orbit_docs_cli.py task "add a viewport toolbar button"
    python tools/orbit_docs_cli.py target engine/terrain_view/src/ClipmapPlanner.cpp
    python tools/orbit_docs_cli.py stale            # blocks whose sources changed
    python tools/orbit_docs_cli.py coverage         # modules/documents the tree does not reach
    python tools/orbit_docs_cli.py scaffold engine/foo --path /rendering/foo   # draft a card

Same code as the MCP server (tools/mcp_server/orbit_docs_mcp_server.py).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from orbit_docs import DocsIndex, default_root  # noqa: E402
from orbit_docs.api import DocsApi  # noqa: E402


def _print_tree(index: DocsIndex, path: str, depth: int, max_depth: int, structured_only: bool) -> None:
    block = index.blocks[path]
    marker = {"unstructured": "~", "historical": "h", "experimental": "x", "planned": "p", "draft": "d"}.get(
        block.status, " "
    )
    print(f"{'  ' * depth}{marker} {path.rsplit('/', 1)[-1] or '/'}  — {block.title}")
    if depth >= max_depth:
        return
    for child in index.children(path):
        if structured_only and child.legacy:
            continue
        _print_tree(index, child.path, depth + 1, max_depth, structured_only)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=None, help="repository root (default: auto)")
    sub = parser.add_subparsers(dest="command", required=True)

    check = sub.add_parser("check", help="validate the tree")
    check.add_argument("--strict", action="store_true", help="treat warnings as errors")
    tree = sub.add_parser("tree", help="print the tree")
    tree.add_argument("path", nargs="?", default="/")
    tree.add_argument("--depth", type=int, default=4)
    tree.add_argument("--all", action="store_true", help="include unstructured (legacy) nodes")
    get = sub.add_parser("get", help="print one node as JSON")
    get.add_argument("path")
    get.add_argument("--section")
    search = sub.add_parser("search", help="search the tree")
    search.add_argument("query")
    search.add_argument("--limit", type=int, default=8)
    task = sub.add_parser("task", help="starting packet for a task")
    task.add_argument("task")
    target = sub.add_parser("target", help="docs for a file/module/symbol")
    target.add_argument("target")
    sub.add_parser("stale", help="structured blocks whose sources changed since `verified`")
    cov = sub.add_parser("coverage", help="engine modules and documents the structured tree does not reach")
    cov.add_argument("--fail-under", type=float, default=None, help="exit 1 if module coverage percent is below this")
    cov.add_argument("--require-modules", action="store_true", help="exit 1 if any engine/apps module has no card")
    scaf = sub.add_parser("scaffold", help="print a draft card for a module (engine/<m> or apps/<m>)")
    scaf.add_argument("module")
    scaf.add_argument("--path", default=None, help="tree path for the new node")
    args = parser.parse_args(argv)

    index = DocsIndex(args.root or default_root())
    api = DocsApi(index)

    if args.command == "check":
        report = index.check()
        for item in report["errors"]:
            print(f"error   {item['path']}: [{item['code']}] {item['message']}")
        for item in report["warnings"]:
            print(f"warning {item['path']}: [{item['code']}] {item['message']}")
        stats = index.stats()
        print(
            f"{stats['structured_nodes']} structured nodes, {stats['legacy_nodes']} unstructured, "
            f"{len(report['errors'])} errors, {len(report['warnings'])} warnings"
        )
        failed = bool(report["errors"]) or (args.strict and bool(report["warnings"]))
        return 1 if failed else 0
    if args.command == "tree":
        _print_tree(index, args.path, 0, args.depth, not args.all)
        return 0
    if args.command == "coverage":
        report = index.coverage()
        done = report["modules_total"] - len(report["modules_uncovered"])
        percent = 100.0 * done / max(1, report["modules_total"])
        print(f"modules: {done}/{report['modules_total']} covered ({percent:.0f}%)")
        for module in report["modules_uncovered"]:
            print(f"  uncovered module: {module}")
        linked = report["documents_total"] - len(report["documents_unlinked"])
        print(f"documents: {linked}/{report['documents_total']} linked from the structured tree")
        for doc in report["documents_unlinked"]:
            print(f"  unlinked document: {doc['path']} ({doc['sections']} sections) {doc['file']}")
        if args.fail_under is not None and percent < args.fail_under:
            return 1
        if args.require_modules and report["modules_uncovered"]:
            return 1
        return 0
    if args.command == "scaffold":
        from orbit_docs.scaffold import scaffold

        try:
            print(scaffold(index, args.module.rstrip("/"), args.path))
        except ValueError as error:
            print(f"error: {error}", file=sys.stderr)
            return 1
        return 0
    if args.command == "stale":
        stale = 0
        for block in index.blocks.values():
            fresh = index.freshness(block)
            if fresh and fresh["possibly_stale"]:
                stale += 1
                print(f"{block.path}: {', '.join(fresh['sources_changed_since'])}")
        print(f"{stale} possibly stale block(s)")
        return 0

    result = {
        "get": lambda: api.get(args.path, args.section),
        "search": lambda: api.search(args.query, args.limit),
        "task": lambda: api.for_task(args.task),
        "target": lambda: api.for_target(args.target),
    }[args.command]()
    print(json.dumps(result, indent=1, ensure_ascii=False))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
