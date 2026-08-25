class_name GPUPlanetContext
extends Node
## Immutable coarse planet context uploaded once per adopted world.
##
## This node intentionally does no camera-centred work and has no per-frame terrain
## synthesis. The CPU's only responsibility here is to convert the already-baked
## PlanetFields arrays into six-face GPU textures. All sub-grid terrain generation,
## material classification and scatter are expected to happen in shaders/compute.

const ROCK_MAX_ID := 12.0
const SOIL_DEPTH_MAX_M := 14.0
const SEDIMENT_MAX_M := 160.0
const ERodibility_MAX := 1.85
const STRATA_DIP_MAX := 1.2
const TEMP_MIN_C := -40.0
const TEMP_MAX_C := 50.0
const PRECIP_MAX_MM := 3000.0
const TEMP_RANGE_MAX_C := 55.0
const DISCHARGE_LOG_MAX := log(10001.0)

var ready_state := false
var face_res := 0

var soil_texture: Texture2DArray
var surface_texture: Texture2DArray
var geology_texture: Texture2DArray
var structure_texture: Texture2DArray
var climate_texture: Texture2DArray
var hydrology_texture: Texture2DArray

func _ready() -> void:
	if not Planet.world_ready.is_connected(_on_world_ready):
		Planet.world_ready.connect(_on_world_ready)
	if Planet.ready_state and Planet.fields != null:
		build_from_fields(Planet.fields)

func _on_world_ready(fields: PlanetFields) -> void:
	build_from_fields(fields)

func build_from_fields(fields: PlanetFields) -> void:
	ready_state = false
	face_res = fields.grid.res
	soil_texture = _build_texture(fields, 0)
	surface_texture = _build_texture(fields, 1)
	geology_texture = _build_texture(fields, 2)
	structure_texture = _build_texture(fields, 3)
	climate_texture = _build_texture(fields, 4)
	hydrology_texture = _build_texture(fields, 5)
	ready_state = soil_texture != null \
		and surface_texture != null \
		and geology_texture != null \
		and structure_texture != null \
		and climate_texture != null \
		and hydrology_texture != null

func _build_texture(fields: PlanetFields, kind: int) -> Texture2DArray:
	var grid := fields.grid
	var res: int = grid.res
	var tex_res := res + 2
	var step := 2.0 / float(res)
	var images: Array[Image] = []

	for face in 6:
		var image := Image.create(tex_res, tex_res, false, Image.FORMAT_RGBA8)
		for y in tex_res:
			var j := y - 1
			var v := (float(j) + 0.5) * step - 1.0
			for x in tex_res:
				var i := x - 1
				var c: int
				if i >= 0 and i < res and j >= 0 and j < res:
					c = grid.idx(face, i, j)
				else:
					var u := (float(i) + 0.5) * step - 1.0
					c = grid.dir_to_index(CubeSphere.face_uv_to_dir(face, u, v))
				image.set_pixel(x, y, _pack_cell(fields, c, kind))
		images.append(image)

	var texture := Texture2DArray.new()
	if texture.create_from_images(images) != OK:
		return null
	return texture

func _pack_cell(fields: PlanetFields, c: int, kind: int) -> Color:
	match kind:
		0:
			# Soil composition remains a continuous mixture.
			return Color(
				clampf(fields.soil_sand[c], 0.0, 1.0),
				clampf(fields.soil_silt[c], 0.0, 1.0),
				clampf(fields.soil_clay[c], 0.0, 1.0),
				clampf(fields.soil_organic[c], 0.0, 1.0))
		1:
			return Color(
				clampf(fields.soil_depth[c] / SOIL_DEPTH_MAX_M, 0.0, 1.0),
				clampf(fields.soil_moisture[c], 0.0, 1.0),
				clampf(fields.vegetation[c], 0.0, 1.0),
				clampf(fields.sediment[c] / SEDIMENT_MAX_M, 0.0, 1.0))
		2:
			return Color(
				clampf(float(fields.rock[c]) / ROCK_MAX_ID, 0.0, 1.0),
				clampf(fields.erodibility[c] / ERodibility_MAX, 0.0, 1.0),
				clampf(fields.strata_dip[c] / STRATA_DIP_MAX, 0.0, 1.0),
				clampf(fields.basin[c], 0.0, 1.0))
		3:
			var uplift_q := clampf(fields.uplift[c] / maxf(fields.cfg.max_uplift, 1.0) * 0.5 + 0.5, 0.0, 1.0)
			return Color(
				uplift_q,
				clampf(fields.fault[c], 0.0, 1.0),
				clampf(fields.floodplain[c], 0.0, 1.0),
				clampf(fields.wetland[c], 0.0, 1.0))
		4:
			return Color(
				clampf((fields.temp_mean[c] - TEMP_MIN_C) / (TEMP_MAX_C - TEMP_MIN_C), 0.0, 1.0),
				clampf(fields.precip[c] / PRECIP_MAX_MM, 0.0, 1.0),
				clampf(fields.humidity[c], 0.0, 1.0),
				clampf(fields.temp_range[c] / TEMP_RANGE_MAX_C, 0.0, 1.0))
		5:
			var flow_index := int(fields.flow_dir[c])
			var flow := Vector3.ZERO
			if flow_index >= 0 and flow_index < 8:
				var d := fields.grid.cell_dir(c)
				var target := fields.grid.cell_dir(fields.grid.nbr[c * 8 + flow_index])
				flow = target - d
				flow -= d * flow.dot(d)
				if flow.length_squared() > 1e-12:
					flow = flow.normalized()
			var basis := CubeSphere.tangent_basis(fields.grid.cell_dir(c))
			var fx := clampf(flow.dot(basis[0]) * 0.5 + 0.5, 0.0, 1.0)
			var fy := clampf(flow.dot(basis[1]) * 0.5 + 0.5, 0.0, 1.0)
			var q := clampf(log(1.0 + maxf(fields.discharge[c], 0.0)) / DISCHARGE_LOG_MAX, 0.0, 1.0)
			var depositional := clampf(fields.floodplain[c] * 0.72 + fields.wetland[c] * 0.38, 0.0, 1.0)
			return Color(fx, fy, q, depositional)
	return Color(0.0, 0.0, 0.0, 0.0)
