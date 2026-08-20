class_name OrbitSurfaceCache
extends RefCounted
## Builds the graphics-only elevation texture used by distant terrain/ocean.
##
## The previous orbit mask was sampled straight from the 192-cell macro field.
## That is not the same surface the player sees: TerrainDetail can move the
## shoreline by hundreds of metres vertically, which is enough to create or erase
## islands. This cache therefore samples the *pristine runtime terrain* in a
## narrow band around sea level, while leaving unquestionably inland/ocean cells
## on the cheap macro path.
##
## Four samples per macro cell edge gives a 768x768 face with the default world.
## The expensive FastNoiseLite detail path is only evaluated where macro height
## is close enough to sea level that detail could actually flip land/water.

const UPSAMPLE := 4
const MAX_FACE_RES := 768
const COAST_DETAIL_BAND_M := 600.0

static func build() -> Dictionary:
	if not Planet.ready_state or Planet.grid == null:
		return {}

	var res: int = mini(Planet.grid.res * UPSAMPLE, MAX_FACE_RES)
	var tex_res: int = res + 2
	var cell_step: float = 2.0 / float(res)
	var detail := Planet.make_detail()
	var images: Array[Image] = []

	for face in 6:
		var img := Image.create(tex_res, tex_res, false, Image.FORMAT_RF)
		for y in tex_res:
			var j: int = y - 1
			var v: float = (float(j) + 0.5) * cell_step - 1.0
			for x in tex_res:
				var i: int = x - 1
				var u: float = (float(i) + 0.5) * cell_step - 1.0
				var d := CubeSphere.face_uv_to_dir(face, u, v)
				var macro_h: float = Planet.macro_height(d)
				var h: float = macro_h
				if absf(macro_h) <= COAST_DETAIL_BAND_M:
					h = Planet.pristine_height(d, detail)
				img.set_pixel(x, y, Color(h, 0.0, 0.0, 1.0))
		images.append(img)

	var texture_array := Texture2DArray.new()
	var err := texture_array.create_from_images(images)
	if err != OK:
		push_error("Failed to build detailed orbit elevation texture (%d)" % err)
		return {}
	return {
		"texture": texture_array,
		"face_res": res,
	}
