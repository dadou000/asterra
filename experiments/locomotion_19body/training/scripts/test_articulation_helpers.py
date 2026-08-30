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
        self.assertIn("frame__right_ankle_y", names)

    def test_neutral_root_height(self) -> None:
        height = self.helper.standing_root_height_m(self.manifest, clearance_m=0.005)
        self.assertAlmostEqual(height, 0.945, places=6)

    def test_solver_velocity_is_not_humanoid_velocity_capability(self) -> None:
        solver_limits = self.helper.physx_solver_velocity_limits(self.manifest)
        voluntary_limits = {
            str(dof["name"]): float(dof["velocity_limit_rad_s"])
            for dof in self.manifest["dofs"]
        }
        self.assertEqual(set(solver_limits), set(voluntary_limits))
        self.assertTrue(
            all(
                solver_limits[name] == self.helper.PHYSX_SOLVER_VELOCITY_LIMIT_RAD_S
                for name in solver_limits
            )
        )
        self.assertGreater(
            self.helper.PHYSX_SOLVER_VELOCITY_LIMIT_RAD_S,
            max(voluntary_limits.values()),
        )
        self.assertAlmostEqual(self.helper.TRAINING_JOINT_ARMATURE_KGM2, 0.005)

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

    def test_physx_reordering_is_mapped_not_rejected(self) -> None:
        canonical = self.helper.expected_dof_names(self.manifest)

        class FakeRobot:
            joint_names = list(reversed(canonical))

        mapping = self.helper.canonical_to_physx_joint_ids(FakeRobot(), self.manifest)
        self.assertEqual(len(mapping), 54)
        self.assertEqual(mapping[0], 53)
        self.assertEqual(mapping[-1], 0)
        self.assertEqual(
            [FakeRobot.joint_names[index] for index in mapping],
            canonical,
        )

    def test_physx_missing_joint_is_rejected(self) -> None:
        canonical = self.helper.expected_dof_names(self.manifest)

        class FakeRobot:
            joint_names = canonical[:-1]

        with self.assertRaises(RuntimeError):
            self.helper.canonical_to_physx_joint_ids(FakeRobot(), self.manifest)


if __name__ == "__main__":
    unittest.main(verbosity=2)
