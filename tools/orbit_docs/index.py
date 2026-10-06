"""Orbit documentation tree: loading, navigation, search, impact and checks.

Documentation is a tree of small blocks. A block is a Markdown file whose TOML
front matter (between `+++` lines) gives it a tree path, a summary, invariants,
source/symbol links, routing hints and diagnosis playbooks. See
docs/ORBIT_DOCS.md for the format.

Markdown files without front matter are still indexed ("legacy" blocks): each
file becomes `/legacy/<stem>` and each `##` section of it becomes a child
`/legacy/<stem>/<heading-slug>`, so the existing large documents are
searchable at section granularity before they are migrated.

This module has no third-party dependencies and never talks to Studio, so docs
are available before anything is built or launched. The index re-reads files
whose mtime changed, so edits are visible without a restart.
"""

from __future__ import annotations

import fnmatch
import math
import os
import re
import subprocess
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# Directories (relative to the repository root) searched for blocks.
SCAN_PATTERNS = (
    "docs/**/*.md",
    "code_change/*.md",
    "engine/*/docs/**/*.md",
    "apps/*/docs/**/*.md",
)

LEGACY_ROOT = "/legacy"
STATUSES = ("stable", "experimental", "planned", "draft", "historical", "unstructured")
KINDS = (
    "root",
    "section",
    "subsystem",
    "concept",
    "rule",
    "playbook",
    "reference",
    "placement",
    "meta",
)
_FRONT_MATTER = re.compile(r"\A\+\+\+\r?\n(.*?)\r?\n\+\+\+[ \t]*\r?\n?", re.DOTALL)
_WORD = re.compile(r"[a-z0-9_]+")
_HEADING = re.compile(r"^(#{1,6})[ \t]+(.+?)[ \t]*#*[ \t]*$")


class DocsError(Exception):
    """A request the docs index cannot satisfy (carries a stable code)."""

    def __init__(self, code: str, message: str, **details: Any) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details


