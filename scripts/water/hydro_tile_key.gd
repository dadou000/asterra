class_name HydroTileKey
extends RefCounted
## Stable sparse surface-hydrology tile identity.
##
## Surface hydrology is a quadtree per cube-sphere face. The packed ID keeps the
## hierarchy deterministic across streaming/save-load and is independent of GPU
## atlas slot ownership.

const MAX_LEVEL := 27
const MORTON_BITS := 54
const LEVEL_SHIFT := 54
const FACE_SHIFT := 59
const MORTON_MASK := (1 << MORTON_BITS) - 1
const LEVEL_MASK := 0x1f
const FACE_MASK := 0x7

var face := 0
var level := 0
var x := 0
var y := 0


func _init(p_face: int = 0, p_level: int = 0, p_x: int = 0, p_y: int = 0) -> void:
	face = clampi(p_face, 0, 5)
	level = clampi(p_level, 0, MAX_LEVEL)
	var side := 1 << level
	x = clampi(p_x, 0, side - 1)
	y = clampi(p_y, 0, side - 1)


func packed() -> int:
	return pack(face, level, x, y)


func parent() -> HydroTileKey:
	if level <= 0:
		return HydroTileKey.new(face, 0, 0, 0)
	return HydroTileKey.new(face, level - 1, x >> 1, y >> 1)


func child(index: int) -> HydroTileKey:
	var q := clampi(index, 0, 3)
	return HydroTileKey.new(face, mini(level + 1, MAX_LEVEL),
		(x << 1) | (q & 1), (y << 1) | ((q >> 1) & 1))


func same_face_neighbor(dx: int, dy: int) -> HydroTileKey:
	var side := 1 << level
	var nx := x + dx
	var ny := y + dy
	if nx < 0 or ny < 0 or nx >= side or ny >= side:
		return null # cube-face seam mapping belongs to the planetary neighbor mapper
	return HydroTileKey.new(face, level, nx, ny)


func equals(other: HydroTileKey) -> bool:
	return other != null and packed() == other.packed()


func _to_string() -> String:
	return "HydroTileKey(face=%d level=%d x=%d y=%d morton=0x%x)" % [
		face, level, x, y, morton_encode(x, y)]


static func pack(p_face: int, p_level: int, p_x: int, p_y: int) -> int:
	var l := clampi(p_level, 0, MAX_LEVEL)
	var side := 1 << l
	var px := clampi(p_x, 0, side - 1)
	var py := clampi(p_y, 0, side - 1)
	var morton := morton_encode(px, py) & MORTON_MASK
	return morton | ((l & LEVEL_MASK) << LEVEL_SHIFT) \
		| ((clampi(p_face, 0, 5) & FACE_MASK) << FACE_SHIFT)


static func unpack(id: int) -> HydroTileKey:
	var p_face := (id >> FACE_SHIFT) & FACE_MASK
	var p_level := (id >> LEVEL_SHIFT) & LEVEL_MASK
	var morton := id & MORTON_MASK
	var xy := morton_decode(morton, p_level)
	return HydroTileKey.new(p_face, p_level, xy.x, xy.y)


static func morton_encode(px: int, py: int) -> int:
	var code := 0
	for bit in MAX_LEVEL:
		code |= ((px >> bit) & 1) << (bit * 2)
		code |= ((py >> bit) & 1) << (bit * 2 + 1)
	return code


static func morton_decode(code: int, p_level: int = MAX_LEVEL) -> Vector2i:
	var px := 0
	var py := 0
	var count := clampi(p_level, 0, MAX_LEVEL)
	for bit in count:
		px |= ((code >> (bit * 2)) & 1) << bit
		py |= ((code >> (bit * 2 + 1)) & 1) << bit
	return Vector2i(px, py)
