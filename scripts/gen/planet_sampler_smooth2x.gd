extends "res://scripts/gen/planet_sampler.gd"
## Smooth macro sampler for the fully procedural GPU terrain path.
##
## The authoritative macro grid is intentionally low frequency. Before it becomes
## the base for GPU terrain composition we apply several extra seam-safe Gaussian
## relaxations, reconstruct that field with the C2 cubic B-spline, then supersample
## the spline to a dense 4x visual texture. This keeps the original world-scale
## landforms while removing the coarse macro-cell shoulders/facets that were still
## visible beneath procedural detail.

# The expensive direction-space spline is evaluated only at 2x. Godot then
# supersamples that already-smooth image to 4x in native code. This gives the GPU
# a ~2 km macro texel on a 1000 km planet without doing ~60 million GDScript spline
# taps during startup.
const SPLINE_SAMPLE_UPSAMPLE: int = 2
const VISUAL_MACRO_UPSAMPLE: int = 4
const SPLINE_GUTTER: int = 8
const VISUAL_MACRO_GUTTER: int = 16

# The base sampler already performs two 50% Gaussian relaxation passes. These
# additional passes heavily suppress cell-scale energy while retaining broad
# mountain/continental structure. Repeated 3x3 Gaussian relaxation is preferable
# to one huge kernel because the neighbour graph crosses cube faces correctly.
const EXTRA_MACRO_SMOOTH_PASSES: int = 4
const EXTRA_MACRO_SMOOTH_STRENGTH: float = 0.72


func _build_smoothed_macro_elevation() -> void:
	# Clamp + the original two mild passes first.
	super._build_smoothed_macro_elevation()

	for _pass: int in EXTRA_MACRO_SMOOTH_PASSES:
		var src: PackedFloat32Array = _macro_elev
		var dst := PackedFloat32Array()
		dst.resize(grid.cell_count)
		for c: int in grid.cell_count:
			var base: int = c * 8
			# Seam-safe separable 3x3 Gaussian on the spherical neighbour graph:
			#   1 2 1
			#   2 4 2   / 16
			#   1 2 1
			var blurred: float = src[c] * 4.0
			blurred += src[grid.nbr[base + 0]] * 2.0
			blurred += src[grid.nbr[base + 2]] * 2.0
			blurred += src[grid.nbr[base + 4]] * 2.0
			blurred += src[grid.nbr[base + 6]] * 2.0
			blurred += src[grid.nbr[base + 1]]
			blurred += src[grid.nbr[base + 3]]
			blurred += src[grid.nbr[base + 5]]
			blurred += src[grid.nbr[base + 7]]
			blurred *= 1.0 / 16.0
			dst[c] = lerpf(src[c], blurred, EXTRA_MACRO_SMOOTH_STRENGTH)
		_macro_elev = dst

	# Relief-dependent systems must see the same stronger runtime macro field.
	_macro_relief = PackedFloat32Array()
	_macro_relief.resize(grid.cell_count)
	for c: int in grid.cell_count:
		var h: float = _macro_elev[c]
		var relief := 0.0
		for k: int in 8:
			relief = maxf(relief, absf(h - _macro_elev[grid.nbr[c * 8 + k]]))
		_macro_relief[c] = relief


## CPU physics/edit queries use the exact C2 macro reconstruction. The visual
## texture is a dense sampled/prefiltered representation of this same function.
func macro_height(d: Vector3) -> float:
	return grid.sample_cubic_bspline(_macro_elev, d)


func _build_orbit_textures() -> void:
	orbit_elevation_texture = null
	var source_res: int = grid.res
	var spline_res: int = source_res * SPLINE_SAMPLE_UPSAMPLE
	var visual_res: int = source_res * VISUAL_MACRO_UPSAMPLE
	orbit_texture_face_res = visual_res

	var spline_tex_res: int = spline_res + SPLINE_GUTTER * 2
	var visual_tex_res: int = visual_res + VISUAL_MACRO_GUTTER * 2
	var spline_step: float = 2.0 / float(spline_res)
	var images: Array[Image] = []

	for face: int in 6:
		# First evaluate the real spherical C2 field at 2x. This is the expensive
		# operation, so keep it at the resolution that already proved practical.
		var img := Image.create(spline_tex_res, spline_tex_res, false, Image.FORMAT_RF)
		for y: int in spline_tex_res:
			var j: int = y - SPLINE_GUTTER
			var v: float = (float(j) + 0.5) * spline_step - 1.0
			for x: int in spline_tex_res:
				var i: int = x - SPLINE_GUTTER
				var u: float = (float(i) + 0.5) * spline_step - 1.0
				var d: Vector3 = CubeSphere.face_uv_to_dir(face, u, v)
				var h: float = grid.sample_cubic_bspline(_macro_elev, d)
				img.set_pixel(x, y, Color(h, 0.0, 0.0, 1.0))

		# Native Lanczos supersampling makes the already-C2 spline representation
		# much denser for near/regional clipmap vertices. Scaling the whole image
		# also scales the 8-texel spline gutter to the required 16-texel final gutter.
		img.resize(visual_tex_res, visual_tex_res, Image.INTERPOLATE_LANCZOS)

		# Coarse clipmap levels select fractional mips according to vertex spacing,
		# so they see a true low-pass version rather than undersampling the macro.
		var mip_err: Error = img.generate_mipmaps()
		if mip_err != OK:
			push_error("Failed to generate macro elevation mipmaps (%d)" % mip_err)
		images.append(img)

	var texture_array := Texture2DArray.new()
	var err: Error = texture_array.create_from_images(images)
	if err != OK:
		push_error("Failed to build 4x smoothed macro elevation texture array (%d)" % err)
		return
	orbit_elevation_texture = texture_array
