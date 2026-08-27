extends "res://scripts/terrain/terrain_deformation_physics.gd"
## Contact-response correction layer.
##
## The base solver computes the terrain's bearing/support capacity. Capacity is not
## itself an upward force source: the actual reaction cannot exceed the load the
## contacting object is asking the terrain to carry. Capping the reaction here
## prevents penalty-spring overshoot and gives future rigid-body contacts the same
## non-pogo behavior as the deformation experiment.


func apply_contact(center_dir: Vector3, contact_radius_m: float, normal_load_n: float,
		penetration_m: float, normal_speed_mps: float, tangential_speed_mps: float,
		dt: float, cutting: float = 0.0, material_id: int = MATERIAL_TOPSOIL,
		footprint_type: int = FOOTPRINT_GENERIC, shape_radius_m: float = 0.0) -> Dictionary:
	var result: Dictionary = super.apply_contact(
		center_dir, contact_radius_m, normal_load_n, penetration_m,
		normal_speed_mps, tangential_speed_mps, dt, cutting, material_id,
		footprint_type, shape_radius_m)
	var capacity_force_n: float = maxf(float(result.get("support_force_n", 0.0)), 0.0)
	var demanded_force_n: float = maxf(normal_load_n, 0.0)
	var reaction_force_n: float = minf(capacity_force_n, demanded_force_n)
	result["support_capacity_force_n"] = capacity_force_n
	result["support_force_n"] = reaction_force_n
	return result
