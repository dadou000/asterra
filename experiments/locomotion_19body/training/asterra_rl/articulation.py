"""Asterra Stage-0A articulation contract helpers.

This module intentionally keeps JSON/path/contract functions importable without
Isaac Sim. Isaac Lab imports happen only inside :func:`make_articulation_cfg`, so
pure-Python tests can validate the generated manifest on any machine.
"""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any

EXPECTED_PHYSICAL_BODIES = 19
EXPECTED_ANATOMICAL_JOINTS = 18
EXPECTED_VIRTUAL_LINKS = 36
EXPECTED_LINKS = 55
EXPECTED_DOFS = 54
FOOT_NAMES = ("left_foot", "right_foot")


def repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


def experiment_root() -> Path:
    return repo_root() / "experiments" / "locomotion_19body"


def load_articulation_manifest(path: Path | str | None = None) -> dict[str, Any]:
    manifest_path = Path(path) if path is not None else experiment_root() / "generated" / "articulation_manifest.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(
            f"Missing articulation manifest: {manifest_path}\n"
            "Run training/scripts/build_articulation.py first."
        )
    with manifest_path.open("r", encoding="utf-8") as handle:
        manifest: dict[str, Any] = json.load(handle)
    validate_manifest(manifest)
    return manifest


def validate_manifest(manifest: dict[str, Any]) -> None:
    counts = (
        int(manifest.get("physical_body_count", -1)),
        int(manifest.get("anatomical_joint_count", -1)),
        int(manifest.get("training_virtual_link_count", -1)),
        int(manifest.get("training_link_count", -1)),
        int(manifest.get("actuated_dof_count", -1)),
    )
    expected = (
        EXPECTED_PHYSICAL_BODIES,
        EXPECTED_ANATOMICAL_JOINTS,
        EXPECTED_VIRTUAL_LINKS,
        EXPECTED_LINKS,
        EXPECTED_DOFS,
    )
    if counts != expected:
        raise ValueError(f"Unexpected articulation counts {counts}; expected {expected}")

    dofs = manifest.get("dofs", [])
    if len(dofs) != EXPECTED_DOFS:
        raise ValueError(f"Manifest has {len(dofs)} DOFs; expected {EXPECTED_DOFS}")
    indices = [int(dof["action_index"]) for dof in dofs]
    if indices != list(range(EXPECTED_DOFS)):
        raise ValueError("Action indices must be contiguous and define the canonical DOF order")
    names = [str(dof["name"]) for dof in dofs]
    if len(set(names)) != EXPECTED_DOFS:
        raise ValueError("DOF names must be unique")

    physical_names = [str(body["name"]) for body in manifest.get("physical_bodies", [])]
    if len(physical_names) != EXPECTED_PHYSICAL_BODIES or len(set(physical_names)) != EXPECTED_PHYSICAL_BODIES:
        raise ValueError("Manifest physical body list must contain 19 unique names")
    for foot in FOOT_NAMES:
        if foot not in physical_names:
            raise ValueError(f"Missing required contact body {foot}")

    coordinate = manifest.get("training_coordinate_system", {})
    if coordinate.get("up") != "+Z" or coordinate.get("origin") != "root_body_center":
        raise ValueError("Training manifest must be pelvis/root-centered and +Z-up")

    for dof in dofs:
        low = float(dof["lower_rad"])
        high = float(dof["upper_rad"])
        if not math.isfinite(low) or not math.isfinite(high) or low > high:
            raise ValueError(f"Invalid limit for {dof['name']}: {low}..{high}")
        for field in (
            "effort_limit_nm",
            "velocity_limit_rad_s",
            "stiffness_nm_per_rad",
            "damping_nms_per_rad",
        ):
            value = float(dof[field])
            if not math.isfinite(value) or value < 0.0:
                raise ValueError(f"Invalid {field} for {dof['name']}: {value}")


def expected_dof_names(manifest: dict[str, Any]) -> list[str]:
    validate_manifest(manifest)
    return [str(dof["name"]) for dof in manifest["dofs"]]


