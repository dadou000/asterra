#!/usr/bin/env python3
"""Build the Stage-0A Asterra training articulation.

Pure Python by design: validation and asset generation do not require Isaac Sim.
The 19 semantic rigid bodies remain the only meaningful masses/colliders. Each
3-axis anatomical joint is expanded into X/Y/Z co-located revolute joints with
two non-colliding near-massless frame links so Isaac Lab can use its mature
revolute-joint actuator path.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

AXES = ("x", "y", "z")
IDENTITY = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def canonical_sha256(data: dict) -> str:
    raw = json.dumps(data, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


def vec3(value, label: str = "vec3") -> tuple[float, float, float]:
    if not isinstance(value, list) or len(value) != 3:
        raise ValueError(f"{label} must be a 3-element JSON array")
    result = tuple(float(v) for v in value)
    if not all(math.isfinite(v) for v in result):
        raise ValueError(f"{label} contains non-finite values")
    return result


def add(a, b):
    return tuple(a[i] + b[i] for i in range(3))


def sub(a, b):
    return tuple(a[i] - b[i] for i in range(3))


def mul(v, k: float):
    return tuple(k * x for x in v)


def dot(a, b) -> float:
    return sum(a[i] * b[i] for i in range(3))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(v) -> float:
    return math.sqrt(dot(v, v))


def normalized(v, label: str):
    length = norm(v)
    if length < 1.0e-8:
        raise ValueError(f"{label} has near-zero length")
    return tuple(x / length for x in v)


def transpose(m):
    return tuple(tuple(m[j][i] for j in range(3)) for i in range(3))


def mat_mul(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))


def mat_vec(m, v):
    return tuple(sum(m[i][j] * v[j] for j in range(3)) for i in range(3))


def determinant3(m) -> float:
    return (
        m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
        - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
        + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])
    )


def basis_matrix(x_axis, y_axis, z_axis):
    return tuple((x_axis[row], y_axis[row], z_axis[row]) for row in range(3))


def training_rotation(contract: dict):
    rows = contract["training_representation"]["asterra_to_training_rotation_rows"]
    if not isinstance(rows, list) or len(rows) != 3:
        raise ValueError("asterra_to_training_rotation_rows must be 3x3")
    matrix = tuple(vec3(row, "asterra_to_training_rotation_rows") for row in rows)
    gram = mat_mul(matrix, transpose(matrix))
    for i in range(3):
        for j in range(3):
            expected = 1.0 if i == j else 0.0
            if abs(gram[i][j] - expected) > 1.0e-8:
                raise ValueError("Asterra->training rotation must be orthonormal")
    if abs(determinant3(matrix) - 1.0) > 1.0e-8:
        raise ValueError("Asterra->training rotation must be right-handed")
    return matrix


def anatomical_basis(frame: dict):
    flex = normalized(vec3(frame["flex_axis_world"], "flex axis"), "flex axis")
    twist = normalized(vec3(frame["twist_axis_world"], "twist axis"), "twist axis")
    x_axis = normalized(sub(flex, mul(twist, dot(flex, twist))), "orthogonalized flex axis")
    y_axis = twist
    z_axis = normalized(cross(x_axis, y_axis), "lateral axis")
    return basis_matrix(x_axis, y_axis, z_axis)


def rpy_from_matrix(r):
    # URDF convention R = Rz(yaw) * Ry(pitch) * Rx(roll).
    pitch = math.asin(max(-1.0, min(1.0, -r[2][0])))
    cp = math.cos(pitch)
    if abs(cp) > 1.0e-8:
        roll = math.atan2(r[2][1], r[2][2])
        yaw = math.atan2(r[1][0], r[0][0])
    else:
        roll = math.atan2(-r[1][2], r[1][1])
        yaw = 0.0
    return roll, pitch, yaw


def fmt(values) -> str:
    return " ".join(f"{float(v):.10g}" for v in values)


def shape_inertia(body: dict):
    """Diagonal inertia in the body's neutral principal frame."""
    mass = float(body["mass_kg"])
    geom = body["geometry"]
    kind = geom["type"]
    if kind == "box":
        sx, sy, sz = vec3(geom["size_m"], f"{body['name']}.size")
        return (
            mass * (sy * sy + sz * sz) / 12.0,
            mass * (sx * sx + sz * sz) / 12.0,
            mass * (sx * sx + sy * sy) / 12.0,
        )
    if kind == "sphere":
        r = float(geom["radius_m"])
        value = 0.4 * mass * r * r
        return value, value, value
    if kind == "capsule":
        # Godot CapsuleShape3D height is total end-to-end height. Approximate the
        # exact inertia as a cylinder plus two solid hemispheres about body +Y.
        r = float(geom["radius_m"])
        height = float(geom["height_m"])
        cylinder_length = max(0.0, height - 2.0 * r)
        cylinder_volume = math.pi * r * r * cylinder_length
        sphere_volume = (4.0 / 3.0) * math.pi * r**3
        total_volume = cylinder_volume + sphere_volume
        if total_volume <= 0.0:
            raise ValueError(f"{body['name']}: zero-volume capsule")
        cylinder_mass = mass * cylinder_volume / total_volume
        hemisphere_mass = 0.5 * mass * sphere_volume / total_volume
        iyy_cylinder = 0.5 * cylinder_mass * r * r
        ixx_cylinder = cylinder_mass * (3.0 * r * r + cylinder_length**2) / 12.0
        hemisphere_com_offset = cylinder_length * 0.5 + 3.0 * r / 8.0
        iyy_hemisphere = 0.4 * hemisphere_mass * r * r
        ixx_hemisphere_com = (83.0 / 320.0) * hemisphere_mass * r * r
        iyy = iyy_cylinder + 2.0 * iyy_hemisphere
        ixx = ixx_cylinder + 2.0 * (
            ixx_hemisphere_com + hemisphere_mass * hemisphere_com_offset**2
        )
        return ixx, iyy, ixx
    raise ValueError(f"Unsupported geometry type: {kind}")


