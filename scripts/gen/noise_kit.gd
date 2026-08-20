class_name NoiseKit
extends RefCounted
## Thin deterministic wrapper over FastNoiseLite.
##
## FastNoiseLite is native (fast) and integer-hash based (identical on every
## platform), so it satisfies the "deterministic from a canonical world seed"
## requirement while keeping the bake within a sane time budget. All sampling is
## 3D on the unit sphere, which makes every field seamless across cube faces by
## construction -- there is no UV wrap to special-case.

var n: FastNoiseLite

func _init(p_seed: int, frequency: float, octaves: int = 4, lacunarity: float = 2.0,
		gain: float = 0.5, type: int = FastNoiseLite.TYPE_SIMPLEX_SMOOTH) -> void:
	n = FastNoiseLite.new()
	n.seed = p_seed
	n.noise_type = type as FastNoiseLite.NoiseType
	n.frequency = frequency
	n.fractal_type = FastNoiseLite.FRACTAL_FBM
	n.fractal_octaves = octaves
	n.fractal_lacunarity = lacunarity
	n.fractal_gain = gain

static func cellular(p_seed: int, frequency: float, ret: int = FastNoiseLite.RETURN_DISTANCE2_SUB) -> NoiseKit:
	var k := NoiseKit.new(p_seed, frequency, 1)
	k.n.noise_type = FastNoiseLite.TYPE_CELLULAR
	k.n.cellular_return_type = ret as FastNoiseLite.CellularReturnType
	k.n.cellular_distance_function = FastNoiseLite.DISTANCE_EUCLIDEAN
	k.n.fractal_type = FastNoiseLite.FRACTAL_NONE
	return k

static func ridged(p_seed: int, frequency: float, octaves: int = 5) -> NoiseKit:
	var k := NoiseKit.new(p_seed, frequency, octaves)
	k.n.fractal_type = FastNoiseLite.FRACTAL_RIDGED
	return k

## [-1, 1]
func s(d: Vector3) -> float:
	return n.get_noise_3d(d.x, d.y, d.z)

## [0, 1]
func u(d: Vector3) -> float:
	return n.get_noise_3d(d.x, d.y, d.z) * 0.5 + 0.5

## Sample at an arbitrary scale on the unit sphere (radius multiplier).
func at(d: Vector3, scale: float) -> float:
	return n.get_noise_3d(d.x * scale, d.y * scale, d.z * scale)

## Domain-warp a direction on the sphere and renormalise.
static func warp(d: Vector3, wx: NoiseKit, wy: NoiseKit, wz: NoiseKit, amount: float) -> Vector3:
	return (d + Vector3(wx.s(d), wy.s(d), wz.s(d)) * amount).normalized()

static func smoothstepf(a: float, b: float, x: float) -> float:
	if b == a:
		return 0.0
	var t := clampf((x - a) / (b - a), 0.0, 1.0)
	return t * t * (3.0 - 2.0 * t)
