extends "res://scripts/gen/planet_sampler.gd"
## Graphics macro sampler for the fully procedural GPU terrain path.
##
## The authoritative macro grid remains unchanged. For rendering, its already
## smoothed elevation field is resampled to twice the face resolution with the
## CPU's spherical bilinear sampler, then the GPU linearly filters that texture
## again. This adds no new terrain information, but removes visible macro-cell
## stepping/pixelation before procedural detail is added at clipmap vertices.

const VISUAL_MACRO_UPSAMPLE: int = 2


func _build_orbit_textures() -> void:
	orbit_elevation_texture = null
	var source_res: int = grid.res
	var visual_res: int = source_res * VISUAL_MACRO_UPSAMPLE
	orbit_texture_face_res = visual_res
	var tex_res: int = visual_res + 2
	var cell_step: float = 2.0 / float(visual_res)
	var images: Array[Image] = []

	for face: int in 6:
		var img := Image.create(tex_res, tex_res, false, Image.FORMAT_RF)
		for y: int in tex_res:
			var j: int = y - 1
			var v: float = (float(j) + 0.5) * cell_step - 1.0
			for x: int in tex_res:
				var i: int = x - 1
				var u: float = (float(i) + 0.5) * cell_step - 1.0
				# Direction-space resampling handles both the 2x interior and the
				# one-texel cross-face gutter with the same continuous field.
				var d: Vector3 = CubeSphere.face_uv_to_dir(face, u, v)
				var h: float = grid.sample_bilinear(_macro_elev, d)
				img.set_pixel(x, y, Color(h, 0.0, 0.0, 1.0))
		images.append(img)

	var texture_array := Texture2DArray.new()
	var err: Error = texture_array.create_from_images(images)
	if err != OK:
		push_error("Failed to build 2x smoothed macro elevation texture array (%d)" % err)
		return
	orbit_elevation_texture = texture_array
