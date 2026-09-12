class_name CloudNoiseGenerator
extends RefCounted
## Deterministic procedural noise factory for Asterra's volumetric clouds.
##
## The cloud renderer deliberately has no runtime dependency on the EVE/Kerbin
## reference textures under docs/reference. Shape and detail volumes are synthesized
## by Godot from the world seed and can therefore be regenerated on every machine.
##
## The old uvnoise1.dds role is handled by cloud_depth_composite.glsl itself: it
## performs a low-frequency 3D domain warp from three decorrelated samples of the
## shape volume, so a separate 2D UV-noise asset is unnecessary.

const SHAPE_SIZE := 96
const DETAIL_SIZE := 64

const SHAPE_FREQUENCY := 0.055
const DETAIL_FREQUENCY := 0.085
const DETAIL_OCTAVES := 4
const DETAIL_GAIN := 0.53
const DETAIL_LACUNARITY := 2.11

const SHAPE_SEED_SALT := 0x43A51
const DETAIL_SEED_SALT := 0x7D19B


static func create_shape_volume(world_seed: int) -> NoiseTexture3D:
	# One raw cellular/Worley octave is intentional. The compositor combines
	# decorrelated octave samples itself with the EVE/Kerbin persistence (0.57),
	# which keeps cloud morphology identical for visible clouds and light marching.
	var noise := FastNoiseLite.new()
	noise.seed = seed32(world_seed, SHAPE_SEED_SALT)
	noise.noise_type = FastNoiseLite.TYPE_CELLULAR
	noise.frequency = SHAPE_FREQUENCY
	noise.fractal_type = FastNoiseLite.FRACTAL_NONE
	noise.cellular_distance_function = FastNoiseLite.DISTANCE_EUCLIDEAN
	noise.cellular_return_type = FastNoiseLite.RETURN_DISTANCE
	noise.cellular_jitter = 1.0
	noise.domain_warp_enabled = false

	var texture := NoiseTexture3D.new()
	texture.width = SHAPE_SIZE
	texture.height = SHAPE_SIZE
	texture.depth = SHAPE_SIZE
	texture.seamless = true
	texture.seamless_blend_skirt = 0.12
	texture.normalize = true
	texture.noise = noise
	return texture


static func create_detail_volume(world_seed: int) -> NoiseTexture3D:
	# Ridged high-frequency noise erodes cloud boundaries after the macro Worley
	# body has been established. This is the procedural replacement for detail1.dds.
	var noise := FastNoiseLite.new()
	noise.seed = seed32(world_seed, DETAIL_SEED_SALT)
	noise.noise_type = FastNoiseLite.TYPE_SIMPLEX_SMOOTH
	noise.frequency = DETAIL_FREQUENCY
	noise.fractal_type = FastNoiseLite.FRACTAL_RIDGED
	noise.fractal_octaves = DETAIL_OCTAVES
	noise.fractal_gain = DETAIL_GAIN
	noise.fractal_lacunarity = DETAIL_LACUNARITY

	var texture := NoiseTexture3D.new()
	texture.width = DETAIL_SIZE
	texture.height = DETAIL_SIZE
	texture.depth = DETAIL_SIZE
	texture.seamless = true
	texture.seamless_blend_skirt = 0.14
	texture.normalize = true
	texture.noise = noise
	return texture


static func seed32(world_seed: int, salt: int) -> int:
	# FastNoiseLite stores a signed 32-bit seed. Fold Asterra's deterministic
	# 64-bit world seed without touching the global RNG.
	var mixed := world_seed ^ (salt * 0x45D9F3B)
	mixed = mixed ^ (mixed >> 16)
	return int(mixed & 0x7FFFFFFF)
