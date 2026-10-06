"""Tests for the Orbit documentation tree (tools/orbit_docs).

    python -m unittest discover -s tools/tests -p "test_orbit_docs.py"
"""

from __future__ import annotations

import os
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from orbit_docs import DocsError, DocsIndex  # noqa: E402
from orbit_docs.api import DocsApi, extract_section, headings  # noqa: E402
from orbit_docs.scaffold import TODO_MARKER, scaffold  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]


def block(path: str, title: str, summary: str = "A summary.", extra: str = "", body: str = "Body text.") -> str:
    return f'+++\npath = "{path}"\ntitle = "{title}"\nsummary = "{summary}"\n{extra}\n+++\n\n{body}\n'


class TempRepo:
    def __init__(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.root = Path(self._dir.name)

    def write(self, rel: str, text: str) -> Path:
        target = self.root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(textwrap.dedent(text) if text.startswith("\n") else text, encoding="utf-8")
        return target

    def cleanup(self) -> None:
        self._dir.cleanup()


class DocsIndexTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = TempRepo()
        self.addCleanup(self.repo.cleanup)
        r = self.repo
        r.write("docs/tree/index.md", block("/", "Root", "Engine root.", 'kind = "root"\n[routes]\n"terrain looks wrong" = "/gfx/terrain"'))
        r.write("docs/tree/gfx.md", block("/gfx", "Graphics", "Rendering.", 'kind = "section"'))
        r.write("engine/terrain/src/Planner.cpp", "class Planner { void Plan(); };\n")
        r.write("engine/terrain/include/Other.hpp", "struct Other {};\n")
        r.write(
            "engine/terrain/docs/terrain.md",
            block(
                "/gfx/terrain",
                "Terrain",
                "Planet terrain drawn by clipmaps.",
                'kind = "subsystem"\nkeywords = ["clipmap"]\n'
                'sources = ["engine/terrain/src/Planner.cpp"]\nsymbols = ["Planner"]\n'
                'invariants = ["Levels never overlap."]\nused_by = ["/gfx/ui"]\n'
                '[routes]\n"rings do not line up" = "phase"\n'
                '[[diagnose]]\nsymptom = "holes in the terrain"\nsteps = ["switch on hole view"]\ndocs = ["/gfx/terrain/phase"]',
            ),
        )
        r.write(
            "engine/terrain/docs/phase.md",
            block("/gfx/terrain/phase", "Phase alignment", "Adjacent rings share one lattice.", 'kind = "concept"\nstatus = "experimental"', "Rings snap to a shared lattice."),
        )
        r.write("docs/tree/ui.md", block("/gfx/ui", "Viewport UI", "Panels.", 'kind = "subsystem"\ndepends_on = ["/gfx/terrain"]'))
        r.write(
            "docs/OLD_NOTES.md",
            "# Old notes\n\nIntro about legacy things.\n\n## Water pass\n\nWater is drawn flat.\n\n"
            "```\n## not a heading\n```\n\n## Cloud march\n\nClouds are ray marched at half resolution.\n",
        )
        self.index = DocsIndex(r.root)
        self.api = DocsApi(self.index)

    # ------------------------------------------------------------- structure
    def test_tree_navigation_and_breadcrumbs(self) -> None:
        children = [c.path for c in self.index.children("/gfx/terrain")]
        self.assertEqual(children, ["/gfx/terrain/phase"])
        crumbs = [c["path"] for c in self.index.breadcrumbs("/gfx/terrain/phase")]
        self.assertEqual(crumbs, ["/", "/gfx", "/gfx/terrain", "/gfx/terrain/phase"])

    def test_root_routes_resolve_and_node_has_where_to_go(self) -> None:
        node = self.index.node(self.index.get_block("/"))
        self.assertEqual(node["where_to_go"][0]["go"], "/gfx/terrain")
        self.assertEqual(node["where_to_go"][0]["title"], "Terrain")

    def test_clean_repo_passes_check(self) -> None:
        report = self.index.check()
        self.assertEqual(report["errors"], [], report)

    def test_legacy_sections_are_children_and_ignore_fenced_headings(self) -> None:
        kids = [c.path for c in self.index.children("/legacy/old-notes")]
        self.assertEqual(kids, ["/legacy/old-notes/water-pass", "/legacy/old-notes/cloud-march"])
        section = self.index.get_block("/legacy/old-notes/water-pass")
        self.assertIn("not a heading", section.body)  # fenced line stays inside the section
        self.assertEqual(section.status, "unstructured")

    # ----------------------------------------------------------------- search
    def test_search_finds_symptom_and_keyword(self) -> None:
        top = self.index.search("holes in the terrain")[0]["path"]
        self.assertEqual(top, "/gfx/terrain")
        self.assertEqual(self.index.search("clipmap")[0]["path"], "/gfx/terrain")

    def test_search_reaches_legacy_sections(self) -> None:
        top = self.index.search("clouds ray marched resolution")[0]["path"]
        self.assertEqual(top, "/legacy/old-notes/cloud-march")

    def test_stopwords_alone_are_an_empty_query(self) -> None:
        with self.assertRaises(DocsError) as caught:
            self.index.search("how do I the of")
        self.assertEqual(caught.exception.code, "EMPTY_QUERY")

    def test_specific_leaf_outranks_router_section(self) -> None:
        # The root's route table mentions "terrain looks wrong"; the leaf that actually
        # documents the symptom must still win over the router.
        top = self.index.search("terrain looks wrong holes")[0]["path"]
        self.assertEqual(top, "/gfx/terrain")

    def test_empty_query_is_an_error(self) -> None:
        with self.assertRaises(DocsError) as caught:
            self.index.search("   ")
        self.assertEqual(caught.exception.code, "EMPTY_QUERY")

    # ----------------------------------------------------------------- impact
    def test_for_target_by_file_symbol_and_module(self) -> None:
        by_file = self.index.for_target("engine/terrain/src/Planner.cpp")
        self.assertEqual(by_file["docs"][0]["path"], "/gfx/terrain")
        self.assertEqual(by_file["docs"][0]["invariants"], ["Levels never overlap."])
        by_symbol = self.index.for_target("Planner")
        self.assertEqual(by_symbol["docs"][0]["path"], "/gfx/terrain")
        by_module = self.index.for_target("engine/terrain/include/Other.hpp")
        self.assertTrue(by_module["docs"])
        self.assertIn("module", by_module["docs"][0]["why"])

    def test_for_target_reports_neighbours(self) -> None:
        result = self.index.for_target("engine/terrain/src/Planner.cpp")
        self.assertIn("/gfx/ui", [n["path"] for n in result["also_affected"]])

    def test_for_target_unknown_has_note(self) -> None:
        result = self.index.for_target("somewhere/else.cpp")
        self.assertEqual(result["docs"], [])
        self.assertTrue(result["note"])

    def test_rules_apply_by_glob_and_task_includes_always_rules(self) -> None:
        self.repo.write(
            "docs/tree/rule.md",
            block("/rule", "A rule", "Always do the thing.", 'kind = "rule"\napplies_to = ["engine/**"]\ninclude_in_tasks = true'),
        )
        self.index.refresh()
        result = self.index.for_target("engine/terrain/src/Planner.cpp")
        self.assertIn("/rule", [r["path"] for r in result["rules"]])
        task = self.index.for_task("rings terrain")
        self.assertIn("/rule", [r["path"] for r in task["always_applicable_rules"]])
        self.assertEqual(task["start_here"][0]["path"], "/gfx/terrain")

    # ---------------------------------------------------------------- checks
    def test_check_flags_broken_links_sources_and_symbols(self) -> None:
        self.repo.write(
            "docs/tree/bad.md",
            block(
                "/gfx/bad",
                "Bad",
                "Broken on purpose.",
                'kind = "concept"\nrelated = ["/nowhere"]\nsources = ["engine/terrain/src/Missing.cpp"]\n'
                '[routes]\n"x" = "child-that-is-not-there"',
            ),
        )
        self.repo.write(
            "docs/tree/badsym.md",
            block("/gfx/badsym", "Bad symbol", "Symbol not in sources.", 'kind = "concept"\nsources = ["engine/terrain/src/Planner.cpp"]\nsymbols = ["NotThere"]'),
        )
        self.repo.write("docs/tree/orphan.md", block("/missing/parent/leaf", "Orphan", "No parent."))
        self.repo.write("docs/tree/dup.md", block("/gfx/terrain", "Duplicate", "Same path twice."))
        self.index.refresh()
        codes = {e["code"] for e in self.index.check()["errors"]}
        for expected in ("BROKEN_LINK", "MISSING_SOURCE", "BROKEN_ROUTE", "MISSING_SYMBOL", "NO_PARENT", "DUPLICATE_PATH"):
            self.assertIn(expected, codes)

    def test_check_flags_missing_summary_and_bad_front_matter(self) -> None:
        self.repo.write("docs/tree/nosummary.md", '+++\npath = "/gfx/nosummary"\ntitle = "No summary"\n+++\n\nbody\n')
        self.repo.write("docs/tree/broken.md", "+++\npath = \n+++\n\nbody\n")
        self.index.refresh()
        codes = {e["code"] for e in self.index.check()["errors"]}
        self.assertIn("NO_SUMMARY", codes)
        self.assertIn("BLOCK_BAD_FRONT_MATTER", codes)

    def test_check_flags_missing_root(self) -> None:
        (self.repo.root / "docs/tree/index.md").unlink()
        self.index.refresh()
        self.assertIn("NO_ROOT", {e["code"] for e in self.index.check()["errors"]})

    # --------------------------------------------------------------- refresh
    def test_edits_are_picked_up_without_restart(self) -> None:
        path = self.repo.write("docs/tree/ui.md", block("/gfx/ui", "Viewport UI v2", "Panels, edited."))
        os.utime(path, (time.time() + 5, time.time() + 5))
        self.assertTrue(self.index.refresh())
        self.assertEqual(self.index.get_block("/gfx/ui").title, "Viewport UI v2")
        self.assertFalse(self.index.refresh())


class CoverageAndScaffoldTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = TempRepo()
        self.addCleanup(self.repo.cleanup)
        r = self.repo
        r.write("docs/tree/index.md", block("/", "Root", "Root.", 'kind = "root"\n[routes]\n"old stuff" = "/legacy/old-spec"'))
        # two modules: one with a card, one without
        r.write("engine/alpha/CMakeLists.txt", "add_library(OrbitAlpha STATIC a.cpp)\nadd_test(NAME Orbit.Alpha COMMAND x)\n")
        r.write("engine/alpha/include/orbit/alpha/Alpha.hpp", "namespace orbit::alpha {\nclass AlphaThing\n{\n};\n}\n")
        r.write(
            "engine/alpha/docs/index.md",
            block("/alpha", "Alpha", "Alpha module.", 'sources = ["engine/alpha/include/orbit/alpha/Alpha.hpp"]\nsymbols = ["AlphaThing"]'),
        )
        r.write("engine/beta/CMakeLists.txt", "add_library(OrbitBeta STATIC b.cpp)\ntarget_link_libraries(OrbitBeta PUBLIC Orbit::Alpha)\n")
        r.write("engine/beta/include/orbit/beta/Beta.hpp", "namespace orbit::beta {\nstruct BetaData\n{\n};\n}\n")
        r.write("docs/OLD_SPEC.md", "# Old spec\n\nIntro.\n\n## Part\n\ntext\n")
        r.write("docs/ORPHAN.md", "# Orphan\n\nNobody links here.\n")
        self.index = DocsIndex(r.root)

    def test_coverage_lists_uncovered_modules_and_unlinked_documents(self) -> None:
        report = self.index.coverage()
        self.assertEqual(report["modules_uncovered"], ["engine/beta"])
        unlinked = [d["path"] for d in report["documents_unlinked"]]
        self.assertEqual(unlinked, ["/legacy/orphan"])

    def test_scaffold_drafts_a_card_with_real_facts_and_cannot_pass_check(self) -> None:
        draft = scaffold(self.index, "engine/beta", "/beta")
        self.assertIn(TODO_MARKER, draft)
        self.assertIn('"BetaData"', draft)
        self.assertIn('depends_on = ["/alpha"]', draft)
        self.repo.write("engine/beta/docs/index.md", draft)
        self.index.refresh()
        codes = {e["code"] for e in self.index.check()["errors"]}
        self.assertIn("UNFINISHED_BLOCK", codes)
        report = self.index.coverage()
        self.assertEqual(report["modules_uncovered"], [])

    def test_scaffold_rejects_unknown_module(self) -> None:
        with self.assertRaises(ValueError):
            scaffold(self.index, "engine/missing")


class DocsApiTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = TempRepo()
        self.addCleanup(self.repo.cleanup)
        self.repo.write("docs/tree/index.md", block("/", "Root", "Root.", 'kind = "root"'))
        self.repo.write(
            "docs/tree/page.md",
            block("/page", "Page", "A long page.", body="## One\n\nfirst\n\n### Sub\n\nnested\n\n## Two\n\n" + "x" * 500),
        )
        self.api = DocsApi(DocsIndex(self.repo.root))

    def test_envelope_shape_and_request_ids_increase(self) -> None:
        first = self.api.get("/page")
        second = self.api.get("/page")
        self.assertTrue(first["ok"])
        for key in ("operation", "request_id", "duration_ms", "result", "warnings", "suggestions"):
            self.assertIn(key, first)
        self.assertNotEqual(first["request_id"], second["request_id"])

    def test_not_found_has_code_and_suggestions(self) -> None:
        result = self.api.get("/pag")
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "DOC_NOT_FOUND")
        self.assertTrue(any("/page" in s for s in result["suggestions"]))

    def test_section_extraction_includes_nested_headings_only(self) -> None:
        one = self.api.get("/page", section="One")["result"]["body"]
        self.assertIn("nested", one)
        self.assertNotIn("xxxx", one)
        bad = self.api.get("/page", section="Nope")
        self.assertEqual(bad["error"]["code"], "SECTION_NOT_FOUND")
        self.assertIn("one", bad["error"]["available"])

    def test_body_paging(self) -> None:
        first = self.api.get("/page", section="Two", max_chars=100)
        self.assertTrue(first["result"]["truncated"])
        rest = self.api.get("/page", section="Two", offset=100, max_chars=10000)
        self.assertNotIn("truncated", rest["result"])
        total = first["result"]["body_chars"]["total"]
        self.assertEqual(100 + rest["result"]["body_chars"]["returned"], total)

    def test_root_and_children(self) -> None:
        self.assertTrue(self.api.root()["ok"])
        kids = self.api.children("/")["result"]["children"]
        self.assertEqual([k["path"] for k in kids], ["/page"])

    def test_helpers(self) -> None:
        text = "# T\n\n## A b\n\n```\n## no\n```\n\n## C\n"
        self.assertEqual([h["slug"] for h in headings(text)], ["a-b", "c"])
        self.assertIsNone(extract_section(text, "no"))