def region_by_joint(policy: dict) -> dict[str, str]:
    result: dict[str, str] = {}
    for region, names in policy["regions"].items():
        for name in names:
            if name in result:
                raise ValueError(f"Joint {name} belongs to multiple policy regions")
            result[str(name)] = str(region)
    return result


def validate_contract(contract: dict, policy: dict) -> None:
    bodies = contract.get("bodies", [])
    joints = contract.get("joints", [])
    if len(bodies) != 19 or len(joints) != 18:
        raise ValueError(f"Expected 19 bodies/18 joints, got {len(bodies)}/{len(joints)}")
    body_names = [str(body["name"]) for body in bodies]
    if len(set(body_names)) != 19:
        raise ValueError("Body names must be unique")
    body_set = set(body_names)
    root = str(contract["root_body"])
    if root not in body_set:
        raise ValueError(f"Missing root body {root}")

    joint_names: list[str] = []
    child_to_parent: dict[str, str] = {}
    for joint in joints:
        name = str(joint["name"])
        parent, child = str(joint["parent"]), str(joint["child"])
        joint_names.append(name)
        if parent not in body_set or child not in body_set:
            raise ValueError(f"{name}: unknown parent/child body")
        if child == root or child in child_to_parent:
            raise ValueError(f"{name}: invalid/multiple parent for {child}")
        child_to_parent[child] = parent
        frame = joint["frame"]
        flex = normalized(vec3(frame["flex_axis_world"]), f"{name}.flex")
        twist = normalized(vec3(frame["twist_axis_world"]), f"{name}.twist")
        if abs(dot(flex, twist)) >= 0.98 or frame.get("euler_order") != "XYZ":
            raise ValueError(f"{name}: invalid anatomical frame")
        actuator = joint["actuator"]
        for index, axis in enumerate(AXES):
            low, high = (float(v) for v in joint["limits_deg"][axis])
            if not (-179.0 <= low <= high <= 179.0):
                raise ValueError(f"{name}.{axis}: invalid limit {low}..{high}")
            for field in ("effort_limit_nm", "velocity_limit_rad_s", "stiffness_nm_per_rad", "damping_nms_per_rad"):
                values = actuator[field]
                if not isinstance(values, list) or len(values) != 3:
                    raise ValueError(f"{name}.{field}: expected 3 values")
                if not math.isfinite(float(values[index])) or float(values[index]) < 0.0:
                    raise ValueError(f"{name}.{field}[{index}] is invalid")
    if len(set(joint_names)) != 18:
        raise ValueError("Joint names must be unique")
    if set(child_to_parent) != body_set - {root}:
        raise ValueError("Articulation must be one 19-body tree")

    children = {name: [] for name in body_names}
    for child, parent in child_to_parent.items():
        children[parent].append(child)
    visited: set[str] = set()
    stack = [root]
    while stack:
        body = stack.pop()
        if body in visited:
            raise ValueError(f"Cycle at body {body}")
        visited.add(body)
        stack.extend(children[body])
    if visited != body_set:
        raise ValueError(f"Disconnected bodies: {sorted(body_set - visited)}")

    owners = region_by_joint(policy)
    if set(owners) != set(joint_names):
        missing = sorted(set(joint_names) - set(owners))
        extra = sorted(set(owners) - set(joint_names))
        raise ValueError(f"Policy region mismatch; missing={missing} extra={extra}")

    for body in bodies:
        if float(body["mass_kg"]) <= 0.0:
            raise ValueError(f"{body['name']}: non-positive mass")
        if not all(math.isfinite(v) and v > 0.0 for v in shape_inertia(body)):
            raise ValueError(f"{body['name']}: invalid inertia")

    rep = contract["training_representation"]
    if (int(rep["physical_body_count"]), int(rep["physical_joint_count"]), int(rep["actuated_dof_count"])) != (19, 18, 54):
        raise ValueError("Training representation count mismatch")
    if rep.get("joint_expansion") != "xyz_revolute_chain" or rep.get("training_up_axis") != "+Z":
        raise ValueError("Stage-0A training representation must be XYZ revolute and +Z-up")
    transform = training_rotation(contract)
    if max(abs(mat_vec(transform, (0.0, 1.0, 0.0))[i] - (0.0, 0.0, 1.0)[i]) for i in range(3)) > 1.0e-8:
        raise ValueError("Asterra +Y must map to training +Z")


