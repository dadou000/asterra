class_name GPUPlanetContext
extends Node
## Immutable coarse planet context uploaded once per adopted world.
##
## This node intentionally does no camera-centred work and has no per-frame terrain
## synthesis. The CPU only converts the already-baked PlanetFields arrays into
## six-face GPU textures when a world is adopted. All sub-grid terrain generation,
## material classification and scatter happen on the GPU.

const ROCK_MAX_ID := 12.0
const BIOME_MAX_ID := 19.0
const SOIL_DEPTH_MAX_M := 14.0
const SEDIMENT_MAX_M := 160.0
const ERODIBILITY_MAX := 1.85
const STRATA_DIP_MAX := 1.2
const TEMP_MIN_C := -40.0
const TEMP_MAX_C := 50.0
const PRECIP_MAX_MM := 3000.0
const TEMP_RANGE_MAX_C := 55.0
const DISCHARGE_LOG_MAX := 9.210440366976517

var ready_state := false
var face_res := 0
var generation := 0

var soil_texture: Texture2DArray
var surface_texture: Texture2DArray
var geology_texture: Texture2DArray
var structure_texture: Texture2DArray
var climate_texture: Texture2DArray
var hydrology_texture: Texture2DArray
var rock_texture: Texture2DArray
var biome_texture: Texture2DArray

func _ready() -> void:
	if not Planet.world_ready.is_connected(_on_world_ready):
		Planet.world_ready.connect(_on_world_ready)
	if Planet.ready_state and Planet.fields != null:
		build_from_fields(Planet.fields)

func _on_world_ready(fields: PlanetFields) -> void:
	build_from_fields(fields)

func build_from_fields(fields: PlanetFields) -> void:
	ready_state = false
	generation += 1
	face_res = fields.grid.res
	soil_texture = _build_texture(fields, 0, Image.FORMAT_RGBA8)
	surface_texture = _build_texture(fields, 1, Image.FORMAT_RGBA8)
	geology_texture = _build_texture(fields, 2, Image.FORMAT_RGBA8)
	structure_texture = _build_texture(fields, 3, Image.FORMAT_RGBA8)
	climate_texture = _build_texture(fields, 4, Image.FORMAT_RGBA8)
	hydrology_texture = _build_texture(fields, 5, Image.FORMAT_RGBA8)
	rock_texture = _build_texture(fields, 6, Image.FORMAT_R8)
	biome_texture = _build_texture(fields, 7, Image.FORMAT_R8)
	ready_state = soil_texture != null \
		and surface_texture != null \
		and geology_texture != null \
		and structure_texture != null \
		and climate_texture != null \
		and hydrology_texture != null \
		and rock_texture != null \
		and biome_texture != null

func _build_texture(fields: PlanetFields, kind: int, format: Image.Format) -> Texture2DArray:
	var grid := fields.grid
	var res: int = grid.res
	var tex_res := res + 2
	var step := 2.0 / float(res)
	var images: Array[Image] = []

	for face in 6:
		var image := Image.create(tex_res, tex_res, false, format)
		for y in tex_res:
			var j := y - 1
			var v := (float(j) + 0.5) * step - 1.0
			for x in tex_res:
				var i := x - 1
				if i >= 0 and i < res and j >= 0 and j < res:
					image.set_pixel(x, y, _pack_cell(fields, grid.idx(face, i, j), kind))
				else:
					# Continuous gutters are sampled in direction space, exactly like the
					# elevation texture. This lets hardware filtering cross cube faces
					# without seeing a nearest-cell step at the seam.
					var u := (float(i) + 0.5) * step - 1.0
					var d := CubeSphere.face_uv_to_dir(face, u, v)
					image.set_pixel(x, y, _pack_direction(fields, d, kind))
		images.append(image)

	var texture := Texture2DArray.new()
	if texture.create_from_images(images) != OK:
		return null
	return texture

