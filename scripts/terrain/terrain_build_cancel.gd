class_name TerrainBuildCancel
extends RefCounted
## Thread-safe cooperative cancellation shared by terrain cache workers.
## Godot worker tasks cannot be force-killed; checking this once per generated
## row lets an obsolete orbit/location request relinquish its worker in
## milliseconds instead of finishing seconds of data nobody can publish.

var _mutex := Mutex.new()
var _cancelled := false


func cancel() -> void:
	_mutex.lock()
	_cancelled = true
	_mutex.unlock()


func is_cancelled() -> bool:
	_mutex.lock()
	var value := _cancelled
	_mutex.unlock()
	return value
