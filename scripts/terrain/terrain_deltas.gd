extends Node
## Autoload: sparse runtime terrain deltas.
##
## 1.7 "Store only player/runtime deltas where possible; regenerate untouched
## terrain from seed." Untouched ground costs zero bytes. Edits are stored as
## height offsets on a fixed cube-sphere lattice (~0.75 m spacing on a 1000 km
## planet) grouped into 64x64 tiles, so a save file scales with how much the
## player actually dug, not with the size of the planet.

const EDIT_DEPTH := 21                  ## lattice = 2^EDIT_DEPTH samples per face edge
const LATTICE := 1 << EDIT_DEPTH
const TILE := 64
const TILE_SHIFT := 6
const FORMAT_VERSION := 1
## Fraction of a lattice interval used only to disambiguate an exact cube edge.
## CubeSphere.dir_to_face_uv() deliberately resolves equal dominant axes to one
## face; probing a tiny distance past the source edge selects the face that owns
## the next sample without moving far enough to change its rounded lattice index.
const SEAM_PROBE := 0.125

signal region_changed(center_dir: Vector3, radius_m: float)

var _tiles: Dictionary = {}             ## tile_key:int -> PackedFloat32Array(TILE*TILE)
var _mutex := Mutex.new()
var _count: int = 0

func clear() -> void:
	_mutex.lock()
	_tiles.clear()
	_count = 0
	_mutex.unlock()

func is_empty() -> bool:
	return _count == 0

func edited_tile_count() -> int:
	return _count

## Metres between lattice samples at the surface.
func sample_spacing(radius: float) -> float:
	return (PI * 0.5 * radius) / float(LATTICE)

# ------------------------------------------------------------- addressing ---
func tile_key(face: int, ti: int, tj: int) -> int:
	return (face << 30) | (tj << 15) | ti

## Direction -> continuous lattice coordinates [face, gi, gj].
func dir_to_lattice(d: Vector3) -> Array:
	var fuv := CubeSphere.dir_to_face_uv(d)
	return [fuv[0], (fuv[1] * 0.5 + 0.5) * float(LATTICE), (fuv[2] * 0.5 + 0.5) * float(LATTICE)]

func lattice_to_dir(face: int, gi: float, gj: float) -> Vector3:
	return CubeSphere.face_uv_to_dir(face, gi / float(LATTICE) * 2.0 - 1.0, gj / float(LATTICE) * 2.0 - 1.0)

## Convert a possibly off-face integer sample to the single sparse-lattice
## address that owns it. The edit lattice is half-open on each face
## (`0 .. LATTICE - 1`), so the positive edge belongs to the adjacent face.
## Reprojection also rotates or mirrors the edge coordinate as required by that
## face's cube-sphere basis; arithmetic wrapping cannot do that correctly.
func canonical_address(face: int, i: int, j: int) -> Vector3i:
	if face < 0 or face >= 6:
		return Vector3i(-1, -1, -1)
	if i >= 0 and i < LATTICE and j >= 0 and j < LATTICE:
		return Vector3i(face, i, j)

	var pi := float(i)
	var pj := float(j)
	if i < 0:
		pi -= SEAM_PROBE
	elif i >= LATTICE:
		pi += SEAM_PROBE
	if j < 0:
		pj -= SEAM_PROBE
	elif j >= LATTICE:
		pj += SEAM_PROBE

	var owner := dir_to_lattice(lattice_to_dir(face, pi, pj))
	var owner_face: int = owner[0]
	var owner_i := int(round(float(owner[1])))
	var owner_j := int(round(float(owner[2])))
	# At the eight cube corners every incident face contains an exact positive
	# edge. There is no fourth face to probe into, so choose its nearest valid
	# sample. This fallback is sub-sample displacement and keeps one stable owner.
	return Vector3i(owner_face,
		clampi(owner_i, 0, LATTICE - 1),
		clampi(owner_j, 0, LATTICE - 1))

# ------------------------------------------------------------------ read ---
## Height offset in metres at a direction, bilinear over the lattice.
func offset_at(d: Vector3) -> float:
	if _count == 0:
		return 0.0
	var l := dir_to_lattice(d)
	return _offset_lattice(l[0], l[1], l[2], _tiles, true)

## Thread-safe read from a snapshot taken with snapshot_for_bounds().
func offset_at_snapshot(d: Vector3, snap: Dictionary) -> float:
	if snap.is_empty():
		return 0.0
	var l := dir_to_lattice(d)
	return _offset_lattice(l[0], l[1], l[2], snap, false)

