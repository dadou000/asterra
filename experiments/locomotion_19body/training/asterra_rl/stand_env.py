"""Direct Isaac Lab stand task for the Asterra 19-body foundation controller."""

from __future__ import annotations

import math
from collections.abc import Sequence

import torch

import isaaclab.sim as sim_utils
from isaaclab.assets import Articulation
from isaaclab.envs import DirectRLEnv, DirectRLEnvCfg
from isaaclab.scene import InteractiveSceneCfg
from isaaclab.sensors import ContactSensor, ContactSensorCfg
from isaaclab.sim import SimulationCfg
from isaaclab.sim.spawners.from_files import GroundPlaneCfg, spawn_ground_plane
from isaaclab.utils import configclass

from .actions import CanonicalActionProcessor, DOF_COUNT
from .articulation import (
    canonical_to_physx_joint_ids,
    load_articulation_manifest,
    make_articulation_cfg,
    resolve_training_usd,
    standing_root_height_m,
    validate_live_articulation,
)
from .observations import (
    KEY_BODY_NAMES,
    OBSERVATION_SIZE,
    build_foundation_observation,
    gravity_frame_axes,
    normalize_joint_position,
    world_to_gravity_frame,
)
from .rewards import compute_stand_reward, horizontal_distance_to_support, stand_termination, whole_body_com

_MANIFEST = load_articulation_manifest()
_USD_PATH = resolve_training_usd()


@configclass
class AsterraStandEnvCfg(DirectRLEnvCfg):
    # Foundation policy: 240 Hz physics / 60 Hz policy.
    decimation = 4
    episode_length_s = 8.0
    action_space = DOF_COUNT
    observation_space = OBSERVATION_SIZE
    state_space = 0

    sim: SimulationCfg = SimulationCfg(
        dt=1.0 / 240.0,
        render_interval=decimation,
        gravity=(0.0, 0.0, -9.81),
    )
    scene: InteractiveSceneCfg = InteractiveSceneCfg(
        num_envs=2048,
        env_spacing=2.5,
        replicate_physics=True,
        clone_in_fabric=True,
    )
    robot = make_articulation_cfg(
        _USD_PATH,
        _MANIFEST,
        prim_path="/World/envs/env_.*/Robot",
        passive=False,
    )
    feet_contact = ContactSensorCfg(
        prim_path="{ENV_REGEX_NS}/Robot/.*_foot",
        update_period=0.0,
        history_length=3,
        track_air_time=True,
        debug_vis=False,
    )

    # Stage-1 stand curriculum. Later recovery stages add pushes and stronger noise.
    action_target_fraction = 0.85
    reset_joint_noise_deg = 2.0
    reset_linear_velocity_m_s = 0.08
    reset_angular_velocity_rad_s = 0.12
    min_root_height_m = 0.55
    min_upright_cos = 0.35
    max_horizontal_distance_m = 2.0


