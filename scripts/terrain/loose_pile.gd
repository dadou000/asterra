class_name LoosePile
extends Node3D
## A physical heap of excavated material sitting on the ground.
##
## 1.7: excavated terrain becomes loose material that has to be transported and
## redeposited, not deleted. A pile knows its material and its loose volume, and
## its shape follows the material's angle of repose -- clay stands steeper than
## sand, so the same volume makes a different heap.

var stock: MaterialStock
var world_pos: Vec3D
var surface_dir: Vector3

var _mesh: MeshInstance3D

func setup(p_stock: MaterialStock, p_world: Vec3D, p_dir: Vector3) -> void:
	stock = p_stock
	world_pos = p_world
	surface_dir = p_dir
	_rebuild()

func _ready() -> void:
	Frames.origin_shifted.connect(func(_d): position = Frames.to_render(world_pos))
	position = Frames.to_render(world_pos)

func volume() -> float:
	return stock.total_volume()

func _rebuild() -> void:
	if _mesh == null:
		_mesh = MeshInstance3D.new()
		add_child(_mesh)
	var dom := stock.dominant()
	var repose: float = 34.0
	var col := Color(0.42, 0.34, 0.26)
	if not dom.is_empty():
		repose = float(dom["props"].get("repose", 34.0))
		col = _color_for(dom["material_id"], dom["rock_family"])
	# Cone of revolution: V = pi/3 * r^2 * h, with h = r * tan(repose).
	var v := maxf(volume(), 0.01)
	var t := tan(deg_to_rad(clampf(repose, 12.0, 55.0)))
	var r := pow(3.0 * v / (PI * t), 1.0 / 3.0)
	var h := r * t
	var cyl := CylinderMesh.new()
	cyl.top_radius = 0.02
	cyl.bottom_radius = r
	cyl.height = h
	cyl.radial_segments = 14
	var mat := StandardMaterial3D.new()
	mat.albedo_color = col
	mat.roughness = 1.0
	cyl.material = mat
	_mesh.mesh = cyl
	_mesh.position = Vector3(0, h * 0.5, 0)
	# Stand the cone up along the local vertical.
	var up := surface_dir
	var basis_y := up
	var basis_x := Vector3(0, 1, 0).cross(basis_y)
	if basis_x.length() < 1e-4:
		basis_x = Vector3(1, 0, 0)
	basis_x = basis_x.normalized()
	var basis_z := basis_x.cross(basis_y).normalized()
	transform.basis = Basis(basis_x, basis_y, basis_z)

func add_from(other: MaterialStock) -> void:
	for k in other.entries:
		var e: Dictionary = other.entries[k]
		stock.add(e["material_id"], e["rock_family"], e["volume_loose"], e["props"])
	_rebuild()

func take_all() -> MaterialStock:
	var s := stock
	stock = MaterialStock.new()
	queue_free()
	return s

static func _color_for(material_id: int, rock_family: int) -> Color:
	match material_id:
		MaterialDB.SOIL_O: return Color(0.20, 0.15, 0.10)
		MaterialDB.SOIL_A: return Color(0.31, 0.23, 0.15)
		MaterialDB.SOIL_B: return Color(0.46, 0.34, 0.21)
		MaterialDB.SOIL_C: return Color(0.55, 0.47, 0.36)
		MaterialDB.SAND: return Color(0.78, 0.70, 0.50)
		MaterialDB.GRAVEL: return Color(0.55, 0.53, 0.50)
		MaterialDB.CLAY: return Color(0.52, 0.40, 0.32)
	var e := clampf(PlanetFields.ROCK_ERODIBILITY[maxi(0, rock_family)] / 1.9, 0.0, 1.0)
	return Color(0.40, 0.39, 0.38).lerp(Color(0.62, 0.60, 0.56), e)

func serialize() -> Dictionary:
	return {"x": world_pos.x, "y": world_pos.y, "z": world_pos.z, "stock": stock.serialize()}
