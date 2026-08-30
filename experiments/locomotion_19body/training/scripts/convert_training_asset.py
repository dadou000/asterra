#!/usr/bin/env python3
"""Convert the generated Asterra training URDF to USD with Isaac Lab 2.3.1."""

from __future__ import annotations

import argparse
from pathlib import Path

from isaaclab.app import AppLauncher


def repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


parser = argparse.ArgumentParser(description="Convert Asterra 19-body training URDF to USD.")
parser.add_argument("--force", action="store_true", help="Force USD regeneration.")
AppLauncher.add_app_launcher_args(parser)
args = parser.parse_args()

app_launcher = AppLauncher(args)
simulation_app = app_launcher.app

from isaaclab.sim.converters import UrdfConverter, UrdfConverterCfg  # noqa: E402


def main() -> None:
    exp = repo_root() / "experiments" / "locomotion_19body"
    urdf_path = exp / "assets" / "asterra_19body_training.urdf"
    usd_dir = exp / "assets" / "usd"
    if not urdf_path.is_file():
        raise FileNotFoundError(f"Missing generated URDF: {urdf_path}\nRun build_articulation.py first.")
    usd_dir.mkdir(parents=True, exist_ok=True)
    cfg = UrdfConverterCfg(
        asset_path=str(urdf_path),
        usd_dir=str(usd_dir),
        fix_base=False,
        merge_fixed_joints=False,
        self_collision=False,
        force_usd_conversion=args.force,
        # Asterra's ArticulationCfg owns the per-DOF PD gains/limits.
        joint_drive=None,
    )
    converter = UrdfConverter(cfg)
    print(f"Asterra USD generated: {converter.usd_path}")


if __name__ == "__main__":
    try:
        main()
    finally:
        simulation_app.close()
