class_name WorldAuthoringLiveEditorPhase4
extends "res://scripts/world_authoring/world_authoring_editor_live_phase3.gd"
## Phase 4 authored-water inspector parity.
##
## The Phase-3 placement tool and runtime already consume per-feature frequency,
## but the inherited water page only exposed amplitude/current. Keep the proven
## editor intact and append the missing live controls/semantics here.


func _build_water_feature_editor(water: Resource, body: Resource) -> void:
	super._build_water_feature_editor(water, body)
	if water == null or _selected_water_feature_id.is_empty():
		return
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource
	if feature == null:
		return

	_section("Live inland-water runtime")
	_add_number_field("Feature wave frequency",
		float(feature.get(&"wave_frequency_scale")),
		0.01, 20.0, 0.01, "×", func(value: float) -> void:
			_session.stage_set(feature, &"wave_frequency_scale", value,
				WorldAuthoringSession.ApplyScope.HOT, "Change feature wave frequency")
	)
	var is_river: bool = int(feature.get(&"feature_type")) == 1
	if is_river:
		_add_note("River surface level is a vertical offset from the authored Bézier path. Width, depth and current interpolate along the knots; the runtime builds a spherical ribbon, clips it against current terrain and feeds the same current/waves to buoyancy probes.")
	else:
		_add_note("Lake surface level is an absolute altitude above the body radius. A new lake adopts the first terrain-click altitude while the field is still at its untouched 0 m default. The runtime triangulates/subdivides the polygon on the sphere and clips exposed ground non-destructively.")