class RealRepositoryTests(unittest.TestCase):
    """The checked-in documentation tree must stay valid (CI runs this too)."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.index = DocsIndex(REPO_ROOT)

    def test_tree_has_no_errors(self) -> None:
        report = self.index.check()
        self.assertEqual(report["errors"], [], "\n".join(f"{e['path']}: {e['message']}" for e in report["errors"]))

    def test_every_structured_node_is_reachable_from_root(self) -> None:
        seen: set[str] = set()
        stack = ["/"]
        while stack:
            path = stack.pop()
            seen.add(path)
            stack.extend(c.path for c in self.index.children(path))
        structured = {p for p, b in self.index.blocks.items() if not b.legacy}
        self.assertEqual(structured - seen, set())

    def test_every_module_has_a_card_and_every_document_is_linked(self) -> None:
        report = self.index.coverage()
        self.assertEqual(report["modules_uncovered"], [])
        self.assertEqual([d["path"] for d in report["documents_unlinked"]], [])

    def test_module_cards_live_in_their_module_and_have_sources(self) -> None:
        cards = [b for b in self.index.blocks.values() if not b.legacy and b.module_dir]
        self.assertGreater(len(cards), 60)
        for card in cards:
            self.assertTrue(card.sources, card.path)

    def test_root_routes_point_somewhere(self) -> None:
        node = self.index.node(self.index.get_block("/"))
        self.assertGreater(len(node["where_to_go"]), 3)
        for route in node["where_to_go"]:
            self.assertIsNotNone(route["title"], route)


if __name__ == "__main__":
    unittest.main()
