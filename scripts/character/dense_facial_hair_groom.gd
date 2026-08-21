extends "res://scripts/character/natural_facial_hair_groom.gd"

## Denser neutral facial-hair baseline.
## Geometry generation, surface projection, LODs and exact face-morph transfer
## remain inherited from natural_facial_hair_groom.gd. This pass only changes
## the canonical starting values so the groom reads as a real short beard and
## full mustache instead of isolated sparse whiskers.

func _apply_natural_facial_defaults() -> void:
	mustache_settings["density"] = 1.0
	mustache_settings["width"] = 0.86
	mustache_settings["thickness"] = 1.15
	mustache_settings["length"] = 0.0055
	mustache_settings["strand_width"] = 0.00042
	mustache_settings["middle_gap"] = 0.007
	mustache_settings["droop"] = 0.18
	mustache_settings["height_offset"] = 0.0
	mustache_settings["forward_offset"] = 0.00055
	mustache_settings["messiness"] = 0.12

	beard_settings["density"] = 1.0
	beard_settings["coverage"] = 0.68
	beard_settings["fullness"] = 1.10
	beard_settings["length"] = 0.0055
	beard_settings["chin_length"] = 0.0065
	beard_settings["strand_width"] = 0.00042
	beard_settings["height_offset"] = 0.0
	beard_settings["forward_offset"] = 0.00055
	beard_settings["messiness"] = 0.12