def slugify(text: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return slug or "section"


def normalize_path(path: str) -> str:
    path = "/" + path.strip().strip("/")
    path = re.sub(r"/+", "/", path)
    return path


def parent_path(path: str) -> str | None:
    if path == "/":
        return None
    head = path.rsplit("/", 1)[0]
    return head or "/"


def _tokens(text: str) -> list[str]:
    out = []
    for word in _WORD.findall(text.lower()):
        out.append(word)
        # Split snake_case / camelCase-ish identifiers into extra tokens.
        if "_" in word:
            out.extend(part for part in word.split("_") if part)
        if len(word) > 3 and word.endswith("s"):
            out.append(word[:-1])
    return out


@dataclass
class Block:
    path: str
    title: str
    summary: str = ""
    body: str = ""
    kind: str = "concept"
    status: str = "stable"
    owner_module: str = ""
    keywords: list[str] = field(default_factory=list)
    sources: list[str] = field(default_factory=list)
    symbols: list[str] = field(default_factory=list)
    invariants: list[str] = field(default_factory=list)
    related: list[str] = field(default_factory=list)
    depends_on: list[str] = field(default_factory=list)
    used_by: list[str] = field(default_factory=list)
    verify: list[str] = field(default_factory=list)
    routes: dict[str, str] = field(default_factory=dict)
    diagnose: list[dict[str, Any]] = field(default_factory=list)
    applies_to: list[str] = field(default_factory=list)
    include_in_tasks: bool = False
    verified: str = ""
    order: int = 0
    # Where the block comes from.
    file: str = ""
    legacy: bool = False
    # For legacy section blocks: 1-based line range inside `file`.
    lines: tuple[int, int] | None = None

    @property
    def module_dir(self) -> str:
        parts = Path(self.file).parts
        if len(parts) >= 3 and parts[0] in ("engine", "apps") and parts[2] == "docs":
            return f"{parts[0]}/{parts[1]}"
        return ""


def _as_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    return [str(v) for v in value]


def _first_paragraph(text: str, limit: int = 320) -> str:
    paragraph: list[str] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            if paragraph:
                break
            continue
        if line.startswith(("#", "```", "|", "---", ">", "- ", "* ")) and not paragraph:
            continue
        paragraph.append(line)
    flat = " ".join(paragraph)
    flat = re.sub(r"\s+", " ", flat)
    return flat if len(flat) <= limit else flat[: limit - 1].rstrip() + "…"


def _block_from_front_matter(meta: dict[str, Any], body: str, file: str) -> Block:
    if "path" not in meta:
        raise DocsError("BLOCK_MISSING_PATH", f"{file}: front matter has no `path`")
    path = normalize_path(str(meta["path"]))
    routes = meta.get("routes", {}) or {}
    diagnose = meta.get("diagnose", []) or []
    return Block(
        path=path,
        title=str(meta.get("title") or path.rsplit("/", 1)[-1] or "Orbit"),
        summary=" ".join(str(meta.get("summary", "")).split()),
        body=body.strip("\n"),
        kind=str(meta.get("kind", "concept")),
        status=str(meta.get("status", "stable")),
        owner_module=str(meta.get("owner_module", "")),
        keywords=_as_list(meta.get("keywords")),
        sources=_as_list(meta.get("sources")),
        symbols=_as_list(meta.get("symbols")),
        invariants=_as_list(meta.get("invariants")),
        related=_as_list(meta.get("related")),
        depends_on=_as_list(meta.get("depends_on")),
        used_by=_as_list(meta.get("used_by")),
        verify=_as_list(meta.get("verify")),
        routes={str(k): str(v) for k, v in routes.items()},
        diagnose=[dict(d) for d in diagnose],
        applies_to=_as_list(meta.get("applies_to")),
        include_in_tasks=bool(meta.get("include_in_tasks", False)),
        verified=str(meta.get("verified", "")),
        order=int(meta.get("order", 0)),
        file=file,
        legacy=False,
    )


def _legacy_blocks(text: str, file: str) -> list[Block]:
    """A file without front matter -> one block plus one block per `##` section."""
    rel = Path(file)
    parts = list(rel.with_suffix("").parts)
    if parts[0] == "docs":
        parts = parts[1:]
    elif parts[0] in ("engine", "apps") and len(parts) > 2 and parts[2] == "docs":
        parts = [parts[1], *parts[3:]]
    root_path = f"{LEGACY_ROOT}/{slugify('-'.join(parts))}"

    lines = text.splitlines()
    title = rel.stem
    for line in lines:
        m = _HEADING.match(line)
        if m and len(m.group(1)) == 1:
            title = m.group(2)
            break

    # Section starts (`##` headings, ignoring fenced code).
    starts: list[tuple[int, str]] = []
    in_fence = False
    for number, line in enumerate(lines, start=1):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        m = _HEADING.match(line)
        if m and len(m.group(1)) == 2:
            starts.append((number, m.group(2)))

    intro_end = (starts[0][0] - 1) if starts else len(lines)
    intro = "\n".join(lines[:intro_end])
    blocks = [
        Block(
            path=root_path,
            title=title,
            summary=_first_paragraph(intro) or title,
            body="\n".join(lines),
            kind="reference",
            status="unstructured",
            file=file,
            legacy=True,
            lines=(1, len(lines)),
        )
    ]
    seen: dict[str, int] = {}
    for index, (number, heading) in enumerate(starts):
        end = starts[index + 1][0] - 1 if index + 1 < len(starts) else len(lines)
        section = "\n".join(lines[number - 1 : end])
        slug = slugify(heading)
        seen[slug] = seen.get(slug, 0) + 1
        if seen[slug] > 1:
            slug = f"{slug}-{seen[slug]}"
        blocks.append(
            Block(
                path=f"{root_path}/{slug}",
                title=heading,
                summary=_first_paragraph("\n".join(lines[number:end])) or heading,
                body=section,
                kind="reference",
                status="unstructured",
                file=file,
                legacy=True,
                lines=(number, end),
                order=index,
            )
        )
    return blocks


def parse_file(root: Path, rel: str) -> list[Block]:
    text = (root / rel).read_text(encoding="utf-8")
    match = _FRONT_MATTER.match(text)
    if match is None:
        return _legacy_blocks(text, rel)
    try:
        meta = tomllib.loads(match.group(1))
    except tomllib.TOMLDecodeError as error:
        raise DocsError("BLOCK_BAD_FRONT_MATTER", f"{rel}: {error}", file=rel) from error
    return [_block_from_front_matter(meta, text[match.end() :], rel)]


class DocsIndex:
    """All blocks of a repository, refreshed when files change."""

    def __init__(self, root: str | os.PathLike[str]) -> None:
        self.root = Path(root).resolve()
        self.blocks: dict[str, Block] = {}
        self.children_of: dict[str, list[str]] = {}
        self.load_errors: list[dict[str, str]] = []
        self.duplicates: list[dict[str, str]] = []
        self._signature: tuple[Any, ...] | None = None
        self._df: dict[str, int] = {}
        self.refresh()

    # ------------------------------------------------------------------ loading
    def _files(self) -> list[str]:
        found: set[str] = set()
        for pattern in SCAN_PATTERNS:
            for path in self.root.glob(pattern):
                if path.is_file():
                    found.add(path.relative_to(self.root).as_posix())
        return sorted(found)

    def refresh(self) -> bool:
        """Reload when any scanned file changed. Returns True if it did."""
        files = self._files()
        signature = tuple(
            (f, (st := (self.root / f).stat()).st_mtime_ns, st.st_size) for f in files
        )
        if signature == self._signature:
            return False
        self._signature = signature
        self.blocks = {}
        self.load_errors = []
        self.duplicates = []
        for rel in files:
            try:
                parsed = parse_file(self.root, rel)
            except DocsError as error:
                self.load_errors.append({"file": rel, "code": error.code, "message": error.message})
                continue
            for block in parsed:
                existing = self.blocks.get(block.path)
                if existing is not None:
                    self.duplicates.append(
                        {"path": block.path, "files": f"{existing.file} | {block.file}"}
                    )
                    continue
                self.blocks[block.path] = block
        self._link()
        return True

    def _link(self) -> None:
        self.children_of = {}
        # Legacy tree hangs off a synthetic /legacy node unless authored.
        if LEGACY_ROOT not in self.blocks and any(b.legacy for b in self.blocks.values()):
            self.blocks[LEGACY_ROOT] = Block(
                path=LEGACY_ROOT,
                title="Unstructured documents",
                summary=(
                    "Existing Markdown documents that have no block front matter yet. "
                    "Each file is indexed whole, and each `##` section is its own child, so large "
                    "documents can be read one section at a time. Prefer structured nodes; "
                    "use these when the tree has no structured answer."
                ),
                kind="section",
                status="unstructured",
                legacy=True,
            )
        for path, block in self.blocks.items():
            parent = parent_path(path)
            if parent is not None:
                self.children_of.setdefault(parent, []).append(path)
        for siblings in self.children_of.values():
            siblings.sort(key=lambda p: (self.blocks[p].order, p))
        self._build_search()

    # ----------------------------------------------------------------- lookups
    def get_block(self, path: str) -> Block:
        self.refresh()
        path = normalize_path(path)
        block = self.blocks.get(path)
        if block is None:
            raise DocsError(
                "DOC_NOT_FOUND",
                f"No documentation node at {path!r}.",
                path=path,
                did_you_mean=self._suggest(path),
                hint="Call docs.root and descend with docs.children, or docs.search.",
            )
        return block

    def _suggest(self, path: str, limit: int = 5) -> list[str]:
        last = slugify(path.rsplit("/", 1)[-1])
        scored = []
        for candidate in self.blocks:
            tail = candidate.rsplit("/", 1)[-1]
            score = 0
            if tail == last:
                score += 3
            if last and (last in candidate or tail in path):
                score += 1
            parts_overlap = len(set(candidate.split("/")) & set(path.split("/")))
            score += parts_overlap
            if score:
                scored.append((score, candidate))
        scored.sort(key=lambda s: (-s[0], s[1]))
        return [c for _, c in scored[:limit]]

    def breadcrumbs(self, path: str) -> list[dict[str, str]]:
        crumbs = []
        pieces = [p for p in path.split("/") if p]
        current = ""
        candidates = ["/"] + [(current := f"{current}/{piece}") for piece in pieces]
        for candidate in candidates:
            block = self.blocks.get(candidate)
            crumbs.append({"path": candidate, "title": block.title if block else candidate})
        return crumbs

    def children(self, path: str) -> list[Block]:
        self.refresh()
        path = normalize_path(path)
        if path not in self.blocks:
            self.get_block(path)
        return [self.blocks[p] for p in self.children_of.get(path, [])]

    def resolve_target(self, base: str, target: str) -> str:
        """Route targets are absolute paths or names of children of `base`."""
        if target.startswith("/"):
            return normalize_path(target)
        return normalize_path(f"{base}/{target}")

    # ----------------------------------------------------------------- summary
    def card(self, block: Block) -> dict[str, Any]:
        """Smallest useful description of a node (used for children/search)."""
        return {
            "path": block.path,
            "title": block.title,
            "summary": block.summary,
            "kind": block.kind,
            "status": block.status,
            "children": len(self.children_of.get(block.path, [])),
        }

    def node(self, block: Block, include_body: bool = True) -> dict[str, Any]:
        out: dict[str, Any] = {
            "path": block.path,
            "title": block.title,
            "kind": block.kind,
            "status": block.status,
            "summary": block.summary,
            "breadcrumbs": self.breadcrumbs(block.path),
            "children": [self.card(c) for c in self.children(block.path)],
        }
        if block.owner_module:
            out["owner_module"] = block.owner_module
        if block.invariants:
            out["invariants"] = block.invariants
        if block.routes:
            out["where_to_go"] = [
                {
                    "if": symptom,
                    "go": (resolved := self.resolve_target(block.path, target)),
                    "title": self.blocks[resolved].title if resolved in self.blocks else None,
                }
                for symptom, target in block.routes.items()
            ]
        if block.diagnose:
            out["diagnose"] = block.diagnose
        if block.verify:
            out["verify"] = block.verify
        if block.sources:
            out["sources"] = block.sources
        if block.symbols:
            out["symbols"] = block.symbols
        for name in ("related", "depends_on", "used_by"):
            values = getattr(block, name)
            if values:
                out[name] = values
        if block.applies_to:
            out["applies_to"] = block.applies_to
        out["file"] = block.file
        if block.lines:
            out["lines"] = list(block.lines)
        freshness = self.freshness(block)
        if freshness is not None:
            out["freshness"] = freshness
        if include_body:
            out["body"] = block.body
        return out

    # --------------------------------------------------------------- freshness
    def freshness(self, block: Block) -> dict[str, Any] | None:
        """Source files changed since `verified` (needs git; None if unknown)."""
        if not block.verified or not block.sources:
            return None
        try:
            result = subprocess.run(
                [
                    "git",
                    "-C",
                    str(self.root),
                    "log",
                    "--name-only",
                    "--format=",
                    f"{block.verified}..HEAD",
                    "--",
                    *block.sources,
                ],
                capture_output=True,
                text=True,
                timeout=10,
                check=True,
            )
        except (OSError, subprocess.SubprocessError):
            return None
        changed = sorted({line for line in result.stdout.splitlines() if line.strip()})
        return {
            "verified": block.verified,
            "sources_changed_since": changed,
            "possibly_stale": bool(changed),
        }

    # ------------------------------------------------------------------ search
    def _fields(self, block: Block) -> list[tuple[str, float]]:
        weak = 0.5 if block.legacy else 1.0
        fields: list[tuple[str, float]] = [
            (block.title, 5.0),
            (" ".join(block.keywords), 4.0),
            (block.path.replace("/", " ").replace("-", " "), 3.0),
            (block.summary, 2.0),
            (" ".join(block.routes), 3.0),
            (" ".join(d.get("symptom", "") for d in block.diagnose), 3.0),
            (" ".join(block.symbols), 3.0),
            (" ".join(block.invariants), 1.5),
            (self._body_for_search(block), 1.0 * weak),
        ]
        return fields

    def _body_for_search(self, block: Block) -> str:
        # A legacy file that is split into sections indexes only its outline;
        # the section nodes carry the text, so hits stay small and specific.
        if block.legacy and self.children_of.get(block.path):
            return " ".join(self.blocks[c].title for c in self.children_of[block.path])
        return block.body

    def _build_search(self) -> None:
        self._postings: dict[str, dict[str, float]] = {}
        self._df = {}
        for path, block in self.blocks.items():
            weights: dict[str, float] = {}
            for text, weight in self._fields(block):
                for token in set(_tokens(text)):
                    weights[token] = max(weights.get(token, 0.0), weight)
            for token in weights:
                self._df[token] = self._df.get(token, 0) + 1
            self._postings[path] = weights
        self._n = max(1, len(self.blocks))

    def search(self, query: str, limit: int = 8, include_legacy: bool = True) -> list[dict[str, Any]]:
        self.refresh()
        terms = list(dict.fromkeys(_tokens(query)))
        if not terms:
            raise DocsError("EMPTY_QUERY", "The search query has no searchable words.")
        needle = " ".join(query.lower().split())
        scored: list[tuple[float, Block, list[str]]] = []
        for path, block in self.blocks.items():
            if block.legacy and not include_legacy:
                continue
            postings = self._postings.get(path, {})
            score = 0.0
            hit: list[str] = []
            for term in terms:
                weight = postings.get(term)
                if weight is None:
                    continue
                idf = math.log(1.0 + self._n / (1 + self._df.get(term, 0)))
                score += weight * idf
                hit.append(term)
            if not score:
                continue
            # Phrase bonus: the whole query appears in a symptom/route/summary.
            haystack = " ".join(
                [block.summary, *block.routes, *(d.get("symptom", "") for d in block.diagnose)]
            ).lower()
            if len(needle) > 6 and needle in haystack:
                score += 12.0
            coverage = len(set(hit)) / len(terms)
            score *= 0.5 + coverage
            if block.status in ("historical", "unstructured"):
                score *= 0.7
            scored.append((score, block, hit))
        scored.sort(key=lambda s: (-s[0], s[1].path))
        results = []
        for score, block, hit in scored[:limit]:
            card = self.card(block)
            card["score"] = round(score, 2)
            card["matched"] = sorted(set(hit))
            results.append(card)
        return results

    # ----------------------------------------------------------------- impact
    def for_target(self, target: str) -> dict[str, Any]:
        """Docs for a source file, directory, module or symbol."""
        self.refresh()
        raw = target.strip().replace("\\", "/")
        rel = raw
        absolute = Path(raw)
        if absolute.is_absolute():
            try:
                rel = absolute.resolve().relative_to(self.root).as_posix()
            except ValueError:
                pass
        rel = rel.lstrip("./") if rel.startswith("./") else rel
        symbol_tail = re.split(r"::|\.", raw)[-1]
        matches: list[tuple[int, str, Block]] = []
        for block in self.blocks.values():
            if block.legacy:
                continue
            reason = ""
            rank = 99
            for source in block.sources:
                if source == rel:
                    reason, rank = f"source file {source}", 0
                    break
                if source.endswith("/") and rel.startswith(source):
                    reason, rank = f"source directory {source}", 1
                    break
            for symbol in block.symbols:
                if symbol == raw or symbol.split("::")[-1] == symbol_tail and "/" not in raw:
                    if rank > 0:
                        reason, rank = f"symbol {symbol}", min(rank, 1)
            module_dir = block.module_dir
            if rank == 99 and module_dir and (rel == module_dir or rel.startswith(module_dir + "/")):
                reason, rank = f"module {module_dir}", 2
            if rank < 99:
                matches.append((rank, reason, block))
        matches.sort(key=lambda m: (m[0], m[2].path))
        rules = [
            b
            for b in self.blocks.values()
            if b.applies_to and any(fnmatch.fnmatch(rel, pattern) for pattern in b.applies_to)
        ]
        direct = [
            {**self.card(b), "why": reason, "invariants": b.invariants, "verify": b.verify}
            for _, reason, b in matches
        ]
        neighbours: dict[str, dict[str, Any]] = {}
        for _, _, block in matches:
            for relation in ("depends_on", "used_by"):
                for other in getattr(block, relation):
                    other = normalize_path(other)
                    if other in self.blocks and all(other != m[2].path for m in matches):
                        entry = neighbours.setdefault(
                            other, {**self.card(self.blocks[other]), "relations": []}
                        )
                        entry["relations"].append(f"{block.path} {relation.replace('_', ' ')} it")
        return {
            "target": raw,
            "normalized": rel,
            "docs": direct,
            "also_affected": list(neighbours.values()),
            "rules": [
                {**self.card(b), "invariants": b.invariants, "applies_to": b.applies_to}
                for b in sorted(rules, key=lambda b: b.path)
            ],
            "note": None if direct else "No block references this target yet; "
            "consider adding it to a block's `sources`/`symbols`.",
        }

    # ------------------------------------------------------------------- task
    def for_task(self, task: str, limit: int = 4) -> dict[str, Any]:
        """One compact packet of what an agent should know before starting."""
        hits = self.search(task, limit=limit * 3)
        structured = [h for h in hits if self.blocks[h["path"]].status not in ("unstructured",)]
        chosen = (structured or hits)[:limit]
        sections = []
        for hit in chosen:
            block = self.blocks[hit["path"]]
            sections.append(
                {
                    **self.card(block),
                    "score": hit["score"],
                    "invariants": block.invariants,
                    "where_to_go": [
                        {"if": s, "go": self.resolve_target(block.path, t)}
                        for s, t in block.routes.items()
                    ],
                    "diagnose": [d for d in block.diagnose],
                    "verify": block.verify,
                    "sources": block.sources,
                    "symbols": block.symbols,
                }
            )
        extra = [h for h in hits if h["path"] not in {c["path"] for c in chosen}][:limit]
        always = [
            {
                **self.card(b),
                "invariants": b.invariants,
            }
            for b in sorted(self.blocks.values(), key=lambda b: b.path)
            if b.include_in_tasks
        ]
        return {
            "task": task,
            "start_here": sections,
            "more_matches": extra,
            "always_applicable_rules": always,
            "next": [
                "docs.get(path) for the nodes above (body holds the details).",
                "docs.for_target(file_or_symbol) before editing a file, to see invariants and neighbours.",
                "Write the placement note required by /rules/placement before changing behavior.",
            ],
        }

    # ------------------------------------------------------------------ checks
    def check(self) -> dict[str, list[dict[str, str]]]:
        """Structural validation. `errors` should fail CI; `warnings` are advice."""
        self.refresh()
        errors: list[dict[str, str]] = []
        warnings: list[dict[str, str]] = []

        def error(path: str, code: str, message: str) -> None:
            errors.append({"path": path, "code": code, "message": message})

        def warn(path: str, code: str, message: str) -> None:
            warnings.append({"path": path, "code": code, "message": message})

        for item in self.load_errors:
            error(item["file"], item["code"], item["message"])
        for item in self.duplicates:
            error(item["path"], "DUPLICATE_PATH", f"two files claim this path: {item['files']}")
        if "/" not in self.blocks:
            error("/", "NO_ROOT", "There is no root node (a block with path = \"/\").")

        word_cache: dict[str, str] = {}

        def file_text(rel: str) -> str | None:
            if rel not in word_cache:
                target = self.root / rel
                try:
                    word_cache[rel] = target.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    return None
            return word_cache[rel]

        for path, block in self.blocks.items():
            if block.legacy:
                continue
            if block.kind not in KINDS:
                error(path, "BAD_KIND", f"kind {block.kind!r} is not one of {KINDS}")
            if block.status not in STATUSES:
                error(path, "BAD_STATUS", f"status {block.status!r} is not one of {STATUSES}")
            if not block.summary:
                error(path, "NO_SUMMARY", "every node needs a summary (it is what parents show)")
            elif len(block.summary) > 600:
                warn(path, "LONG_SUMMARY", f"summary is {len(block.summary)} chars; keep it short")
            parent = parent_path(path)
            if parent is not None and parent not in self.blocks:
                error(path, "NO_PARENT", f"parent node {parent} does not exist")
            words = len(block.body.split())
            if words > 1200:
                warn(path, "LARGE_BLOCK", f"{words} words; consider splitting (target 200-800)")
            if block.kind not in ("root", "section", "meta") and words < 40 and not block.summary:
                warn(path, "THIN_BLOCK", f"only {words} words")
            for relation in ("related", "depends_on", "used_by"):
                for other in getattr(block, relation):
                    if normalize_path(other) not in self.blocks:
                        error(path, "BROKEN_LINK", f"{relation} -> {other} does not exist")
            for symptom, target in block.routes.items():
                resolved = self.resolve_target(path, target)
                if resolved not in self.blocks:
                    error(path, "BROKEN_ROUTE", f"route {symptom!r} -> {target} ({resolved}) does not exist")
            for index, item in enumerate(block.diagnose):
                if not item.get("symptom") or not item.get("steps"):
                    error(path, "BAD_DIAGNOSE", f"diagnose[{index}] needs `symptom` and `steps`")
                for target in _as_list(item.get("docs")):
                    if normalize_path(target) not in self.blocks:
                        error(path, "BROKEN_LINK", f"diagnose[{index}].docs -> {target} does not exist")
            source_texts = []
            for source in block.sources:
                target = self.root / source
                if not target.exists():
                    error(path, "MISSING_SOURCE", f"source {source} does not exist")
                    continue
                if target.is_file():
                    text = file_text(source)
                    if text is not None:
                        source_texts.append(text)
            for symbol in block.symbols:
                tail = symbol.split("::")[-1]
                if not source_texts:
                    warn(path, "UNCHECKED_SYMBOL", f"symbol {symbol} has no source file to check against")
                    continue
                pattern = re.compile(rf"\b{re.escape(tail)}\b")
                if not any(pattern.search(text) for text in source_texts):
                    error(path, "MISSING_SYMBOL", f"symbol {symbol} not found in this block's sources")
            if block.verified and not re.fullmatch(r"[0-9a-f]{7,40}", block.verified):
                error(path, "BAD_VERIFIED", "verified must be a git commit sha")
            if block.kind in ("subsystem", "concept") and block.status == "stable" and not block.sources:
                warn(path, "NO_SOURCES", "stable subsystem/concept blocks should link their source files")
            if block.kind in ("subsystem", "concept") and block.status == "stable" and not block.invariants:
                warn(path, "NO_INVARIANTS", "stable subsystem/concept blocks should state invariants")

        return {"errors": errors, "warnings": warnings}

    def stats(self) -> dict[str, Any]:
        self.refresh()
        structured = [b for b in self.blocks.values() if not b.legacy]
        return {
            "root": str(self.root),
            "structured_nodes": len(structured),
            "legacy_nodes": len(self.blocks) - len(structured),
            "files": len({b.file for b in self.blocks.values() if b.file}),
        }


def default_root() -> Path:
    env = os.environ.get("ORBIT_REPO_ROOT")
    if env:
        return Path(env)
    return Path(__file__).resolve().parents[2]
