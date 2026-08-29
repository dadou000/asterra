class_name AuthoringRingSystemDefinition
extends Resource
## First persistent ring-system contract. The renderer is added in Phase 1.

@export var enabled: bool = false
@export var inner_radius_m: float = 0.0
@export var outer_radius_m: float = 0.0
@export_range(0.0, 1.0, 0.001) var optical_depth: float = 0.35
@export_range(0.0, 1.0, 0.001) var roughness: float = 0.75
@export var tint: Color = Color(0.72, 0.68, 0.61, 1.0)
@export var texture_asset_id: String = ""
@export var bands: Array[Dictionary] = []

func normalize_ranges(reference_radius_m: float) -> void:
	inner_radius_m = maxf(inner_radius_m, reference_radius_m)
	outer_radius_m = maxf(outer_radius_m, inner_radius_m)