func _offset_lattice(face: int, gi: float, gj: float, src: Dictionary, lock: bool) -> float:
	var i0 := int(floor(gi))
	var j0 := int(floor(gj))
	var tx := gi - float(i0)
	var ty := gj - float(j0)
	if lock:
		_mutex.lock()
	var v00 := _raw(src, face, i0, j0)
	var v10 := _raw(src, face, i0 + 1, j0)
	var v01 := _raw(src, face, i0, j0 + 1)
	var v11 := _raw(src, face, i0 + 1, j0 + 1)
	if lock:
		_mutex.unlock()
	return lerpf(lerpf(v00, v10, tx), lerpf(v01, v11, tx), ty)

func _raw(src: Dictionary, face: int, i: int, j: int) -> float:
	var address := canonical_address(face, i, j)
	if address.x < 0:
		return 0.0
	face = address.x
	i = address.y
	j = address.z
	var k := tile_key(face, i >> TILE_SHIFT, j >> TILE_SHIFT)
	if not src.has(k):
		return 0.0
	var t: PackedFloat32Array = src[k]
	return t[(j & (TILE - 1)) * TILE + (i & (TILE - 1))]

## Duplicate every tile overlapping a spherical cap, for use on a worker thread.
func snapshot_for_bounds(center: Vector3, angular_radius: float) -> Dictionary:
	if _count == 0:
		return {}
	var out := {}
	_mutex.lock()
	# Cheap and correct: sample the cap's bounding box on each touched face.
	var keys: Array = _tiles.keys()
	for k in keys:
		var face: int = (int(k) >> 30) & 0x7
		var ti: int = int(k) & 0x7FFF
		var tj: int = (int(k) >> 15) & 0x7FFF
		var cd := lattice_to_dir(face,
			float(ti * TILE) + float(TILE) * 0.5,
			float(tj * TILE) + float(TILE) * 0.5)
		# Tile half-diagonal in radians, generously padded.
		var tile_ang := (float(TILE) / float(LATTICE)) * PI
		if cd.angle_to(center) <= angular_radius + tile_ang:
			out[k] = (_tiles[k] as PackedFloat32Array).duplicate()
	_mutex.unlock()
	return out

# ----------------------------------------------------------------- write ---
## Add `delta` metres at a lattice sample. Off-face addresses are reprojected
## onto the adjacent cube face, so a brush can cross an edge without truncation.
## Returns the applied change.
func add_offset(face: int, i: int, j: int, delta: float, min_offset: float, max_offset: float) -> float:
	if delta == 0.0:
		return 0.0
	var address := canonical_address(face, i, j)
	if address.x < 0:
		return 0.0
	face = address.x
	i = address.y
	j = address.z
	var k := tile_key(face, i >> TILE_SHIFT, j >> TILE_SHIFT)
	_mutex.lock()
	if not _tiles.has(k):
		var arr := PackedFloat32Array()
		arr.resize(TILE * TILE)
		_tiles[k] = arr
		_count += 1
	var t: PackedFloat32Array = _tiles[k]
	var idx := (j & (TILE - 1)) * TILE + (i & (TILE - 1))
	var before := t[idx]
	var after := clampf(before + delta, min_offset, max_offset)
	t[idx] = after
	_tiles[k] = t
	_mutex.unlock()
	return after - before

func get_offset(face: int, i: int, j: int) -> float:
	_mutex.lock()
	var v := _raw(_tiles, face, i, j)
	_mutex.unlock()
	return v

func notify_changed(center: Vector3, radius_m: float) -> void:
	region_changed.emit(center, radius_m)

# ------------------------------------------------------------ persistence ---
func serialize() -> Dictionary:
	_mutex.lock()
	var keys := PackedInt64Array()
	var blob := PackedByteArray()
	for k in _tiles.keys():
		keys.append(k)
		blob.append_array((_tiles[k] as PackedFloat32Array).to_byte_array())
	_mutex.unlock()
	return {"version": FORMAT_VERSION, "keys": keys, "tiles": blob}

func deserialize(data: Dictionary) -> void:
	clear()
	if not data.has("keys"):
		return
	var keys: PackedInt64Array = data["keys"]
	var blob: PackedByteArray = data["tiles"]
	var stride := TILE * TILE * 4
	_mutex.lock()
	for n in keys.size():
		var slice := blob.slice(n * stride, (n + 1) * stride)
		_tiles[keys[n]] = slice.to_float32_array()
		_count += 1
	_mutex.unlock()