def root_center_asterra(contract: dict):
    root = str(contract["root_body"])
    body = next(body for body in contract["bodies"] if body["name"] == root)
    return vec3(body["neutral_center_m"], f"{root}.center")


def point_to_training(contract: dict, point_asterra):
    return mat_vec(training_rotation(contract), sub(point_asterra, root_center_asterra(contract)))


def rotation_to_training(contract: dict, rotation_asterra):
    return mat_mul(training_rotation(contract), rotation_asterra)


def derive_link_frames(contract: dict):
    """Neutral training frames, pelvis-centered and +Z-up."""
    root = str(contract["root_body"])
    frames = {root: {"position_m": (0.0, 0.0, 0.0), "rotation": IDENTITY}}
    pending = list(contract["joints"])
    while pending:
        progressed = False
        for joint in pending[:]:
            parent, child = str(joint["parent"]), str(joint["child"])
            if parent not in frames:
                continue
            frames[child] = {
                "position_m": point_to_training(contract, vec3(joint["anchor_m"], f"{joint['name']}.anchor")),
                "rotation": rotation_to_training(contract, anatomical_basis(joint["frame"])),
            }
            pending.remove(joint)
            progressed = True
        if not progressed:
            raise ValueError("Unable to derive training link frames")
    return frames


def origin_relative(parent_frame: dict, child_position, child_rotation):
    parent_rt = transpose(parent_frame["rotation"])
    xyz = mat_vec(parent_rt, sub(child_position, parent_frame["position_m"]))
    rotation = mat_mul(parent_rt, child_rotation)
    return xyz, rpy_from_matrix(rotation)


def body_training_world_pose(contract: dict, body: dict):
    center = point_to_training(contract, vec3(body["neutral_center_m"], f"{body['name']}.center"))
    return center, training_rotation(contract)