def canonical_to_physx_joint_ids(robot: Any, manifest: dict[str, Any]) -> list[int]:
    """Map canonical manifest action order to the articulation's internal DOF order."""
    expected = expected_dof_names(manifest)
    actual = list(robot.joint_names)
    if len(actual) != len(expected) or set(actual) != set(expected):
        raise RuntimeError(
            "PhysX joint-name drift: "
            f"missing={sorted(set(expected) - set(actual))} "
            f"extra={sorted(set(actual) - set(expected))}"
        )
    index_by_name = {name: index for index, name in enumerate(actual)}
    return [index_by_name[name] for name in expected]


def expected_body_names(manifest: dict[str, Any]) -> list[str]:
    """Return all expected PhysX links as a set-stable list.

    Physical link names are canonical. Virtual coordinate-frame links are derived
    from the anatomical joint names in exactly the same way as build_articulation.py.
    PhysX may reorder links internally, so live validation compares body names as a
    set while preserving the physical/virtual distinction.
    """
    validate_manifest(manifest)
    names = [str(body["name"]) for body in manifest["physical_bodies"]]
    for joint in manifest["anatomical_joints"]:
        joint_name = str(joint["name"])
        names.append(f"frame__{joint_name}_x")
        names.append(f"frame__{joint_name}_y")
    return names


def resolve_training_usd(path: Path | str | None = None) -> Path:
    if path is not None:
        candidate = Path(path).expanduser().resolve()
        if candidate.is_file():
            return candidate
        raise FileNotFoundError(f"Training USD does not exist: {candidate}")

    usd_dir = experiment_root() / "assets" / "usd"
    preferred = usd_dir / "asterra_19body_training.usd"
    if preferred.is_file():
        return preferred

    matches = sorted(usd_dir.glob("asterra_19body_training*.usd")) if usd_dir.is_dir() else []
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        raise RuntimeError(f"Ambiguous Asterra training USDs: {matches}")
    raise FileNotFoundError(
        f"Missing generated training USD under {usd_dir}.\n"
        "Run build_articulation.py, then convert_training_asset.py --headless --force."
    )


def _training_rotation_rows(manifest: dict[str, Any]) -> tuple[tuple[float, float, float], ...]:
    rows = manifest["training_coordinate_system"]["asterra_to_training_rotation_rows"]
    return tuple(tuple(float(v) for v in row) for row in rows)


def _mat_vec(
    matrix: tuple[tuple[float, float, float], ...],
    vector: tuple[float, float, float],
) -> tuple[float, float, float]:
    return tuple(sum(matrix[i][j] * vector[j] for j in range(3)) for i in range(3))


def _vertical_half_extent_m(body: dict[str, Any], manifest: dict[str, Any]) -> float:
    """Conservative neutral vertical half-extent in the +Z-up training frame."""
    geom = body["geometry"]
    kind = str(geom["type"])
    if kind == "sphere":
        return float(geom["radius_m"])

    rotation = _training_rotation_rows(manifest)
    if kind == "box":
        size = tuple(float(v) for v in geom["size_m"])
        return sum(abs(rotation[2][axis]) * size[axis] * 0.5 for axis in range(3))

    if kind == "capsule":
        source_axis_name = str(geom.get("axis", "y")).lower()
        axis_index = {"x": 0, "y": 1, "z": 2}[source_axis_name]
        source_axis = tuple(1.0 if i == axis_index else 0.0 for i in range(3))
        axis_training = _mat_vec(rotation, source_axis)
        vertical_axis = abs(axis_training[2])
        radius = float(geom["radius_m"])
        half_height = float(geom["height_m"]) * 0.5
        radial_vertical = radius * math.sqrt(max(0.0, 1.0 - vertical_axis * vertical_axis))
        return half_height * vertical_axis + radial_vertical

    raise ValueError(f"Unsupported geometry for support-height calculation: {kind}")


