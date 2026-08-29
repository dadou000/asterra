extends Node
## Parser/runtime stub for isolated CI. The production project uses
## planet_sampler_gpu_runtime.gd for the Planet autoload.
##
## configure_for_sculpt_ci() and pristine_height() provide a tiny deterministic
## generated surface for numerical Planet Studio sculpt tests. They are used only
## by the isolated CI project and deliberately do not emulate the production GPU
## terrain query path.

signal world_ready

class CIPlanetConfig extends Resource:
	var planet_radius: float = 1000000.0

var ready_state: bool = false
var cfg: Resource
var fields: Resource
var orbit_elevation_texture: Texture2DArray
var orbit_texture_face_res: int = 0

func configure_for_sculpt_ci(radius_m: float = 1000000.0) -> void:
	var next := CIPlanetConfig.new()
	next.planet_radius = maxf(radius_m, 1.0)
	cfg = next
	ready_state = true

func pristine_height(direction: Vector3, _detail: Variant = null) -> float:
	if direction.length_squared() <= 1e-12:
		return 0.0
	var d := direction.normalized()
	# Non-flat on purpose: a correct flatten brush must compensate this generated
	# slope with different sparse delta values instead of equalizing the deltas.
	return 12.0 + d.y * 8.0 + d.z * 3.0
