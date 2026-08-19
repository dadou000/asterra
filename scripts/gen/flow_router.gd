class_name FlowRouter
extends RefCounted
## Deterministic D8 flow routing on the cube-sphere grid.
##
## Shared by the erosion pass and the hydrology pass so that "where water goes"
## has exactly one definition in the codebase. Everything here is O(N) or
## O(N log N) and order-independent, so two bakes of the same seed give the same
## drainage network on any machine.

var grid: PlanetGrid
var n: int

## Receiver cell of each cell (== itself for outlets and ocean).
var rec := PackedInt32Array()
## Neighbour slot 0..7 used to reach the receiver, 255 for outlets.
var rec_slot := PackedByteArray()
## Horizontal distance to the receiver, metres.
var rec_dist := PackedFloat32Array()
## Depression-filled surface used for routing (>= real elevation).
var filled := PackedFloat32Array()
## Traversal order: outlets first, headwaters last.
var order := PackedInt32Array()

var _donor_start := PackedInt32Array()
var _donor_list := PackedInt32Array()

func _init(p_grid: PlanetGrid) -> void:
	grid = p_grid
	n = grid.cell_count
	rec.resize(n)
	rec_slot.resize(n)
	rec_dist.resize(n)
	filled.resize(n)
	order.resize(n)

## Fill closed depressions up to their spill point (priority flood, Barnes 2014).
## Cells below sea level seed the queue, so every land cell ends up with a
## monotone path to the ocean -- the precondition for a valid river network.
func fill_depressions(elev: PackedFloat32Array, sea: float = 0.0, eps: float = 1e-3) -> void:
	var heap := MinHeap.new()
	var visited := PackedByteArray()
	visited.resize(n)
	for c in n:
		filled[c] = elev[c]
		if elev[c] <= sea:
			visited[c] = 1
			heap.push(elev[c], c)
	# Degenerate world with no ocean: seed from the global minimum.
	if heap.is_empty():
		var lo := 1e30
		var lo_c := 0
		for c in n:
			if elev[c] < lo:
				lo = elev[c]
				lo_c = c
		visited[lo_c] = 1
		heap.push(lo, lo_c)
	while not heap.is_empty():
		var top := heap.pop()
		var h: float = top[0]
		var c: int = top[1]
		var base := c * 8
		for k in 8:
			var nb := grid.nbr[base + k]
			if visited[nb] == 1:
				continue
			visited[nb] = 1
			var nh: float = maxf(elev[nb], h + eps)
			filled[nb] = nh
			heap.push(nh, nb)

## Steepest-descent receivers on the filled surface.
func compute_receivers(sea: float = 0.0) -> void:
	for c in n:
		var base := c * 8
		var hc := filled[c]
		var best_slope := 0.0
		var best := c
		var best_k := 255
		var best_d := grid.cell_size[c]
		if hc > sea:
			for k in 8:
				var nb := grid.nbr[base + k]
				var dist := grid.cell_size[c] * (1.41421356 if (k & 1) == 1 else 1.0)
				var slope := (hc - filled[nb]) / dist
				if slope > best_slope:
					best_slope = slope
					best = nb
					best_k = k
					best_d = dist
		rec[c] = best
		rec_slot[c] = best_k
		rec_dist[c] = best_d

## Build the outlet-first traversal order in O(N) from the receiver tree.
func build_order() -> void:
	var count := PackedInt32Array()
	count.resize(n)
	for c in n:
		if rec[c] != c:
			count[rec[c]] += 1
	_donor_start = PackedInt32Array()
	_donor_start.resize(n + 1)
	var acc := 0
	for c in n:
		_donor_start[c] = acc
		acc += count[c]
	_donor_start[n] = acc
	var cursor := _donor_start.duplicate()
	_donor_list = PackedInt32Array()
	_donor_list.resize(acc)
	for c in n:
		var r := rec[c]
		if r != c:
			_donor_list[cursor[r]] = c
			cursor[r] += 1
	# Breadth-first from every outlet.
	var head := 0
	var tail := 0
	for c in n:
		if rec[c] == c:
			order[tail] = c
			tail += 1
	while head < tail:
		var c := order[head]
		head += 1
		for i in range(_donor_start[c], _donor_start[c + 1]):
			order[tail] = _donor_list[i]
			tail += 1
	# Any cell not reached (should not happen after filling) is appended.
	if tail < n:
		var seen := PackedByteArray()
		seen.resize(n)
		for i in tail:
			seen[order[i]] = 1
		for c in n:
			if seen[c] == 0:
				order[tail] = c
				tail += 1

func donors_of(c: int) -> PackedInt32Array:
	return _donor_list.slice(_donor_start[c], _donor_start[c + 1])

func donor_count(c: int) -> int:
	return _donor_start[c + 1] - _donor_start[c]

## Accumulate a per-cell weight downstream (headwaters -> outlets).
func accumulate(weight: PackedFloat32Array) -> PackedFloat32Array:
	var out := weight.duplicate()
	for i in range(n - 1, -1, -1):
		var c := order[i]
		var r := rec[c]
		if r != c:
			out[r] += out[c]
	return out

func route_and_order(elev: PackedFloat32Array, sea: float = 0.0) -> void:
	fill_depressions(elev, sea)
	compute_receivers(sea)
	build_order()