def standing_root_height_m(manifest: dict[str, Any], clearance_m: float = 0.005) -> float:
    """Compute root Z that places the neutral soles just above z=0."""
    validate_manifest(manifest)
    if clearance_m < 0.0:
        raise ValueError("clearance_m must be non-negative")
    body_by_name = {str(body["name"]): body for body in manifest["physical_bodies"]}
    lowest_root_relative_z = math.inf
    for foot_name in FOOT_NAMES:
        body = body_by_name[foot_name]
        center = tuple(float(v) for v in body["neutral_center_training_root_relative_m"])
        bottom = center[2] - _vertical_half_extent_m(body, manifest)
        lowest_root_relative_z = min(lowest_root_relative_z, bottom)
    if not math.isfinite(lowest_root_relative_z):
        raise ValueError("Unable to derive neutral support height")
    return -lowest_root_relative_z + clearance_m


def _dofs_by_region(manifest: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    groups: dict[str, list[dict[str, Any]]] = {}
    for dof in manifest["dofs"]:
        groups.setdefault(str(dof["region"]), []).append(dof)
    return groups


def make_articulation_cfg(
    usd_path: Path | str,
    manifest: dict[str, Any],
    *,
    prim_path: str = "{ENV_REGEX_NS}/Robot",
    passive: bool = False,
    root_clearance_m: float = 0.005,
):
    """Build an Isaac Lab 2.3.1 ``ArticulationCfg`` from the manifest.

    The manifest is the source of joint order/gains/limits. The USD contains hard
    geometric/joint limits; the actuator config reasserts torque, velocity and PD
    capability. ``passive=True`` zeroes joint drives without changing hard limits.
    """
    validate_manifest(manifest)
    usd = Path(usd_path).expanduser().resolve()
    if not usd.is_file():
        raise FileNotFoundError(f"Training USD does not exist: {usd}")

    import isaaclab.sim as sim_utils
    from isaaclab.actuators import ImplicitActuatorCfg
    from isaaclab.assets import ArticulationCfg

    actuators: dict[str, ImplicitActuatorCfg] = {}
    for region, dofs in _dofs_by_region(manifest).items():
        names = [str(dof["name"]) for dof in dofs]
        effort = {str(dof["name"]): float(dof["effort_limit_nm"]) for dof in dofs}
        velocity = {str(dof["name"]): float(dof["velocity_limit_rad_s"]) for dof in dofs}
        if passive:
            stiffness: dict[str, float] | float = 0.0
            damping: dict[str, float] | float = 0.0
        else:
            stiffness = {str(dof["name"]): float(dof["stiffness_nm_per_rad"]) for dof in dofs}
            damping = {str(dof["name"]): float(dof["damping_nms_per_rad"]) for dof in dofs}
        actuators[region] = ImplicitActuatorCfg(
            joint_names_expr=names,
            effort_limit_sim=effort,
            velocity_limit_sim=velocity,
            stiffness=stiffness,
            damping=damping,
        )

    return ArticulationCfg(
        prim_path=prim_path,
        spawn=sim_utils.UsdFileCfg(
            usd_path=str(usd),
            activate_contact_sensors=True,
            rigid_props=sim_utils.RigidBodyPropertiesCfg(
                disable_gravity=False,
                linear_damping=0.04,
                angular_damping=0.08,
                max_linear_velocity=100.0,
                max_angular_velocity=30.0,
                max_depenetration_velocity=5.0,
                enable_gyroscopic_forces=True,
            ),
            articulation_props=sim_utils.ArticulationRootPropertiesCfg(
                enabled_self_collisions=False,
                solver_position_iteration_count=8,
                solver_velocity_iteration_count=4,
                sleep_threshold=0.0,
                stabilization_threshold=0.001,
            ),
        ),
        init_state=ArticulationCfg.InitialStateCfg(
            pos=(0.0, 0.0, standing_root_height_m(manifest, root_clearance_m)),
            joint_pos={".*": 0.0},
            joint_vel={".*": 0.0},
        ),
        soft_joint_pos_limit_factor=1.0,
        actuators=actuators,
    )


def validate_live_articulation(
    robot: Any,
    manifest: dict[str, Any],
    *,
    atol: float = 2.0e-4,
) -> dict[str, Any]:
    """Validate an initialized Isaac articulation against the generated contract.

    ``sim.reset()`` must have completed so joint limits/gains/masses are populated.
    The canonical policy order is never inferred from PhysX traversal order; a
    name-based manifest->PhysX index map is returned for later observation/action code.
    """
    validate_manifest(manifest)
    import torch

    expected_joints = expected_dof_names(manifest)
    actual_joints = list(robot.joint_names)
    canonical_to_physx = canonical_to_physx_joint_ids(robot, manifest)

    expected_bodies = set(expected_body_names(manifest))
    actual_bodies = set(robot.body_names)
    if actual_bodies != expected_bodies:
        raise RuntimeError(
            "PhysX body-name drift: "
            f"missing={sorted(expected_bodies - actual_bodies)} "
            f"extra={sorted(actual_bodies - expected_bodies)}"
        )

    device = robot.data.joint_pos.device
    dtype = robot.data.joint_pos.dtype
    dof_by_name = {str(dof["name"]): dof for dof in manifest["dofs"]}
    dofs_physx_order = [dof_by_name[name] for name in actual_joints]

    expected_limits = torch.tensor(
        [[float(dof["lower_rad"]), float(dof["upper_rad"])] for dof in dofs_physx_order],
        device=device,
        dtype=dtype,
    )
    expected_stiffness = torch.tensor(
        [float(dof["stiffness_nm_per_rad"]) for dof in dofs_physx_order],
        device=device,
        dtype=dtype,
    )
    expected_damping = torch.tensor(
        [float(dof["damping_nms_per_rad"]) for dof in dofs_physx_order],
        device=device,
        dtype=dtype,
    )
    expected_effort = torch.tensor(
        [float(dof["effort_limit_nm"]) for dof in dofs_physx_order],
        device=device,
        dtype=dtype,
    )
    expected_velocity = torch.tensor(
        [float(dof["velocity_limit_rad_s"]) for dof in dofs_physx_order],
        device=device,
        dtype=dtype,
    )

    checks = (
        ("joint position limits", robot.data.joint_pos_limits[0], expected_limits),
        ("joint effort limits", robot.data.joint_effort_limits[0], expected_effort),
        ("joint velocity limits", robot.data.joint_vel_limits[0], expected_velocity),
    )
    for label, actual, expected in checks:
        if not torch.allclose(actual, expected, atol=atol, rtol=0.0):
            delta = torch.max(torch.abs(actual - expected)).item()
            raise RuntimeError(f"{label} mismatch; max abs delta={delta:.6g}")

    live_stiffness = robot.data.joint_stiffness[0]
    live_damping = robot.data.joint_damping[0]
    passive = bool(
        torch.max(torch.abs(live_stiffness)).item() <= atol
        and torch.max(torch.abs(live_damping)).item() <= atol
    )
    if not passive:
        if not torch.allclose(live_stiffness, expected_stiffness, atol=atol, rtol=0.0):
            delta = torch.max(torch.abs(live_stiffness - expected_stiffness)).item()
            raise RuntimeError(f"joint stiffness mismatch; max abs delta={delta:.6g}")
        if not torch.allclose(live_damping, expected_damping, atol=atol, rtol=0.0):
            delta = torch.max(torch.abs(live_damping - expected_damping)).item()
            raise RuntimeError(f"joint damping mismatch; max abs delta={delta:.6g}")

    total_mass = float(robot.data.default_mass[0].sum().item())
    expected_mass = float(manifest["training_total_mass_kg"])
    if abs(total_mass - expected_mass) > 5.0e-3:
        raise RuntimeError(
            f"training total mass mismatch: PhysX={total_mass:.6f} manifest={expected_mass:.6f}"
        )

    return {
        "joint_count": len(actual_joints),
        "physx_order_matches_canonical": actual_joints == expected_joints,
        "canonical_to_physx_joint_ids": canonical_to_physx,
        "body_count": len(actual_bodies),
        "physical_body_count": int(manifest["physical_body_count"]),
        "virtual_body_count": int(manifest["training_virtual_link_count"]),
        "training_total_mass_kg": total_mass,
        "passive_actuators": passive,
        "device": str(device),
        "dtype": str(dtype),
    }
