#!/usr/bin/env python3
"""Pure-Python tests for the Isaac smoke articulation helpers."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
EXP = ROOT / "experiments" / "locomotion_19body"
HELPER_PATH = EXP / "training" / "asterra_rl" / "articulation.py"
BUILDER_PATH = EXP / "training" / "scripts" / "build_articulation.py"


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ArticulationHelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.helper = load_module(HELPER_PATH, "asterra_articulation_helper")
        cls.builder = load_module(BUILDER_PATH, "asterra_build_articulation")
        contract = cls.builder.load_json(EXP / "config" / "physics_contract_19body.json")
        policy = cls.builder.load_json(EXP / "config" / "modular_policy.json")
        cls.manifest = cls.builder.build_manifest(contract, policy)

    def test_manifest_counts_and_dof_order(self) -> None:
        self.helper.validate_manifest(self.manifest)
        names = self.helper.expected_dof_names(self.manifest)
        self.assertEqual(len(names), 54)
        self.assertEqual(names[:3], ["pelvis_spine_x", "pelvis_spine_y", "pelvis_spine_z"])
        self.assertEqual(names[-3:], ["right_ankle_x", "right_ankle_y", "right_ankle_z"])

    def test_expected_physx_link_set(self) -> None:
        names = self.helper.expected_body_names(self.manifest)
        self.assertEqual(len(names), 55)
        self.assertEqual(len(set(names)), 55)
        self.assertIn("pelvis", names)
        self.assertIn("__frame__right_ankle_y", names)

    def test_neutral_root_height(self) -> None:
        height = self.helper.standing_root_height_m(self.manifest, clearance_m=0.005)
        self.assertAlmostEqual(height, 0.945, places=6)

    def test_load_manifest_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "manifest.json"
            path.write_text(json.dumps(self.manifest), encoding="utf-8")
            loaded = self.helper.load_articulation_manifest(path)
            self.assertEqual(loaded["source_contract_sha256"], self.manifest["source_contract_sha256"])

    def test_bad_dof_order_is_rejected(self) -> None:
        broken = json.loads(json.dumps(self.manifest))
        broken["dofs"][1]["action_index"] = 9
        with self.assertRaises(ValueError):
            self.helper.validate_manifest(broken)


if __name__ == "__main__":
    unittest.main(verbosity=2)