def body_local_pose(contract: dict, body: dict, link_frame: dict):
    link_rt = transpose(link_frame["rotation"])
    center, body_rotation = body_training_world_pose(contract, body)
    xyz = mat_vec(link_rt, sub(center, link_frame["position_m"]))
    rotation = mat_mul(link_rt, body_rotation)
    return xyz, rpy_from_matrix(rotation)


def add_origin(parent, xyz, rpy=(0.0, 0.0, 0.0)):
    ET.SubElement(parent, "origin", xyz=fmt(xyz), rpy=fmt(rpy))


def add_physical_link(robot, body: dict, frame: dict, contract: dict):
    link = ET.SubElement(robot, "link", name=str(body["name"]))
    local_xyz, local_rpy = body_local_pose(contract, body, frame)
    inertia = shape_inertia(body)
    inertial = ET.SubElement(link, "inertial")
    add_origin(inertial, local_xyz, local_rpy)
    ET.SubElement(inertial, "mass", value=f"{float(body['mass_kg']):.10g}")
    ET.SubElement(inertial, "inertia", ixx=f"{inertia[0]:.12g}", ixy="0", ixz="0", iyy=f"{inertia[1]:.12g}", iyz="0", izz=f"{inertia[2]:.12g}")
    geom = body["geometry"]
    kind = geom["type"]
    if kind in ("box", "sphere"):
        for tag in ("visual", "collision"):
            node = ET.SubElement(link, tag)
            add_origin(node, local_xyz, local_rpy)
            geometry = ET.SubElement(node, "geometry")
            if kind == "box":
                ET.SubElement(geometry, "box", size=fmt(vec3(geom["size_m"])))
            else:
                ET.SubElement(geometry, "sphere", radius=f"{float(geom['radius_m']):.10g}")
        return
    if kind != "capsule":
        raise ValueError(f"Unsupported geometry {kind}")

    # Standard URDF has no capsule primitive: use a cylinder + two spheres.
    radius = float(geom["radius_m"])
    cylinder_length = max(1.0e-6, float(geom["height_m"]) - 2.0 * radius)
    cylinder_body_rotation = ((1.0, 0.0, 0.0), (0.0, 0.0, 1.0), (0.0, -1.0, 0.0))  # URDF Z -> body Y
    link_rt = transpose(frame["rotation"])
    cylinder_rpy = rpy_from_matrix(mat_mul(link_rt, rotation_to_training(contract, cylinder_body_rotation)))
    for tag in ("visual", "collision"):
        cylinder = ET.SubElement(link, tag)
        add_origin(cylinder, local_xyz, cylinder_rpy)
        geometry = ET.SubElement(cylinder, "geometry")
        ET.SubElement(geometry, "cylinder", radius=f"{radius:.10g}", length=f"{cylinder_length:.10g}")
        for sign in (-1.0, 1.0):
            cap_a = add(vec3(body["neutral_center_m"]), (0.0, sign * cylinder_length * 0.5, 0.0))
            cap_w = point_to_training(contract, cap_a)
            cap_local = mat_vec(link_rt, sub(cap_w, frame["position_m"]))
            sphere = ET.SubElement(link, tag)
            add_origin(sphere, cap_local)
            geometry = ET.SubElement(sphere, "geometry")
            ET.SubElement(geometry, "sphere", radius=f"{radius:.10g}")


def add_virtual_link(robot, name: str, contract: dict):
    rep = contract["training_representation"]
    mass = float(rep["virtual_link_mass_kg"])
    inertia = vec3(rep["virtual_link_inertia_kg_m2"])
    link = ET.SubElement(robot, "link", name=name)
    inertial = ET.SubElement(link, "inertial")
    add_origin(inertial, (0.0, 0.0, 0.0))
    ET.SubElement(inertial, "mass", value=f"{mass:.10g}")
    ET.SubElement(inertial, "inertia", ixx=f"{inertia[0]:.12g}", ixy="0", ixz="0", iyy=f"{inertia[1]:.12g}", iyz="0", izz=f"{inertia[2]:.12g}")


