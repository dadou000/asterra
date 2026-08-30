#!/usr/bin/env python3
"""Fast command-contract tests for the Godot training bridge."""

from __future__ import annotations

import argparse
from pathlib import Path
import unittest

from game_training_bridge import Bridge


def bridge_args(*, headless: bool) -> argparse.Namespace:
    return argparse.Namespace(
        task="train_stand",
        status=Path("status.json"),
        log=Path("bridge.log"),
        cancel=Path("cancel.request"),
        mode="hold",
        num_envs=256,
        seconds=2.0,
        max_iterations=100,
        seed=1467,
        joint_noise_deg=0.0,
        device="cuda:0",
        headless=headless,
        strict_contact=True,
        force=False,
    )


class GameTrainingBridgeTests(unittest.TestCase):
    def test_stand_training_is_always_headless_from_godot(self) -> None:
        for selected in (False, True):
            with self.subTest(headless_selected=selected):
                command = Bridge(bridge_args(headless=selected)).stand_training_command()
                self.assertEqual(command.count("--headless"), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
