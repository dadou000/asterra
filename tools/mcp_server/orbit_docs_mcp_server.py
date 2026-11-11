"""MCP server for Orbit's documentation tree.

Read-only and offline: it reads the Markdown blocks in the repository directly
and never talks to Orbit Studio, so an agent can learn the architecture before
anything is built or launched. Edits to documentation files are picked up on
the next call (no restart).

Usage:
    pip install -r tools/mcp_server/requirements.txt
    python tools/mcp_server/orbit_docs_mcp_server.py

Register it with Claude Code:
    claude mcp add orbit-docs -- python <repo>/tools/mcp_server/orbit_docs_mcp_server.py

`ORBIT_REPO_ROOT` overrides the repository root (default: this file's repo).
The block format and the tool contracts are documented in docs/ORBIT_DOCS.md.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from orbit_docs import DocsIndex, default_root  # noqa: E402
from orbit_docs.api import DEFAULT_MAX_CHARS, DocsApi  # noqa: E402

try:
    # MCP Python SDK v2 public API.
    from mcp.server import MCPServer as _Server
except (ImportError, ModuleNotFoundError):
    # MCP Python SDK v1 fallback.
    from mcp.server.fastmcp import FastMCP as _Server

mcp = _Server("orbit-docs")
_api = DocsApi(DocsIndex(default_root()))


def _out(envelope: dict[str, Any]) -> str:
    return json.dumps(envelope, indent=1, ensure_ascii=False)


@mcp.tool()
def orbit_docs_root() -> str:
    """Start here. Root of the Orbit documentation tree: engine summary, the main
    subsystems with one-line summaries, 'if you are here because of X go to Y'
    routing, and index statistics. Descend with orbit_docs_children/orbit_docs_get."""
    return _out(_api.root())


@mcp.tool()
def orbit_docs_for_task(task: str, limit: int = 4) -> str:
    """One compact starting packet for a task described in plain words (for example
    'terrain moves when the camera moves' or 'add a viewport toolbar button'):
    the best-matching nodes with their invariants, routing hints, diagnosis
    playbooks, verification steps, source files and symbols, plus the rules that
    always apply. Call this before reading code or whole documents."""
    return _out(_api.for_task(task, limit))


@mcp.tool()
def orbit_docs_get(
    path: str,
    section: str | None = None,
    offset: int = 0,
    max_chars: int = DEFAULT_MAX_CHARS,
) -> str:
    """Read one documentation node by tree path (for example
    '/rendering/terrain/clipmaps'): summary, breadcrumbs, children, invariants,
    where-to-go routing, diagnosis playbooks, verification, source links, freshness
    and the Markdown body. `section` limits the body to one heading (title or slug);
    long bodies are paged with offset/max_chars."""
    return _out(_api.get(path, section, offset, max_chars))


@mcp.tool()
def orbit_docs_children(path: str = "/") -> str:
    """List the direct children of a node with their summaries (cheap navigation)."""
    return _out(_api.children(path))


@mcp.tool()
def orbit_docs_search(query: str, limit: int = 8, include_unstructured: bool = True) -> str:
    """Ranked search over titles, keywords, symptoms, symbols, invariants and text.
    Large legacy documents are indexed per section, so hits are small. Use plain
    symptom words ('rings do not line up'); returns cards, not bodies."""
    return _out(_api.search(query, limit, include_unstructured))


@mcp.tool()
def orbit_docs_for_target(target: str) -> str:
    """Impact analysis before editing: given a repo-relative file path, a directory,
    an engine module path or a symbol name (for example
    'engine/terrain_view/src/ClipmapPlanner.cpp' or 'ClipmapPlanner'), return the
    blocks that own it, their invariants and verification steps, neighbouring
    blocks that depend on or use it, and the rules that apply to that path."""
    return _out(_api.for_target(target))


@mcp.tool()
def orbit_docs_coverage() -> str:
    """What the structured tree does not reach yet: engine/apps modules without a card and
    legacy documents no structured node links to. Use it to find undocumented areas."""
    return _out(_api.coverage())


@mcp.tool()
def orbit_docs_check() -> str:
    """Validate the documentation tree: parents, links, routes, source files and
    symbols that must exist, required fields. Errors fail CI; warnings are advice."""
    return _out(_api.check())


if __name__ == "__main__":
    mcp.run()
