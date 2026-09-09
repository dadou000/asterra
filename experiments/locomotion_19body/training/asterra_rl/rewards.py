"""Stand-stage rewards for the Asterra physics-first humanoid controller."""

from __future__ import annotations

from typing import Mapping

import torch


def whole_body_com(body_com_pos_w: torch.Tensor, body_mass: torch.Tensor) -> torch.Tensor:
    if body_mass.ndim == 1:
        body_mass = body_mass.unsqueeze(0).expand(body_com_pos_w.shape[0], -1)
    weights = body_mass / body_mass.sum(dim=1, keepdim=True).clamp_min(1.0e-8)
    return torch.sum(body_com_pos_w * weights.unsqueeze(-1), dim=1)


def horizontal_distance_to_support(
    com_w: torch.Tensor,
    left_foot_w: torch.Tensor,
    right_foot_w: torch.Tensor,
    gravity_up_w: torch.Tensor,
) -> torch.Tensor:
    support_mid = 0.5 * (left_foot_w + right_foot_w)
    delta = com_w - support_mid
    vertical = torch.sum(delta * gravity_up_w, dim=-1, keepdim=True) * gravity_up_w
    return torch.linalg.vector_norm(delta - vertical, dim=-1)


def compute_stand_reward(
    *,
    root_height: torch.Tensor,
    target_root_height: float,
    upright_cos: torch.Tensor,
    root_lin_vel_gravity: torch.Tensor,
    root_ang_vel_gravity: torch.Tensor,
    com_support_distance: torch.Tensor,
    foot_contact_load: torch.Tensor,
    normalized_joint_pos: torch.Tensor,
    normalized_joint_vel: torch.Tensor,
    normalized_torque: torch.Tensor,
    actions: torch.Tensor,
    previous_actions: torch.Tensor,
    step_dt: float,
) -> tuple[torch.Tensor, Mapping[str, torch.Tensor]]:
    """Dense stand reward with physical stability as the primary objective."""
    height_error = (root_height - float(target_root_height)) / 0.12
    height = torch.exp(-torch.square(height_error))
    upright = torch.exp(-4.0 * torch.square(1.0 - upright_cos.clamp(-1.0, 1.0)))
    com_support = torch.exp(-torch.square(com_support_distance / 0.10))
    supported_feet = (foot_contact_load > 0.05).float().mean(dim=-1)
    pose = torch.exp(-torch.mean(torch.square(normalized_joint_pos), dim=-1) / 0.35)
    root_lin_penalty = torch.sum(torch.square(root_lin_vel_gravity), dim=-1)
    root_ang_penalty = torch.sum(torch.square(root_ang_vel_gravity), dim=-1)
    joint_vel_penalty = torch.mean(torch.square(normalized_joint_vel), dim=-1)
    torque_penalty = torch.mean(torch.square(normalized_torque), dim=-1)
    action_rate_penalty = torch.mean(torch.square(actions - previous_actions), dim=-1)

    terms = {
        "alive": torch.ones_like(root_height) * 0.50,
        "height": height * 2.00,
        "upright": upright * 2.50,
        "com_support": com_support * 2.00,
        "feet_supported": supported_feet * 0.50,
        "neutral_pose": pose * 0.35,
        "root_linear_velocity": -0.20 * root_lin_penalty,
        "root_angular_velocity": -0.15 * root_ang_penalty,
        "joint_velocity": -0.025 * joint_vel_penalty,
        "normalized_torque": -0.025 * torque_penalty,
        "action_rate": -0.050 * action_rate_penalty,
    }
    total = torch.stack(tuple(terms.values()), dim=0).sum(dim=0) * float(step_dt)
    return total, {name: value * float(step_dt) for name, value in terms.items()}


def stand_termination(
    *,
    root_height: torch.Tensor,
    upright_cos: torch.Tensor,
    horizontal_root_distance: torch.Tensor,
    min_root_height: float = 0.55,
    min_upright_cos: float = 0.35,
    max_horizontal_distance: float = 2.0,
) -> torch.Tensor:
    return (
        (root_height < float(min_root_height))
        | (upright_cos < float(min_upright_cos))
        | (horizontal_root_distance > float(max_horizontal_distance))
    )