def add_revolute_joint(robot, name, parent, child, xyz, rpy, axis, limits, actuator, index):
    joint = ET.SubElement(robot, "joint", name=name, type="revolute")
    ET.SubElement(joint, "parent", link=parent)
    ET.SubElement(joint, "child", link=child)
    add_origin(joint, xyz, rpy)
    ET.SubElement(joint, "axis", xyz=fmt(axis))
    low, high = (math.radians(float(v)) for v in limits)
    ET.SubElement(joint, "limit", lower=f"{low:.12g}", upper=f"{high:.12g}", effort=f"{float(actuator['effort_limit_nm'][index]):.12g}", velocity=f"{float(actuator['velocity_limit_rad_s'][index]):.12g}")
    ET.SubElement(joint, "dynamics", damping="0", friction="0")


def build_manifest(contract: dict, policy: dict) -> dict:
    validate_contract(contract, policy)
    frames = derive_link_frames(contract)
    regions = region_by_joint(policy)
    dofs = []
    for joint in contract["joints"]:
        actuator = joint["actuator"]
        for axis_index, axis in enumerate(AXES):
            low_deg, high_deg = (float(v) for v in joint["limits_deg"][axis])
            dofs.append({
                "action_index": len(dofs),
                "name": f"{joint['name']}_{axis}",
                "anatomical_joint": str(joint["name"]),
                "axis": axis,
                "region": regions[str(joint["name"])],
                "lower_deg": low_deg,
                "upper_deg": high_deg,
                "lower_rad": math.radians(low_deg),
                "upper_rad": math.radians(high_deg),
                "effort_limit_nm": float(actuator["effort_limit_nm"][axis_index]),
                "velocity_limit_rad_s": float(actuator["velocity_limit_rad_s"][axis_index]),
                "stiffness_nm_per_rad": float(actuator["stiffness_nm_per_rad"][axis_index]),
                "damping_nms_per_rad": float(actuator["damping_nms_per_rad"][axis_index]),
                "calibration_status": str(actuator["calibration_status"]),
            })
    physical_bodies = []
    for body in contract["bodies"]:
        name = str(body["name"])
        local_xyz, local_rpy = body_local_pose(contract, body, frames[name])
        physical_bodies.append({
            "name": name,
            "mass_kg": float(body["mass_kg"]),
            "neutral_center_asterra_m": list(vec3(body["neutral_center_m"])),
            "neutral_center_training_root_relative_m": list(body_training_world_pose(contract, body)[0]),
            "inertia_diagonal_body_kg_m2": list(shape_inertia(body)),
            "training_link_frame_world": {"position_m": list(frames[name]["position_m"]), "rotation_matrix": [list(row) for row in frames[name]["rotation"]]},
            "body_pose_in_training_link": {"position_m": list(local_xyz), "rpy_rad": list(local_rpy)},
            "geometry": body["geometry"],
        })
    virtual_mass = 36 * float(contract["training_representation"]["virtual_link_mass_kg"])
    physical_mass = sum(float(body["mass_kg"]) for body in contract["bodies"])
    return {
        "schema_version": 1,
        "source_contract": contract["contract_name"],
        "source_contract_sha256": canonical_sha256(contract),
        "policy_architecture": policy["architecture"],
        "physical_body_count": 19,
        "anatomical_joint_count": 18,
        "training_virtual_link_count": 36,
        "training_link_count": 55,
        "actuated_dof_count": len(dofs),
        "total_physical_mass_kg": physical_mass,
        "training_virtual_mass_kg": virtual_mass,
        "training_total_mass_kg": physical_mass + virtual_mass,
        "root_body": contract["root_body"],
        "source_coordinate_system": contract["coordinate_system"],
        "training_coordinate_system": {
            "up": contract["training_representation"]["training_up_axis"],
            "forward": contract["training_representation"]["training_forward_axis"],
            "asterra_to_training_rotation_rows": contract["training_representation"]["asterra_to_training_rotation_rows"],
            "origin": "root_body_center",
        },
        "physical_bodies": physical_bodies,
        "anatomical_joints": contract["joints"],
        "dofs": dofs,
        "action_regions": policy["regions"],
        "runtime_contract": {"policy_hz": int(policy["runtime"]["policy_hz"]), "physics_hz": int(policy["runtime"]["physics_hz"]), "action_composition": policy["action_composition"]},
    }


