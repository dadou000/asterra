class_name PlanetTerrain
extends Node3D
## Lightweight terrain interface retained for the Phase-1 harness and debug UI.
##
## The former implementation lived here: a runtime cube-sphere quadtree that
## launched ChunkBuilder jobs, generated unique vertex/normal/index arrays, and
## uploaded replacement meshes while moving. That visual pipeline has been
## removed. SparsePlanetTerrain is now the only visual terrain implementation.
##
## This class intentionally contains no visual terrain generation code. It keeps
## only the small API/type surface still consumed by the harness, HUD and debug
## tools while those callers are migrated to renderer-agnostic interfaces.

enum Band { ORBITAL, REGIONAL, LOCAL, EDITABLE }
const BAND_MIN_DEPTH := [0, 5, 10, 14]
const MAX_TERRAIN_HEIGHT := 6800.0

enum FaceEdge { LEFT, RIGHT, BOTTOM, TOP }
const STITCH_LEFT := 1 << FaceEdge.LEFT
const STITCH_RIGHT := 1 << FaceEdge.RIGHT
const STITCH_BOTTOM := 1 << FaceEdge.BOTTOM
const STITCH_TOP := 1 << FaceEdge.TOP

## Kept only for old debug/test code that names PlanetTerrain.QuadNode. The sparse
## renderer does not allocate these nodes at runtime.
class QuadNode extends RefCounted:
	var face: int = 0
	var depth: int = 0
	var u0: float = -1.0
	var v0: float = -1.0
	var size: float = 2.0
	var center_dir := Vector3(1.0, 0.0, 0.0)
	var center_world := Vec3D.new(0.0, 0.0, 0.0)
	var arc: float = 0.0
	var lod_centers: Array = []
	var children: Array = []
	var chunk: Node3D = null
	var ground_mi: MeshInstance3D = null
	var water_mi: MeshInstance3D = null
	var body: StaticBody3D = null
	var state: int = 0
	var task_id: int = -1
	var dirty: bool = false
	var abandoned: bool = false
	var revision: int = 0
	var requested_revision: int = -1
	var request_token: int = 0
	var stitch_mask: int = 0
	var built_stitch_mask: int = -1
	var geometric_error: float = 1.0
	var fine_vis: float = 0.0
	var drawn: bool = false
	var morph: float = 0.0
	var parent_ref: WeakRef = null

	func parent() -> QuadNode:
		return parent_ref.get_ref() if parent_ref != null else null

	func is_leaf() -> bool:
		return children.is_empty()

var cfg: GenConfig
var observer: Vec3D = Vec3D.new(0.0, 0.0, 0.0)
var forced_depth: int = -1
var roots: Array = []

var _ground_mat: ShaderMaterial
var _water_mat: ShaderMaterial
var _stats: Dictionary = {
	"chunks": 0,
	"nodes": 0,
	"queued": 0,
	"in_flight": 0,
	"culled": 0,
	"handoffs": 0,
	"horizon_deg": 0.0,
	"horizon_km": 0.0,
}
var _obs_dir := Vector3(1.0, 0.0, 0.0)
var _horizon_angle: float = 0.0


func build_roots() -> void:
	cfg = Planet.cfg
	roots.clear()


func set_observer(world_pos: Vec3D) -> void:
	observer = world_pos


func debug_materials() -> Array:
	var result: Array = []
	if _ground_mat != null:
		result.append(_ground_mat)
	if _water_mat != null:
		result.append(_water_mat)
	return result


func stats() -> Dictionary:
	return _stats.duplicate()


static func band_for_depth(depth: int) -> int:
	if depth >= BAND_MIN_DEPTH[Band.EDITABLE]:
		return Band.EDITABLE
	if depth >= BAND_MIN_DEPTH[Band.LOCAL]:
		return Band.LOCAL
	if depth >= BAND_MIN_DEPTH[Band.REGIONAL]:
		return Band.REGIONAL
	return Band.ORBITAL
