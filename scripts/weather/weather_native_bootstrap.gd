extends Node
## Loads the native weather extension before WeatherSystem starts.
##
## The tracked source tree intentionally does not contain a live .gdextension
## resource pointing at a missing DLL. CMake copies the manifest into res://bin
## only after a successful native build, so an unbuilt checkout boots cleanly.

const EXTENSION_PATH := "res://bin/asterra_weather.gdextension"

var loaded := false
var load_status := -1


func _init() -> void:
	if ClassDB.class_exists(&"WeatherNative"):
		loaded = true
		return
	if not FileAccess.file_exists(EXTENSION_PATH):
		return

	load_status = int(GDExtensionManager.load_extension(EXTENSION_PATH))
	loaded = ClassDB.class_exists(&"WeatherNative")
	if not loaded:
		push_error("WeatherNativeBootstrap: failed to load %s (status %d)" % [
			EXTENSION_PATH, load_status])
