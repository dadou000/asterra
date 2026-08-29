class_name TerrainShaderSlotDefinition
extends Resource
## One independently maskable terrain graph slot. Multiple slots may target the
## same clipmap level; displacement slots compose in order and can construct or
## destruct terrain through signed strength / subtraction blend modes.

const GRAPH_SCRIPT := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")

enum Domain {
	DISPLACEMENT,
	MATERIAL,
}

enum BiomeMaskMode {
	ALL,
	ONLY,
	EXCEPT,
}

enum BlendMode {
	ADD,
	SUBTRACT,
	MULTIPLY,
	MIN,
	MAX,
	REPLACE,
}

@export var slot_id: String = ""
@export var display_name: String = "Terrain Slot"
@export var enabled: bool = true
@export_enum("Displacement", "Material") var domain: int = Domain.DISPLACEMENT
@export_enum("Add", "Subtract", "Multiply", "Min", "Max", "Replace") var blend_mode: int = BlendMode.ADD
@export_range(-1000.0, 1000.0, 0.001) var strength: float = 1.0
@export var clipmap_level_mask: int = 0xff
@export_enum("All biomes", "Only selected", "All except selected") var biome_mask_mode: int = BiomeMaskMode.ALL
@export var biome_ids: PackedInt32Array = PackedInt32Array()
@export var graph: Resource

func ensure_valid() -> void:
	if slot_id.is_empty():
		slot_id = make_slot_id(display_name)
	clipmap_level_mask &= 0x7fffffff
	var sanitized := PackedInt32Array()
	for biome_id: int in biome_ids:
		var value := clampi(biome_id, 0, 17)
		if not sanitized.has(value):
			sanitized.append(value)
	biome_ids = sanitized
	if graph == null:
		graph = GRAPH_SCRIPT.new()
		graph.set(&"display_name", "%s Graph" % display_name)
		graph.set(&"domain", domain)
		graph.call("create_default_graph", domain)
	else:
		graph.set(&"domain", domain)
		graph.call("ensure_valid")

func applies_to_clipmap(level: int) -> bool:
	if level < 0 or level >= 31:
		return false
	return (clipmap_level_mask & (1 << level)) != 0

func set_clipmap_enabled(level: int, value: bool) -> void:
	if level < 0 or level >= 31:
		return
	if value:
		clipmap_level_mask |= 1 << level
	else:
		clipmap_level_mask &= ~(1 << level)

func applies_to_biome(biome_id: int) -> bool:
	var selected := biome_ids.has(clampi(biome_id, 0, 17))
	match biome_mask_mode:
		BiomeMaskMode.ONLY:
			return selected
		BiomeMaskMode.EXCEPT:
			return not selected
		_:
			return true

static func make_slot_id(label: String) -> String:
	var safe := label.strip_edges().to_lower().replace(" ", "-")
	if safe.is_empty():
		safe = "terrain-slot"
	return "%s-%d-%d" % [safe, Time.get_ticks_usec(), randi() & 0x7fffffff]
