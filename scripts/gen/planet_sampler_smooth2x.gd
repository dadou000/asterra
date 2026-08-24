extends "res://scripts/gen/planet_sampler.gd"
## Smooth macro sampler for the fully procedural GPU terrain path.
##
## The authoritative macro grid remains unchanged. Terrain elevation is
## reconstructed with a seam-safe cubic B-spline before being sampled onto a 2x
## visual texture. The texture also carries mipmaps so coarse clipmap levels can
## sample a prefiltered macro surface instead of aliasing a ~4 km texel lattice.

const VISUAL_MACRO_UPSAMPLE: int = 2
# The coarse visual levels use up to roughly mip 3 of the 2x macro texture. A
# generous gutter keeps those filtered footprints on the same continuous
# direction-space spline when they approach a cube-face boundary.
const VISUAL_MACRO_GUTTER: int = 8


## CPU physics/edit queries use the exact C2 macro reconstruction. The visual
## texture is only a sampled/prefiltered representation of this same function.
func macro_height(d: Vector3) -> float:
	return grid.sample_cubic_bspline(_macro_elev, d)


func _build_orbit_textures() -> void:
	orbit_elevation_texture = null
	var source_res: int = grid.res
	var visual_res: int = source_res * VISUAL_MACRO_UPSAMPLE
	orbit_texture_face_res = visual_res
	var gutter: int = VISUAL_MACRO_GUTTER
	var tex_res: int = visual_res + gutter * 2
	var cell_step: float = 2.0 / float(visual_res)
	var images: Array[Image] = []

	for face: int in 6:
		var img := Image.create(tex_res, tex_res, false, Image.FORMAT_RF)
		for y: int in tex_res:
			var j: int = y - gutter
			var v: float = (float(j) + 0.5) * cell_step - 1.0
			for x: int in tex_res:
				var i: int = x - gutter
				var u: float = (float(i) + 0.5) * cell_step - 1.0
				# Direction-space reconstruction fills both the interior and the wide
				# cross-face gutter from the same continuous spherical spline.
				var d: Vector3 = CubeSphere.face_uv_to_dir(face, u, v)
				var h: float = grid.sample_cubic_bspline(_macro_elev, d)
				img.set_pixel(x, y, Color(h, 0.0, 0.0, 1.0))

		# Generate the prefiltered hierarchy once. Runtime clipmap levels select a
		# mip from their vertex spacing, preventing coarse rings from aliasing the
		# macro elevation lattice into kilometre-scale plates.
		var mip_err: Error = img.generate_mipmaps()
		if mip_err != OK:
			push_error("Failed to generate macro elevation mipmaps (%d)" % mip_err)
		images.append(img)

	var texture_array := Texture2DArray.new()
	var err: Error = texture_array.create_from_images(images)
	if err != OK:
		push_error("Failed to build mipmapped 2x cubic macro elevation texture array (%d)" % err)
		return
	orbit_elevation_texture = texture_array
