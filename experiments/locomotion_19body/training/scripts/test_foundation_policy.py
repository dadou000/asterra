#!/usr/bin/env python3
"""Fast tensor-contract tests for foundation observations, actions and stand rewards."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest

import torch

ROOT = Path(__file__).resolve().parents[4]
EXP = ROOT / "experiments" / "locomotion_19body"
TRAINING = EXP / "training"
if str(TRAINING) not in sys.path:
    sys.path.insert(0, str(TRAINING))

from asterra_rl.actions import CanonicalActionProcessor
from asterra_rl.observations import OBSERVATION_SIZE, build_foundation_observation
from asterra_rl.rewards import compute_stand_reward


def load_builder():
    path = TRAINING / "scripts" / "build_articulation.py"
    spec = importlib.util.spec_from_file_location("asterra_build_articulation_foundation_test", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FoundationPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        builder = load_builder()
        contract = builder.load_json(EXP / "config" / "physics_contract_19body.json")
        policy = builder.load_json(EXP / "config" / "modular_policy.json")
        cls.manifest = builder.build_manifest(contract, policy)
        cls.processor = CanonicalActionProcessor(cls.manifest, device="cpu")

    def test_zero_action_is_neutral(self) -> None:
        actions = torch.zeros((3, 54))
        target = self.processor.normalized_to_targets(actions)
        self.assertEqual(tuple(target.shape), (3, 54))
        self.assertTrue(torch.allclose(target, torch.zeros_like(target)))

    def test_targets_respect_static_hard_limits(self) -> None:
        actions = torch.linspace(-1.0, 1.0, 54).repeat(2, 1)
        target = self.processor.normalized_to_targets(actions)
        self.assertTrue(torch.all(target >= self.processor.lower - 1.0e-6))
        self.assertTrue(torch.all(target <= self.processor.upper + 1.0e-6))

    def test_shoulder_elevation_tightens_twist(self) -> None:
        idx = self.processor.joint_indices["left_shoulder"]
        actions = torch.zeros((1, 54))
        actions[0, idx.x] = 1.0
        actions[0, idx.y] = 1.0
        target = self.processor.normalized_to_targets(actions)
        self.assertLess(abs(torch.rad2deg(target[0, idx.y]).item()), 70.0)

    def test_foundation_observation_is_197_values(self) -> None:
        n = 2
        root_pos = torch.tensor([[0.0, 0.0, 0.945], [0.0, 0.0, 0.945]])
        root_quat = torch.tensor([[1.0, 0.0, 0.0, 0.0]]).repeat(n, 1)
        zeros3 = torch.zeros((n, 3))
        body_pos = torch.zeros((n, 55, 3))
        key_ids = torch.tensor([0, 1, 2, 3, 4], dtype=torch.long)
        q = torch.zeros((n, 54))
        obs = build_foundation_observation(
            root_pos_w=root_pos,
            root_quat_w=root_quat,
            root_lin_vel_w=zeros3,
            root_ang_vel_w=zeros3,
            body_pos_w=body_pos,
            env_origins_w=torch.zeros((n, 3)),
            key_body_ids=key_ids,
            joint_pos_canonical=q,
            joint_vel_canonical=q,
            joint_lower_canonical=self.processor.lower,
            joint_upper_canonical=self.processor.upper,
            joint_velocity_limits_canonical=self.processor.velocity_limits,
            previous_action=q,
            foot_contact_load=torch.ones((n, 2)),
            command=torch.zeros((n, 3)),
            phase=torch.tensor([[0.0, 1.0]]).repeat(n, 1),
            gravity_w=torch.tensor((0.0, 0.0, -9.81)),
            reference_forward_w=torch.tensor((0.0, 1.0, 0.0)),
        )
        self.assertEqual(tuple(obs.shape), (n, OBSERVATION_SIZE))
        self.assertAlmostEqual(obs[0, 0].item(), 0.945, places=5)
        self.assertTrue(torch.isfinite(obs).all())

    def test_ideal_stand_scores_above_fallen_state(self) -> None:
        zeros3 = torch.zeros((1, 3))
        zeros54 = torch.zeros((1, 54))
        common = dict(
            target_root_height=0.945,
            root_lin_vel_gravity=zeros3,
            root_ang_vel_gravity=zeros3,
            foot_contact_load=torch.ones((1, 2)),
            normalized_joint_pos=zeros54,
            normalized_joint_vel=zeros54,
            normalized_torque=zeros54,
            actions=zeros54,
            previous_actions=zeros54,
            step_dt=1.0 / 60.0,
        )
        ideal, _ = compute_stand_reward(
            root_height=torch.tensor([0.945]),
            upright_cos=torch.tensor([1.0]),
            com_support_distance=torch.tensor([0.0]),
            **common,
        )
        fallen, _ = compute_stand_reward(
            root_height=torch.tensor([0.50]),
            upright_cos=torch.tensor([0.1]),
            com_support_distance=torch.tensor([0.40]),
            **common,
        )
        self.assertGreater(ideal.item(), fallen.item())


if __name__ == "__main__":
    unittest.main(verbosity=2)
