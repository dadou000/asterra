class_name BiomePaintLayer
extends Resource
## Sparse, non-destructive biome overrides painted after procedural generation.
## Strokes are stored in planet-centric directions and real metre radii so the
## authoring data is independent of the current clipmap resolution.

enum BlendMode {
	REPLACE,
	ERASE,
}

@export var layer_id: String = ""
@export var display_name: String = "Biome Paint"
@export var enabled: bool = true
@export var opacity: float = 1.0
@export var priority: int = 0
@export_enum("Replace", "Erase") var blend_mode: int = BlendMode.REPLACE
@export_range(0, 17, 1) var active_biome_id: int = 6
@export_range(0.05, 1000000.0, 0.05) var brush_radius_m: float = 50.0
@export_range(0.0, 1.0, 0.001) var brush_hardness: float = 0.75
@export_range(0.0, 1.0, 0.001) var brush_opacity: float = 1.0
@export var strokes: Array[Dictionary] = []

func ensure_valid() -> void:
	if layer_id.is_empty():
		layer_id = make_layer_id(display_name)
	opacity = clampf(opacity, 0.0, 1.0)
	active_biome_id = clampi(active_biome_id, 0, 17)
	brush_radius_m = maxf(0.05, brush_radius_m)
	brush_hardness = clampf(brush_hardness, 0.0, 1.0)
	brush_opacity = clampf(brush_opacity, 0.0, 1.0)

func add_stroke(center_direction: Vector3, biome_id: int = -1, radius_m: float = -1.0, hardness: float = -1.0, stroke_opacity: float = -1.0) -> bool:
	var direction := center_direction.normalized()
	if direction.length_squared() < 0.99:
		return false
	var resolved_biome := active_biome_id if biome_id < 0 else clampi(biome_id, 0, 17)
	var resolved_radius := brush_radius_m if radius_m <= 0.0 else maxf(0.05, radius_m)
	var resolved_hardness := brush_hardness if hardness < 0.0 else clampf(hardness, 0.0, 1.0)
	var resolved_opacity := brush_opacity if stroke_opacity < 0.0 else clampf(stroke_opacity, 0.0, 1.0)
	strokes.append({
		"center_dir": direction,
		"biome_id": resolved_biome,
		"radius_m": resolved_radius,
		"hardness": resolved_hardness,
		"opacity": resolved_opacity,
	})
	return true

func clear_strokes() -> void:
	strokes.clear()

func stroke_count() -> int:
	return strokes.size()

static func make_layer_id(label: String) -> String:
	var safe := label.strip_edges().to_lower().replace(" ", "-")
	if safe.is_empty():
		safe = "biome-layer"
	return "%s-%d-%d" % [safe, Time.get_ticks_usec(), randi() & 0x7fffffff]