func _pack_direction(fields: PlanetFields, d: Vector3, kind: int) -> Color:
	var grid := fields.grid
	# Categorical maps intentionally remain nearest-neighbour.
	if kind >= 6:
		return _pack_cell(fields, grid.dir_to_index(d), kind)

	match kind:
		0:
			return Color(
				clampf(grid.sample_bilinear(fields.soil_sand, d), 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.soil_silt, d), 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.soil_clay, d), 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.soil_organic, d), 0.0, 1.0))
		1:
			return Color(
				clampf(grid.sample_bilinear(fields.soil_depth, d) / SOIL_DEPTH_MAX_M, 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.soil_moisture, d), 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.vegetation, d), 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.sediment, d) / SEDIMENT_MAX_M, 0.0, 1.0))
		2:
			return Color(
				clampf(grid.sample_bilinear(fields.erodibility, d) / ERODIBILITY_MAX, 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.strata_dip, d) / STRATA_DIP_MAX, 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.basin, d), 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.plate_boundary, d), 0.0, 1.0))
		3:
			var uplift_q := clampf(
				grid.sample_bilinear(fields.uplift, d) / maxf(fields.cfg.max_uplift, 1.0) * 0.5 + 0.5,
				0.0, 1.0)
			return Color(
				uplift_q,
				clampf(grid.sample_bilinear(fields.fault, d), 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.floodplain, d), 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.wetland, d), 0.0, 1.0))
		4:
			return Color(
				clampf((grid.sample_bilinear(fields.temp_mean, d) - TEMP_MIN_C)
					/ (TEMP_MAX_C - TEMP_MIN_C), 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.precip, d) / PRECIP_MAX_MM, 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.humidity, d), 0.0, 1.0),
				clampf(grid.sample_bilinear(fields.temp_range, d) / TEMP_RANGE_MAX_C, 0.0, 1.0))
		5:
			# Flow direction is discrete at this coarse scale, while discharge and
			# deposition are continuous. Re-express the selected flow vector in the
			# gutter direction's tangent basis so the encoded RG frame stays valid.
			var c := grid.dir_to_index(d)
			var flow_index := int(fields.flow_dir[c])
			var flow := Vector3.ZERO
			if flow_index >= 0 and flow_index < 8:
				var source := grid.cell_dir(c)
				var target := grid.cell_dir(grid.nbr[c * 8 + flow_index])
				flow = target - source
				flow -= d * flow.dot(d)
				if flow.length_squared() > 1e-12:
					flow = flow.normalized()
			var basis := CubeSphere.tangent_basis(d)
			var fx := clampf(flow.dot(basis[0]) * 0.5 + 0.5, 0.0, 1.0)
			var fy := clampf(flow.dot(basis[1]) * 0.5 + 0.5, 0.0, 1.0)
			var q := clampf(log(1.0 + maxf(grid.sample_bilinear(fields.discharge, d), 0.0))
				/ DISCHARGE_LOG_MAX, 0.0, 1.0)
			var depositional := clampf(
				grid.sample_bilinear(fields.floodplain, d) * 0.72
				+ grid.sample_bilinear(fields.wetland, d) * 0.38,
				0.0, 1.0)
			return Color(fx, fy, q, depositional)
	return Color(0.0, 0.0, 0.0, 0.0)

func _pack_cell(fields: PlanetFields, c: int, kind: int) -> Color:
	match kind:
		0:
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
				clampf(fields.erodibility[c] / ERODIBILITY_MAX, 0.0, 1.0),
				clampf(fields.strata_dip[c] / STRATA_DIP_MAX, 0.0, 1.0),
				clampf(fields.basin[c], 0.0, 1.0),
				clampf(fields.plate_boundary[c], 0.0, 1.0))
		3:
			var uplift_q := clampf(
				fields.uplift[c] / maxf(fields.cfg.max_uplift, 1.0) * 0.5 + 0.5,
				0.0, 1.0)
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
			var d := fields.grid.cell_dir(c)
			if flow_index >= 0 and flow_index < 8:
				var target := fields.grid.cell_dir(fields.grid.nbr[c * 8 + flow_index])
				flow = target - d
				flow -= d * flow.dot(d)
				if flow.length_squared() > 1e-12:
					flow = flow.normalized()
			var basis := CubeSphere.tangent_basis(d)
			var fx := clampf(flow.dot(basis[0]) * 0.5 + 0.5, 0.0, 1.0)
			var fy := clampf(flow.dot(basis[1]) * 0.5 + 0.5, 0.0, 1.0)
			var q := clampf(
				log(1.0 + maxf(fields.discharge[c], 0.0)) / DISCHARGE_LOG_MAX,
				0.0, 1.0)
			var depositional := clampf(
				fields.floodplain[c] * 0.72 + fields.wetland[c] * 0.38,
				0.0, 1.0)
			return Color(fx, fy, q, depositional)
		6:
			return Color(clampf(float(fields.rock[c]) / ROCK_MAX_ID, 0.0, 1.0), 0.0, 0.0, 1.0)
		7:
			return Color(clampf(float(fields.biome[c]) / BIOME_MAX_ID, 0.0, 1.0), 0.0, 0.0, 1.0)
	return Color(0.0, 0.0, 0.0, 0.0)
