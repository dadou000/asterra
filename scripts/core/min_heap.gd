class_name MinHeap
extends RefCounted
## Binary min-heap keyed by float, carrying an int payload.
## Used by the priority-flood depression solver in the hydrology pass.

var _key: PackedFloat32Array = PackedFloat32Array()
var _val: PackedInt32Array = PackedInt32Array()
var _n: int = 0

func size() -> int:
	return _n

func is_empty() -> bool:
	return _n == 0

func clear() -> void:
	_n = 0
	_key.clear()
	_val.clear()

func push(k: float, v: int) -> void:
	if _n >= _key.size():
		var grow := maxi(64, _key.size() * 2)
		_key.resize(grow)
		_val.resize(grow)
	var i := _n
	_key[i] = k
	_val[i] = v
	_n += 1
	while i > 0:
		var p := (i - 1) >> 1
		if _key[p] <= _key[i]:
			break
		_swap(p, i)
		i = p

func pop() -> Array:
	var rk := _key[0]
	var rv := _val[0]
	_n -= 1
	if _n > 0:
		_key[0] = _key[_n]
		_val[0] = _val[_n]
		var i := 0
		while true:
			var l := i * 2 + 1
			var r := l + 1
			var m := i
			if l < _n and _key[l] < _key[m]:
				m = l
			if r < _n and _key[r] < _key[m]:
				m = r
			if m == i:
				break
			_swap(m, i)
			i = m
	return [rk, rv]

func _swap(a: int, b: int) -> void:
	var tk := _key[a]
	_key[a] = _key[b]
	_key[b] = tk
	var tv := _val[a]
	_val[a] = _val[b]
	_val[b] = tv
