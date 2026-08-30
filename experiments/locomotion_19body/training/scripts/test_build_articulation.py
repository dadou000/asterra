#!/usr/bin/env python3
"""Pure-stdlib tests for the Asterra articulation builder."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("asterra_build_articulation", HERE / "build_articulation.py")
assert SPEC is not None and SPEC.loader is not None
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


class ArticulationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        exp = repo_root() / "experiments" / "locomotion_19body"
        cls.contract = builder.load_json(exp / "config" / "physics_contract_19body.json")
        cls.policy = builder.load_json(exp / "config" / "modular_policy.json")

    def test_contract_validates(self):
        builder.validate_contract(self.contract, self.policy)

    def test_manifest_counts_and_mass(self):
        manifest = builder.build_manifest(self.contract, self.policy)
        self.assertEqual(manifest["physical_body_count"], 19)
        self.assertEqual(manifest["anatomical_joint_count"], 18)
        self.assertEqual(manifest["training_virtual_link_count"], 36)
        self.assertEqual(manifest["training_link_count"], 55)
        self.assertEqual(manifest["actuated_dof_count"], 54)
        self.assertAlmostEqual(manifest["total_physical_mass_kg"], 72.0, places=9)
        self.assertAlmostEqual(manifest["training_virtual_mass_kg"], 0.0036, places=9)
        self.assertAlmostEqual(manifest["training_total_mass_kg"], 72.0036, places=9)

    def test_coordinate_conversion_is_right_handed_z_up(self):
        c = builder.training_rotation(self.contract)
        up = tuple(round(v, 9) for v in builder.mat_vec(c, (0.0, 1.0, 0.0)))
        forward = tuple(round(v, 9) for v in builder.mat_vec(c, (0.0, 0.0, -1.0)))
        self.assertEqual(up, (0.0, 0.0, 1.0))
        self.assertEqual(forward, (0.0, 1.0, 0.0))
        self.assertAlmostEqual(builder.determinant3(c), 1.0, places=9)
        manifest = builder.build_manifest(self.contract, self.policy)
        pelvis = next(b for b in manifest["physical_bodies"] if b["name"] == "pelvis")
        head = next(b for b in manifest["physical_bodies"] if b["name"] == "head")
        self.assertEqual(tuple(round(v, 9) for v in pelvis["neutral_center_training_root_relative_m"]), (0.0, 0.0, 0.0))
        self.assertEqual(tuple(round(v, 9) for v in head["neutral_center_training_root_relative_m"]), (0.0, 0.0, 0.88))

    def test_action_order_is_stable_xyz(self):
        manifest = builder.build_manifest(self.contract, self.policy)
        names = [dof["name"] for dof in manifest["dofs"]]
        self.assertEqual(names[:6], ["pelvis_spine_x", "pelvis_spine_y", "pelvis_spine_z", "spine_chest_x", "spine_chest_y", "spine_chest_z"])
        self.assertEqual(names[-3:], ["right_ankle_x", "right_ankle_y", "right_ankle_z"])
        self.assertEqual([dof["action_index"] for dof in manifest["dofs"]], list(range(54)))

    def test_each_joint_has_one_policy_region(self):
        regions = {}
        for region, joints in self.policy["regions"].items():
            for joint in joints:
                self.assertNotIn(joint, regions)
                regions[joint] = region
        self.assertEqual(set(regions), {joint["name"] for joint in self.contract["joints"]})

    def test_urdf_has_19_physical_plus_36_virtual_links(self):
        root = builder.build_urdf(self.contract).getroot()
        links, joints = root.findall("link"), root.findall("joint")
        self.assertEqual(len(links), 55)
        self.assertEqual(len(joints), 54)
        self.assertTrue(all(joint.get("type") == "revolute" for joint in joints))
        physical_names = {body["name"] for body in self.contract["bodies"]}
        self.assertEqual(len([link for link in links if link.get("name") in physical_names]), 19)
        virtual = [link for link in links if link.get("name", "").startswith("__frame__")]
        self.assertEqual(len(virtual), 36)
        self.assertTrue(all(link.find("collision") is None for link in virtual))

    def test_generated_files_roundtrip(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            manifest_path, urdf_path = directory / "manifest.json", directory / "robot.urdf"
            builder.write_json(manifest_path, builder.build_manifest(self.contract, self.policy))
            builder.write_urdf(urdf_path, builder.build_urdf(self.contract))
            self.assertEqual(json.loads(manifest_path.read_text())["actuated_dof_count"], 54)
            self.assertEqual(ET.parse(urdf_path).getroot().get("name"), "asterra_19body_training")


if __name__ == "__main__":
    unittest.main(verbosity=2)
