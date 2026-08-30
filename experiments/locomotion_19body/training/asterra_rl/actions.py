"""Canonical action processing for the Asterra humanoid foundation policy.

Neural actions are intent only. This module maps normalized canonical actions to
physical joint targets, then applies the anatomical coupled-ROM envelope before
anything is sent to PhysX/Jolt. It never changes torque/velocity capability.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import torch

DOF_COUNT = 54


def _smoothstep(edge0: float, edge1: float, x: torch.Tensor) -> torch.Tensor:
    if edge1 <= edge0:
        raise ValueError("smoothstep edge1 must be greater than edge0")
    t = ((x - edge0) / (edge1 - edge0)).clamp(0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _lerp_pair(values: list[float], t: torch.Tensor) -> torch.Tensor:
    return float(values[0]) + (float(values[1]) - float(values[0])) * t


@dataclass(frozen=True)
class JointIndices:
    x: int
    y: int
    z: int


class CanonicalActionProcessor:
    """Map ``[-1, 1]`` canonical policy actions to coupled anatomical targets."""

    def __init__(
        self,
        manifest: dict[str, Any],
        *,
        device: torch.device | str,
        dtype: torch.dtype = torch.float32,
        target_fraction: float = 0.85,
    ) -> None:
        if not (0.0 < target_fraction <= 1.0):
            raise ValueError("target_fraction must be in (0, 1]")
        dofs = list(manifest["dofs"])
        if len(dofs) != DOF_COUNT:
            raise ValueError(f"Expected {DOF_COUNT} DOFs, got {len(dofs)}")
        self.device = torch.device(device)
        self.dtype = dtype
        self.target_fraction = float(target_fraction)
        self.lower = torch.tensor([float(d["lower_rad"]) for d in dofs], device=self.device, dtype=dtype)
        self.upper = torch.tensor([float(d["upper_rad"]) for d in dofs], device=self.device, dtype=dtype)
        self.velocity_limits = torch.tensor(
            [float(d["velocity_limit_rad_s"]) for d in dofs], device=self.device, dtype=dtype
        )
        self.effort_limits = torch.tensor(
            [float(d["effort_limit_nm"]) for d in dofs], device=self.device, dtype=dtype
        )
        self.names = [str(d["name"]) for d in dofs]
        self.name_to_index = {name: index for index, name in enumerate(self.names)}
        self.joint_indices: dict[str, JointIndices] = {}
        for joint in manifest["anatomical_joints"]:
            name = str(joint["name"])
            self.joint_indices[name] = JointIndices(
                self.name_to_index[f"{name}_x"],
                self.name_to_index[f"{name}_y"],
                self.name_to_index[f"{name}_z"],
            )
        self.couplings = {
            str(joint["name"]): dict(joint.get("coupling", {"type": "none"}))
            for joint in manifest["anatomical_joints"]
            if str(joint.get("coupling", {}).get("type", "none")) != "none"
        }

    def normalized_to_targets(self, actions: torch.Tensor) -> torch.Tensor:
        if actions.shape[-1] != DOF_COUNT:
            raise ValueError(f"Expected actions[..., {DOF_COUNT}], got {tuple(actions.shape)}")
        action = actions.clamp(-1.0, 1.0)
        target = torch.where(
            action >= 0.0,
            action * self.upper * self.target_fraction,
            (-action) * self.lower * self.target_fraction,
        )
        target = torch.maximum(torch.minimum(target, self.upper), self.lower)
        self._apply_coupled_rom(target)
        return target

    def _apply_coupled_rom(self, target: torch.Tensor) -> None:
        for joint_name, coupling in self.couplings.items():
            kind = str(coupling.get("type", "none"))
            indices = self.joint_indices[joint_name]
            if kind == "knee_screw_home":
                self._clamp_knee(target, indices, coupling)
            elif kind == "hip_deep_flexion":
                self._clamp_hip(target, indices, coupling)
            elif kind == "shoulder_elevation_twist":
                self._clamp_shoulder(target, indices, coupling)
            elif kind == "ankle_sagittal_tightening":
                self._clamp_ankle(target, indices, coupling)
            elif kind != "none":
                raise ValueError(f"Unsupported anatomical coupling type: {kind}")

    def _clamp_knee(self, target: torch.Tensor, idx: JointIndices, c: dict[str, Any]) -> None:
        x_deg = torch.rad2deg(target[..., idx.x])
        flex = (float(c["flexion_sign"]) * x_deg).clamp(
            float(c["flexion_range_deg"][0]), float(c["flexion_range_deg"][1])
        )
        unlock = _smoothstep(float(c["unlock_smoothstep_deg"][0]), float(c["unlock_smoothstep_deg"][1]), flex)
        axial = torch.deg2rad(_lerp_pair(c["axial_abs_deg"], unlock))
        frontal = torch.deg2rad(_lerp_pair(c["frontal_abs_deg"], unlock))
        target[..., idx.y] = torch.maximum(torch.minimum(target[..., idx.y], axial), -axial)
        target[..., idx.z] = torch.maximum(torch.minimum(target[..., idx.z], frontal), -frontal)

    def _clamp_hip(self, target: torch.Tensor, idx: JointIndices, c: dict[str, Any]) -> None:
        flex_deg = torch.rad2deg(target[..., idx.x]).clamp(0.0, 125.0)
        deep = _smoothstep(float(c["flexion_smoothstep_deg"][0]), float(c["flexion_smoothstep_deg"][1]), flex_deg)
        internal = torch.deg2rad(_lerp_pair(c["internal_rotation_deg"], deep))
        external = torch.deg2rad(_lerp_pair(c["external_rotation_deg"], deep))
        abduction = torch.deg2rad(_lerp_pair(c["abduction_deg"], deep))
        adduction = torch.deg2rad(_lerp_pair(c["adduction_deg"], deep))
        if str(c["side"]) == "left":
            y_low, y_high = -internal, external
            z_low, z_high = -abduction, adduction
        else:
            y_low, y_high = -external, internal
            z_low, z_high = -adduction, abduction
        target[..., idx.y] = torch.maximum(torch.minimum(target[..., idx.y], y_high), y_low)
        target[..., idx.z] = torch.maximum(torch.minimum(target[..., idx.z], z_high), z_low)

    def _clamp_shoulder(self, target: torch.Tensor, idx: JointIndices, c: dict[str, Any]) -> None:
        flex_fraction = (torch.rad2deg(target[..., idx.x]).abs() / float(c["elevation_flex_divisor_deg"])).clamp(0.0, 1.0)
        lateral_fraction = (torch.rad2deg(target[..., idx.z]).abs() / float(c["elevation_lateral_divisor_deg"])).clamp(0.0, 1.0)
        elevation = torch.maximum(flex_fraction, lateral_fraction)
        tighten = _smoothstep(float(c["tighten_smoothstep"][0]), float(c["tighten_smoothstep"][1]), elevation)
        axial = torch.deg2rad(_lerp_pair(c["axial_abs_deg"], tighten))
        target[..., idx.y] = torch.maximum(torch.minimum(target[..., idx.y], axial), -axial)

    def _clamp_ankle(self, target: torch.Tensor, idx: JointIndices, c: dict[str, Any]) -> None:
        sagittal_deg = torch.rad2deg(target[..., idx.x])
        center_distance = (
            (sagittal_deg - float(c["sagittal_center_deg"])).abs() / float(c["sagittal_half_range_deg"])
        ).clamp(0.0, 1.0)
        tighten = _smoothstep(float(c["tighten_smoothstep"][0]), float(c["tighten_smoothstep"][1]), center_distance)
        y_scale = _lerp_pair(c["y_scale"], tighten)
        z_scale = _lerp_pair(c["z_scale"], tighten)
        y_low = self.lower[idx.y] * y_scale
        y_high = self.upper[idx.y] * y_scale
        z_low = self.lower[idx.z] * z_scale
        z_high = self.upper[idx.z] * z_scale
        target[..., idx.y] = torch.maximum(torch.minimum(target[..., idx.y], y_high), y_low)
        target[..., idx.z] = torch.maximum(torch.minimum(target[..., idx.z], z_high), z_low)

    @staticmethod
    def canonical_targets_to_physx(
        canonical_targets: torch.Tensor,
        canonical_to_physx_ids: torch.Tensor,
        template_physx: torch.Tensor,
    ) -> torch.Tensor:
        result = template_physx.clone()
        result[:, canonical_to_physx_ids] = canonical_targets
        return result

    @staticmethod
    def physx_to_canonical(values_physx: torch.Tensor, canonical_to_physx_ids: torch.Tensor) -> torch.Tensor:
        return values_physx.index_select(1, canonical_to_physx_ids)
