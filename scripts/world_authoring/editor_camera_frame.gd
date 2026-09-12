extends RefCounted
## Parallel-transport a tangent heading instead of switching reference axes at
## a latitude threshold. Heading remains continuous through either pole.
var up := Vector3.ZERO
var north := Vector3.ZERO

func tangent(new_up: Vector3, yaw: float) -> Array:
	new_up = new_up.normalized() if new_up.length_squared() > 1e-12 else Vector3.UP
	if up.length_squared() < 0.5:
		var reference := Vector3.UP if absf(new_up.y) < 0.99 else Vector3.FORWARD
		north = (reference - new_up * reference.dot(new_up)).normalized()
	else:
		var dot := clampf(up.dot(new_up), -1.0, 1.0)
		if dot < -0.999999:
			# An antipodal teleport has no unique shortest arc. Keep the previous
			# heading as the deterministic rotation axis.
			north = Quaternion(north, PI) * north
		elif dot < 0.9999999:
			north = Quaternion(up, new_up) * north
		north = (north - new_up * north.dot(new_up)).normalized()
	up = new_up
	var east := north.cross(up).normalized()
	var forward := (north * cos(yaw) + east * sin(yaw)).normalized()
	return [forward, forward.cross(up).normalized()]
