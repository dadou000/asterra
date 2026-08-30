extends RigidBody3D
## Small runtime helper used by the policy-enabled ragdoll feet.
##
## Contact impulse is converted to an approximate per-physics-tick contact force so
## the Godot policy observation can match Isaac Lab's left/right foot load feature.

var contact_force_n: float = 0.0
var contact_count: int = 0


func _integrate_forces(state: PhysicsDirectBodyState3D) -> void:
	contact_count = state.get_contact_count()
	var impulse_sum := 0.0
	for index: int in range(contact_count):
		impulse_sum += state.get_contact_impulse(index).length()
	contact_force_n = impulse_sum / maxf(state.step, 0.000001)