def build_urdf(contract: dict) -> ET.ElementTree:
    frames = derive_link_frames(contract)
    robot = ET.Element("robot", name="asterra_19body_training")
    for body in contract["bodies"]:
        add_physical_link(robot, body, frames[str(body["name"])], contract)
    for joint in contract["joints"]:
        add_virtual_link(robot, f"frame__{joint['name']}_x", contract)
        add_virtual_link(robot, f"frame__{joint['name']}_y", contract)
    for joint in contract["joints"]:
        name, parent, child = str(joint["name"]), str(joint["parent"]), str(joint["child"])
        joint_position = point_to_training(contract, vec3(joint["anchor_m"], f"{name}.anchor"))
        joint_rotation = rotation_to_training(contract, anatomical_basis(joint["frame"]))
        first_xyz, first_rpy = origin_relative(frames[parent], joint_position, joint_rotation)
        x_link, y_link = f"frame__{name}_x", f"frame__{name}_y"
        actuator, limits = joint["actuator"], joint["limits_deg"]
        add_revolute_joint(robot, f"{name}_x", parent, x_link, first_xyz, first_rpy, (1,0,0), limits["x"], actuator, 0)
        add_revolute_joint(robot, f"{name}_y", x_link, y_link, (0,0,0), (0,0,0), (0,1,0), limits["y"], actuator, 1)
        add_revolute_joint(robot, f"{name}_z", y_link, child, (0,0,0), (0,0,0), (0,0,1), limits["z"], actuator, 2)
    return ET.ElementTree(robot)


def write_json(path: Path, data: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def write_urdf(path: Path, tree: ET.ElementTree):
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.indent(tree, space="  ")
    tree.write(path, encoding="utf-8", xml_declaration=True)


def urdf_text(tree: ET.ElementTree) -> str:
    ET.indent(tree, space="  ")
    buffer = io.BytesIO()
    tree.write(buffer, encoding="utf-8", xml_declaration=True)
    return buffer.getvalue().decode("utf-8")


def main(argv=None) -> int:
    exp = repo_root() / "experiments" / "locomotion_19body"
    parser = argparse.ArgumentParser(description="Build Asterra Stage-0A articulation contract/URDF")
    parser.add_argument("--contract", type=Path, default=exp / "config" / "physics_contract_19body.json")
    parser.add_argument("--policy", type=Path, default=exp / "config" / "modular_policy.json")
    parser.add_argument("--manifest", type=Path, default=exp / "generated" / "articulation_manifest.json")
    parser.add_argument("--urdf", type=Path, default=exp / "assets" / "asterra_19body_training.urdf")
    parser.add_argument("--manifest-only", action="store_true")
    parser.add_argument("--check", action="store_true", help="Fail if generated outputs are missing or stale")
    args = parser.parse_args(argv)

    contract, policy = load_json(args.contract), load_json(args.policy)
    manifest = build_manifest(contract, policy)
    manifest_text = json.dumps(manifest, indent=2) + "\n"
    tree = None if args.manifest_only else build_urdf(contract)

    if args.check:
        stale = []
        if not args.manifest.exists() or args.manifest.read_text(encoding="utf-8") != manifest_text:
            stale.append(str(args.manifest))
        if tree is not None and (not args.urdf.exists() or args.urdf.read_text(encoding="utf-8") != urdf_text(tree)):
            stale.append(str(args.urdf))
        if stale:
            print("STALE:\n  " + "\n  ".join(stale), file=sys.stderr)
            return 2
    else:
        write_json(args.manifest, manifest)
        if tree is not None:
            write_urdf(args.urdf, tree)

    print(f"Asterra articulation: 19 physical bodies, 18 anatomical joints, {manifest['actuated_dof_count']} revolute DOFs, {manifest['total_physical_mass_kg']:.3f} kg")
    print(f"contract sha256: {manifest['source_contract_sha256']}")
    if not args.check:
        print(f"manifest: {args.manifest}")
        if tree is not None:
            print(f"urdf: {args.urdf}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
