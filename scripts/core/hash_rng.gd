class_name HashRNG
extends RefCounted
## Deterministic, position-addressable hashing.
##
## Every generated value in Asterra must be reproducible from (world_seed, coords)
## alone, on any machine, in any order, without a global RNG stream. These helpers
## are integer-only (wrapping 64-bit) so they behave identically everywhere.

## splitmix64 constants, written as signed 64-bit because GDScript ints are signed.
const M1 := -7046029254386353131   # 0x9E3779B97F4A7C15
const M2 := -4658895280553007687   # 0xBF58476D1CE4E5B9
const M3 := -7723592293110705685   # 0x94D049BB133111EB

## Logical (zero-filling) right shift. GDScript's >> is arithmetic, which would
## smear the sign bit through the avalanche and destroy the hash quality.
static func ushr(v: int, k: int) -> int:
	return (v >> k) & ((1 << (64 - k)) - 1)

static func mix(v: int) -> int:
	var z := v
	z = (z ^ ushr(z, 30)) * M2
	z = (z ^ ushr(z, 27)) * M3
	z = z ^ ushr(z, 31)
	return z

static func hash2(seed_v: int, a: int) -> int:
	return mix(seed_v * M1 + a * 0x27D4EB2F1BB3F8D3)

static func hash3(seed_v: int, a: int, b: int) -> int:
	return mix(seed_v * M1 + a * 0x27D4EB2F1BB3F8D3 + b * 0x165667B19E3779F9)

static func hash4(seed_v: int, a: int, b: int, c: int) -> int:
	return mix(seed_v * M1 + a * 0x27D4EB2F1BB3F8D3 + b * 0x165667B19E3779F9 + c * 0x2545F4914F6CDD1D)

## Uniform [0,1) from an integer hash.
static func unit(h: int) -> float:
	return float(ushr(h, 11)) / 9007199254740992.0

static func unit2(seed_v: int, a: int) -> float:
	return unit(hash2(seed_v, a))

static func unit3(seed_v: int, a: int, b: int) -> float:
	return unit(hash3(seed_v, a, b))

static func unit4(seed_v: int, a: int, b: int, c: int) -> float:
	return unit(hash4(seed_v, a, b, c))

## Signed [-1,1).
static func sunit4(seed_v: int, a: int, b: int, c: int) -> float:
	return unit(hash4(seed_v, a, b, c)) * 2.0 - 1.0

## Stable 32-bit seed derived from a string label plus the world seed. Used to give
## every generation pass its own independent noise stream.
static func stream(world_seed: int, label: String) -> int:
	var h := world_seed * M1
	for i in label.length():
		h = mix(h + label.unicode_at(i) * 0x100000001B3)
	return int(h & 0x7FFFFFFF)

## Deterministic unit vector on the sphere from an index (used for plate seeds).
static func unit_sphere(seed_v: int, idx: int) -> Vector3:
	var u := unit3(seed_v, idx, 11) * 2.0 - 1.0
	var t := unit3(seed_v, idx, 23) * TAU
	var r := sqrt(maxf(0.0, 1.0 - u * u))
	return Vector3(r * cos(t), u, r * sin(t))