class AsterraStandEnv(DirectRLEnv):
    cfg: AsterraStandEnvCfg

    def __init__(self, cfg: AsterraStandEnvCfg, render_mode: str | None = None, **kwargs):
        super().__init__(cfg, render_mode, **kwargs)

        self.manifest = _MANIFEST
        self.target_root_height = standing_root_height_m(self.manifest)
        canonical_to_physx = canonical_to_physx_joint_ids(self._robot, self.manifest)
        self._canonical_to_physx = torch.tensor(canonical_to_physx, device=self.device, dtype=torch.long)
        self._action_processor = CanonicalActionProcessor(
            self.manifest,
            device=self.device,
            dtype=self._robot.data.joint_pos.dtype,
            target_fraction=self.cfg.action_target_fraction,
        )

        key_ids, key_names = self._robot.find_bodies(list(KEY_BODY_NAMES), preserve_order=True)
        if list(key_names) != list(KEY_BODY_NAMES):
            raise RuntimeError(f"Key-body order mismatch: {key_names}")
        self._key_body_ids = torch.tensor(key_ids, device=self.device, dtype=torch.long)
        foot_ids, foot_names = self._contact_sensor.find_bodies(["left_foot", "right_foot"], preserve_order=True)
        if list(foot_names) != ["left_foot", "right_foot"]:
            raise RuntimeError(f"Foot contact order mismatch: {foot_names}")
        self._foot_sensor_ids = torch.tensor(foot_ids, device=self.device, dtype=torch.long)
        robot_foot_ids, robot_foot_names = self._robot.find_bodies(["left_foot", "right_foot"], preserve_order=True)
        if list(robot_foot_names) != ["left_foot", "right_foot"]:
            raise RuntimeError(f"Robot foot order mismatch: {robot_foot_names}")
        self._robot_foot_ids = torch.tensor(robot_foot_ids, device=self.device, dtype=torch.long)

        self._actions = torch.zeros((self.num_envs, DOF_COUNT), device=self.device)
        self._previous_actions = torch.zeros_like(self._actions)
        self._commands = torch.zeros((self.num_envs, 3), device=self.device)
        self._phase = torch.zeros((self.num_envs, 2), device=self.device)
        self._phase[:, 1] = 1.0

        self._gravity_w = torch.tensor(self.cfg.sim.gravity, device=self.device, dtype=torch.float32)
        self._reference_forward_w = torch.tensor((0.0, 1.0, 0.0), device=self.device, dtype=torch.float32)
        self._gravity_up_w = -self._gravity_w / torch.linalg.vector_norm(self._gravity_w)
        self._body_mass = self._robot.data.default_mass
        self._total_physical_mass = float(self.manifest["total_physical_mass_kg"])
        self._half_body_weight = 0.5 * self._total_physical_mass * torch.linalg.vector_norm(self._gravity_w).item()

        self._episode_sums: dict[str, torch.Tensor] = {}
        self._contract_summary = validate_live_articulation(self._robot, self.manifest)
        print(
            "Asterra stand env ready: "
            f"{self.num_envs} envs, obs={OBSERVATION_SIZE}, action={DOF_COUNT}, "
            f"policy={1.0 / self.step_dt:.1f} Hz, physics={1.0 / self.cfg.sim.dt:.1f} Hz"
        )

    def _setup_scene(self) -> None:
        self._robot = Articulation(self.cfg.robot)
        self.scene.articulations["robot"] = self._robot
        self._contact_sensor = ContactSensor(self.cfg.feet_contact)
        self.scene.sensors["feet_contact"] = self._contact_sensor

        spawn_ground_plane(
            prim_path="/World/ground",
            cfg=GroundPlaneCfg(
                physics_material=sim_utils.RigidBodyMaterialCfg(
                    static_friction=0.90,
                    dynamic_friction=0.90,
                    restitution=0.02,
                )
            ),
        )
        self.scene.clone_environments(copy_from_source=False)
        if self.device == "cpu":
            self.scene.filter_collisions(global_prim_paths=["/World/ground"])
        light_cfg = sim_utils.DomeLightCfg(intensity=2000.0, color=(0.75, 0.78, 0.84))
        light_cfg.func("/World/Light", light_cfg)

    def _pre_physics_step(self, actions: torch.Tensor) -> None:
        self._previous_actions.copy_(self._actions)
        self._actions.copy_(actions.clamp(-1.0, 1.0))
        target_canonical = self._action_processor.normalized_to_targets(self._actions)
        self._target_physx = self._action_processor.canonical_targets_to_physx(
            target_canonical,
            self._canonical_to_physx,
            self._robot.data.default_joint_pos,
        )

    def _apply_action(self) -> None:
        self._robot.set_joint_position_target(self._target_physx)
        self._robot.set_joint_velocity_target(torch.zeros_like(self._target_physx))

    def _foot_contact_load(self) -> torch.Tensor:
        force = self._contact_sensor.data.net_forces_w.index_select(1, self._foot_sensor_ids)
        magnitude = torch.linalg.vector_norm(force, dim=-1)
        return (magnitude / max(self._half_body_weight, 1.0)).clamp(0.0, 2.0)

    def _canonical_joint_state(self) -> tuple[torch.Tensor, torch.Tensor]:
        q = self._action_processor.physx_to_canonical(self._robot.data.joint_pos, self._canonical_to_physx)
        qd = self._action_processor.physx_to_canonical(self._robot.data.joint_vel, self._canonical_to_physx)
        return q, qd

    def _get_observations(self) -> dict[str, torch.Tensor]:
        root = self._robot.data.root_state_w
        q, qd = self._canonical_joint_state()
        obs = build_foundation_observation(
            root_pos_w=root[:, 0:3],
            root_quat_w=root[:, 3:7],
            root_lin_vel_w=root[:, 7:10],
            root_ang_vel_w=root[:, 10:13],
            body_pos_w=self._robot.data.body_state_w[..., 0:3],
            env_origins_w=self.scene.env_origins,
            key_body_ids=self._key_body_ids,
            joint_pos_canonical=q,
            joint_vel_canonical=qd,
            joint_lower_canonical=self._action_processor.lower,
            joint_upper_canonical=self._action_processor.upper,
            joint_velocity_limits_canonical=self._action_processor.velocity_limits,
            previous_action=self._actions,
            foot_contact_load=self._foot_contact_load(),
            command=self._commands,
            phase=self._phase,
            gravity_w=self._gravity_w,
            reference_forward_w=self._reference_forward_w,
        )
        return {"policy": obs}

    def _stand_state(self) -> dict[str, torch.Tensor]:
        root = self._robot.data.root_state_w
        root_delta = root[:, 0:3] - self.scene.env_origins
        root_height = torch.sum(root_delta * self._gravity_up_w, dim=-1)
        root_horizontal = root_delta - root_height.unsqueeze(-1) * self._gravity_up_w
        root_horizontal_distance = torch.linalg.vector_norm(root_horizontal, dim=-1)
        upright_cos = (-self._robot.data.projected_gravity_b[:, 2]).clamp(-1.0, 1.0)

        right, forward, up = gravity_frame_axes(self._gravity_w, self._reference_forward_w)
        right = right.expand(self.num_envs, -1)
        forward = forward.expand(self.num_envs, -1)
        up = up.expand(self.num_envs, -1)
        root_lin_gravity = world_to_gravity_frame(root[:, 7:10], right, forward, up)
        root_ang_gravity = world_to_gravity_frame(root[:, 10:13], right, forward, up)

        q, qd = self._canonical_joint_state()
        q_norm = normalize_joint_position(q, self._action_processor.lower, self._action_processor.upper)
        qd_norm = (qd / self._action_processor.velocity_limits.clamp_min(1.0e-5)).clamp(-2.0, 2.0)
        torque_canonical = self._action_processor.physx_to_canonical(
            self._robot.data.applied_torque, self._canonical_to_physx
        )
        torque_norm = torque_canonical / self._action_processor.effort_limits.clamp_min(1.0e-5)

        com = whole_body_com(self._robot.data.body_com_state_w[..., 0:3], self._body_mass)
        feet = self._robot.data.body_state_w.index_select(1, self._robot_foot_ids)[..., 0:3]
        gravity_up = self._gravity_up_w.unsqueeze(0).expand(self.num_envs, -1)
        com_support_distance = horizontal_distance_to_support(com, feet[:, 0], feet[:, 1], gravity_up)

        return {
            "root_height": root_height,
            "root_horizontal_distance": root_horizontal_distance,
            "upright_cos": upright_cos,
            "root_lin_gravity": root_lin_gravity,
            "root_ang_gravity": root_ang_gravity,
            "q_norm": q_norm,
            "qd_norm": qd_norm,
            "torque_norm": torque_norm,
            "com_support_distance": com_support_distance,
            "foot_contact_load": self._foot_contact_load(),
        }

    def _get_rewards(self) -> torch.Tensor:
        state = self._stand_state()
        reward, terms = compute_stand_reward(
            root_height=state["root_height"],
            target_root_height=self.target_root_height,
            upright_cos=state["upright_cos"],
            root_lin_vel_gravity=state["root_lin_gravity"],
            root_ang_vel_gravity=state["root_ang_gravity"],
            com_support_distance=state["com_support_distance"],
            foot_contact_load=state["foot_contact_load"],
            normalized_joint_pos=state["q_norm"],
            normalized_joint_vel=state["qd_norm"],
            normalized_torque=state["torque_norm"],
            actions=self._actions,
            previous_actions=self._previous_actions,
            step_dt=self.step_dt,
        )
        for name, value in terms.items():
            if name not in self._episode_sums:
                self._episode_sums[name] = torch.zeros(self.num_envs, device=self.device)
            self._episode_sums[name] += value
        return reward

    def _get_dones(self) -> tuple[torch.Tensor, torch.Tensor]:
        state = self._stand_state()
        terminated = stand_termination(
            root_height=state["root_height"],
            upright_cos=state["upright_cos"],
            horizontal_root_distance=state["root_horizontal_distance"],
            min_root_height=self.cfg.min_root_height_m,
            min_upright_cos=self.cfg.min_upright_cos,
            max_horizontal_distance=self.cfg.max_horizontal_distance_m,
        )
        time_out = self.episode_length_buf >= self.max_episode_length - 1
        return terminated, time_out

    def _reset_idx(self, env_ids: Sequence[int] | torch.Tensor | None) -> None:
        if env_ids is None:
            env_ids = self._robot._ALL_INDICES
        if not isinstance(env_ids, torch.Tensor):
            env_ids = torch.as_tensor(env_ids, device=self.device, dtype=torch.long)

        if len(env_ids) > 0 and self._episode_sums:
            self.extras["log"] = {}
            for name, values in self._episode_sums.items():
                self.extras["log"][f"Episode_Reward/{name}"] = torch.mean(values[env_ids]) / self.max_episode_length_s
                values[env_ids] = 0.0
            self.extras["log"]["Episode_Termination/fall"] = int(torch.count_nonzero(self.reset_terminated[env_ids]).item())
            self.extras["log"]["Episode_Termination/time_out"] = int(torch.count_nonzero(self.reset_time_outs[env_ids]).item())

        self._robot.reset(env_ids)
        super()._reset_idx(env_ids)
        self._actions[env_ids] = 0.0
        self._previous_actions[env_ids] = 0.0
        self._commands[env_ids] = 0.0
        self._phase[env_ids, 0] = 0.0
        self._phase[env_ids, 1] = 1.0

        root_state = self._robot.data.default_root_state[env_ids].clone()
        root_state[:, :3] += self.scene.env_origins[env_ids]
        if self.cfg.reset_linear_velocity_m_s > 0.0:
            root_state[:, 7:10] += torch.empty_like(root_state[:, 7:10]).uniform_(
                -self.cfg.reset_linear_velocity_m_s, self.cfg.reset_linear_velocity_m_s
            )
        if self.cfg.reset_angular_velocity_rad_s > 0.0:
            root_state[:, 10:13] += torch.empty_like(root_state[:, 10:13]).uniform_(
                -self.cfg.reset_angular_velocity_rad_s, self.cfg.reset_angular_velocity_rad_s
            )

        joint_pos_physx = self._robot.data.default_joint_pos[env_ids].clone()
        joint_vel_physx = self._robot.data.default_joint_vel[env_ids].clone()
        if self.cfg.reset_joint_noise_deg > 0.0:
            canonical = self._action_processor.physx_to_canonical(joint_pos_physx, self._canonical_to_physx)
            amplitude = math.radians(self.cfg.reset_joint_noise_deg)
            canonical += torch.empty_like(canonical).uniform_(-amplitude, amplitude)
            canonical = torch.maximum(torch.minimum(canonical, self._action_processor.upper), self._action_processor.lower)
            joint_pos_physx[:, self._canonical_to_physx] = canonical

        self._robot.write_root_pose_to_sim(root_state[:, :7], env_ids)
        self._robot.write_root_velocity_to_sim(root_state[:, 7:], env_ids)
        self._robot.write_joint_state_to_sim(joint_pos_physx, joint_vel_physx, None, env_ids)
