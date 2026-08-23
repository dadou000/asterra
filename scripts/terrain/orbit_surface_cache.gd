class_name OrbitSurfaceCache
extends RefCounted
## Builds the graphics-only elevation texture used by distant terrain/ocean.
##
## The expensive part of this cache is intentionally split in two:
## `build_images()` is CPU-only and safe to run on a background thread, while
## `create_texture()` performs the small RenderingServer upload on the main
## thread. The old synchronous path evaluated millions of GDScript samples in a
## single frame and caused multi-second freezes.
##
## The previous orbit mask was sampled straight from the 192-cell macro field.
## That is not the same surface the player sees: TerrainDetail can move the
## shoreline by hundreds of metres vertically, which is enough to create or erase
## islands. This cache therefore samples the *pristine runtime terrain* in a
## narrow band around sea level, while leaving unquestionably inland/ocean cells
## on the cheap macro path.

const UPSAMPLE := 4
const MAX_FACE_RES := 768
const COAST_DETAIL_BAND_M := 600.0

## CPU-only stage. Returns Image resources but does not create a GPU texture.
static func build_images() -> Dictionary:
	if not Planet.ready_state or Planet.grid == null:
		return {}

	var res: int = mini(Planet.grid.res * UPSAMPLE, MAX_FACE_RES)
	var tex_res: int = res + 2
	var started := Time.get_ticks_msec()

	# Sampling the pristine surface costs roughly six times what reading the macro
	# field did, so the six faces are built concurrently. They share nothing
	# writable: each owns its Image and its own TerrainDetail, which is why that
	# class documents one instance per thread.
	var faces: Array[Image] = []
	faces.resize(6)
	var build_face := func(face: int) -> void:
		faces[face] = _build_face(face, res, tex_res)
	var group := WorkerThreadPool.add_group_task(build_face, 6, -1, false,
		"asterra_orbit_face")
	WorkerThreadPool.wait_for_group_task_completion(group)

	var images: Array[Image] = []
	for face in 6:
		if faces[face] == null:
			return {}
		images.append(faces[face])

	print("orbit surface: %d x %d x 6 in %.1f s"
		% [tex_res, tex_res, (Time.get_ticks_msec() - started) / 1000.0])
	return {
		"images": images,
		"face_res": res,
	}

static func _build_face(face: int, res: int, tex_res: int) -> Image:
	var cell_step: float = 2.0 / float(res)
	var detail: TerrainDetail = Planet.make_detail()
	var img := Image.create(tex_res, tex_res, false, Image.FORMAT_RF)
	for y in tex_res:
		var j: int = y - 1
		var v: float = (float(j) + 0.5) * cell_step - 1.0
		for x in tex_res:
			var i: int = x - 1
			var u: float = (float(i) + 0.5) * cell_step - 1.0
			var d: Vector3 = CubeSphere.face_uv_to_dir(face, u, v)
			var macro_h: float = Planet.macro_height(d)
			var h: float = macro_h + Planet.coast_profile_offset(d, macro_h)
			# Land carries the real runtime surface, not the macro field.
			# Bilinear interpolation of 8 km cells is smooth by construction, so a
			# texture built from it describes a planet with no mountains on it --
			# and this texture is what distant relief shading, cast shadows and sky
			# occlusion all read. Sampling the pristine surface here is what lets a
			# range shade like a range from orbit. Deep water keeps the cheap path:
			# nothing reads its shape except the depth gradient, and it is more
			# than half the sphere.
			if macro_h > -COAST_DETAIL_BAND_M:
				h = Planet.pristine_height(d, detail)
			img.set_pixel(x, y, Color(h, 0.0, 0.0, 1.0))
	return img

## Main-thread stage. Uploading ~14 MB is cheap compared with the CPU synthesis
## above, and keeping it here avoids touching RenderingServer resources from the
## worker thread.
static func create_texture(built: Dictionary) -> Texture2DArray:
	if built.is_empty() or not built.has("images"):
		return null
	var images: Array[Image] = []
	for value in built["images"]:
		var image: Image = value
		images.append(image)
	var texture_array := Texture2DArray.new()
	var err := texture_array.create_from_images(images)
	if err != OK:
		push_error("Failed to upload detailed orbit elevation texture (%d)" % err)
		return null
	return texture_array
