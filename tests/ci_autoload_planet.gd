extends Node
## Parser/runtime stub for isolated CI. The production project uses
## planet_sampler_gpu_runtime.gd for the Planet autoload.

signal world_ready

var ready_state: bool = false
var cfg: Resource
var fields: Resource
var orbit_elevation_texture: Texture2DArray
var orbit_texture_face_res: int = 0
