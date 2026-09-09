#!/usr/bin/env python3
"""Focused tests for explicit canonical skeleton landmark selection."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "derive_character_physics_proxy", HERE / "derive_character_physics_proxy.py"
)
assert SPEC is not None and SPEC.loader is not None
resolver = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(resolver)


class ProxyResolverTests(unittest.TestCase):
    def setUp(self):
        self.parents = [-1, 0, 1]

    def test_descendant_accepts_direct_and_nested_children(self):
        self.assertTrue(resolver.is_descendant(1, 0, self.parents))
        self.assertTrue(resolver.is_descendant(2, 0, self.parents))

    def test_descendant_rejects_parent_as_child(self):
        self.assertFalse(resolver.is_descendant(0, 1, self.parents))

    def test_side_suffixes_are_resolved(self):
        self.assertEqual(resolver.named_side("upperarm01.L"), -1)
        self.assertEqual(resolver.named_side("upperarm01.R"), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
