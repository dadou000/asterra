"""Canonical observation construction for the Asterra humanoid foundation policy.

The policy ABI is intentionally independent of PhysX joint traversal order and of a
fixed planetary/world up axis. The caller supplies a gravity direction and reference
forward vector; all spatial features are expressed in that local gravity-aligned
frame before concatenation.
"""

from __future__ import annotations

import torch

DOF_COUNT = 54
KEY_BODY_NAMES = ("head", "left_hand", "right_hand", "left_foot", "right_foot")
FIXED_FEATURE_COUNT = 35
OBSERVATION_SIZE = FIXED_FEATURE_COUNT + 3 * DOF_COUNT


def _normalize(v: torch.Tensor, eps: float = 1.0e-8) -> torch.Tensor:
    return v / torch.linalg.vector_norm(v, dim=-1, keepdim=True).clamp_min(eps)


def quat_apply_wxyz(q: torch.Tensor, v: torch.Tensor) -> torch.Tensor:
    """Rotate vectors by unit quaternions stored as ``w,x,y,z``."""
    q_vec = q[..., 1:4]
    q_w = q[..., 0:1]
    t = 2.0 * torch.cross(q_vec, v, dim=-1)
    return v + q_w * t + torch.cross(q_vec, t, dim=-1)


def gravity_frame_axes(
    gravity_w: torch.Tensor,
    reference_forward_w: torch.Tensor,
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """Return right/forward/up unit axes for a gravity-aligned frame."""
    if gravity_w.ndim == 1:
        gravity_w = gravity_w.unsqueeze(0)
    if reference_forward_w.ndim == 1:
        reference_forward_w = reference_forward_w.unsqueeze(0).expand(gravity_w.shape[0], -1)
    if gravity_w.shape[0] == 1 and reference_forward_w.shape[0] > 1:
        gravity_w = gravity_w.expand(reference_forward_w.shape[0], -1)

    up = -_normalize(gravity_w)
    forward = reference_forward_w - torch.sum(reference_forward_w * up, dim=-1, keepdim=True) * up
    weak = torch.linalg.vector_norm(forward, dim=-1, keepdim=True) < 1.0e-4
    fallback = torch.tensor((1.0, 0.0, 0.0), device=up.device, dtype=up.dtype).expand_as(up)
    fallback = fallback - torch.sum(fallback * up, dim=-1, keepdim=True) * up
    forward = torch.where(weak, fallback, forward)
    forward = _normalize(forward)
    right = _normalize(torch.cross(forward, up, dim=-1))
    forward = _normalize(torch.cross(up, right, dim=-1))
    return right, forward, up


def world_to_gravity_frame(
    vectors_w: torch.Tensor,
    right_w: torch.Tensor,
    forward_w: torch.Tensor,
    up_w: torch.Tensor,
) -> torch.Tensor:
    """Express world-space vectors in the supplied gravity-aligned frame."""
    while right_w.ndim < vectors_w.ndim:
        right_w = right_w.unsqueeze(1)
        forward_w = forward_w.unsqueeze(1)
        up_w = up_w.unsqueeze(1)
    return torch.stack(
        (
            torch.sum(vectors_w * right_w, dim=-1),
            torch.sum(vectors_w * forward_w, dim=-1),
            torch.sum(vectors_w * up_w, dim=-1),
        ),
        dim=-1,
    )


def normalize_joint_position(
    joint_pos: torch.Tensor,
    lower: torch.Tensor,
    upper: torch.Tensor,
) -> torch.Tensor:
    """Normalize asymmetric neutral-centered ROM to approximately ``[-1, 1]``."""
    positive_extent = upper.clamp_min(1.0e-5)
    negative_extent = (-lower).clamp_min(1.0e-5)
    normalized = torch.where(joint_pos >= 0.0, joint_pos / positive_extent, joint_pos / negative_extent)
    return normalized.clamp(-1.5, 1.5)


def build_foundation_observation(
    *,
    root_pos_w: torch.Tensor,
    root_quat_w: torch.Tensor,
    root_lin_vel_w: torch.Tensor,
    root_ang_vel_w: torch.Tensor,
    body_pos_w: torch.Tensor,
    env_origins_w: torch.Tensor,
    key_body_ids: torch.Tensor,
    joint_pos_canonical: torch.Tensor,
    joint_vel_canonical: torch.Tensor,
    joint_lower_canonical: torch.Tensor,
    joint_upper_canonical: torch.Tensor,
    joint_velocity_limits_canonical: torch.Tensor,
    previous_action: torch.Tensor,
    foot_contact_load: torch.Tensor,
    command: torch.Tensor,
    phase: torch.Tensor,
    gravity_w: torch.Tensor,
    reference_forward_w: torch.Tensor,
) -> torch.Tensor:
    """Build the permanent 197-value foundation-policy observation vector.

    Layout::

        0       root height along anti-gravity
        1:7     root forward + up axes in gravity frame (6D orientation)
        7:10    root linear velocity in gravity frame
        10:13   root angular velocity in gravity frame
        13:28   five key-body positions relative to root in gravity frame
        28:30   normalized left/right foot contact load
        30:33   task/locomotion command xyz
        33:35   phase sin/cos
        35:89   canonical joint position
        89:143  canonical joint velocity
        143:197 previous canonical action
    """
    if joint_pos_canonical.shape[-1] != DOF_COUNT:
        raise ValueError(f"Expected {DOF_COUNT} joint positions, got {joint_pos_canonical.shape[-1]}")
    if previous_action.shape[-1] != DOF_COUNT:
        raise ValueError(f"Expected {DOF_COUNT} previous actions, got {previous_action.shape[-1]}")
    if foot_contact_load.shape[-1] != 2 or command.shape[-1] != 3 or phase.shape[-1] != 2:
        raise ValueError("Foot contact/command/phase shapes do not match foundation ABI")

    env_count = root_pos_w.shape[0]
    gravity = gravity_w
    if gravity.ndim == 1:
        gravity = gravity.unsqueeze(0).expand(env_count, -1)
    reference = reference_forward_w
    if reference.ndim == 1:
        reference = reference.unsqueeze(0).expand(env_count, -1)
    right, forward, up = gravity_frame_axes(gravity, reference)

    root_from_origin = root_pos_w - env_origins_w
    root_height = torch.sum(root_from_origin * up, dim=-1, keepdim=True)

    local_forward = torch.tensor((0.0, 1.0, 0.0), device=root_pos_w.device, dtype=root_pos_w.dtype).expand(env_count, -1)
    local_up = torch.tensor((0.0, 0.0, 1.0), device=root_pos_w.device, dtype=root_pos_w.dtype).expand(env_count, -1)
    body_forward_w = quat_apply_wxyz(root_quat_w, local_forward)
    body_up_w = quat_apply_wxyz(root_quat_w, local_up)
    orientation_6d = torch.cat(
        (
            world_to_gravity_frame(body_forward_w, right, forward, up),
            world_to_gravity_frame(body_up_w, right, forward, up),
        ),
        dim=-1,
    )

    root_lin = world_to_gravity_frame(root_lin_vel_w, right, forward, up)
    root_ang = world_to_gravity_frame(root_ang_vel_w, right, forward, up)

    key_positions = body_pos_w.index_select(1, key_body_ids) - root_pos_w.unsqueeze(1)
    key_positions = world_to_gravity_frame(key_positions, right, forward, up).reshape(env_count, -1)

    q = normalize_joint_position(joint_pos_canonical, joint_lower_canonical, joint_upper_canonical)
    qd = (joint_vel_canonical / joint_velocity_limits_canonical.clamp_min(1.0e-5)).clamp(-2.0, 2.0)

    observation = torch.cat(
        (
            root_height,
            orientation_6d,
            root_lin,
            root_ang,
            key_positions,
            foot_contact_load.clamp(0.0, 2.0),
            command,
            phase,
            q,
            qd,
            previous_action.clamp(-1.0, 1.0),
        ),
        dim=-1,
    )
    if observation.shape[-1] != OBSERVATION_SIZE:
        raise RuntimeError(f"Foundation observation ABI drift: {observation.shape[-1]} != {OBSERVATION_SIZE}")
    return observation
