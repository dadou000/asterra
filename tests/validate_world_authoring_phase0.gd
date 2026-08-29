extends SceneTree
## Headless invariants for Planet Studio staging plus the Phase 1/2 authoring model.

const SESSION_SCRIPT := preload("res://scripts/world_authoring/world_authoring_session.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const SHADER_SLOT_SCRIPT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const WATER_FEATURE_SCRIPT := preload("res://scripts/world_authoring/model/water_feature_definition.gd")

func _init() -> void:
	var session: RefCounted = SESSION_SCRIPT.new()
	session.bootstrap_from_current_world()
	_assert(session.staged_system != null, "staged system missing")
	_assert(session.applied_system != null, "applied system missing")
	var bodies: Array = session.staged_system.get(&"bodies")
	_assert(bodies.size() == 1, "bootstrap must import exactly the current Asterra body")
	var body: Resource = session.active_body()
	_assert(body != null, "active body missing")
	_assert(is_equal_approx(float(body.get(&"radius_m")), 1000000.0), "Planet Studio did not import Asterra radius")

	var planet_profile: Resource = body.get(&"planet_profile") as Resource
	var terrain: Resource = planet_profile.get(&"terrain") as Resource
	var water: Resource = planet_profile.get(&"water") as Resource
	var generation: Resource = terrain.get(&"generation_profile") as Resource
	_assert(int(generation.get(&"world_seed")) == 4707684752384786688, "Planet Studio lost the 64-bit world seed")
	_assert(is_equal_approx(float(generation.get(&"ocean_fraction")), 0.62), "Planet Studio did not import ocean fraction")
	_assert(int(generation.get(&"quadtree_max_depth")) == 10, "Planet Studio did not import world.tres streaming override")

	var original_radius: float = float(body.get(&"radius_m"))
	session.stage_set(body, &"radius_m", original_radius + 1234.0, SESSION_SCRIPT.ApplyScope.FULL_REBUILD, "test radius")
	_assert(session.dirty, "edit did not mark session dirty")
	_assert(session.can_undo(), "edit did not create undo history")
	session.undo()
	body = session.active_body()
	_assert(is_equal_approx(float(body.get(&"radius_m")), original_radius), "undo did not restore radius")
	_assert(not session.dirty, "undo to bootstrap state should restore clean state")
	session.redo()
	body = session.active_body()
	_assert(is_equal_approx(float(body.get(&"radius_m")), original_radius + 1234.0), "redo did not restore radius edit")

	# Re-resolve nested resources after undo/redo because staged_system is replaced.
	planet_profile = body.get(&"planet_profile") as Resource
	terrain = planet_profile.get(&"terrain") as Resource
	water = planet_profile.get(&"water") as Resource

	var biome_layer: Resource = session.create_biome_layer("CI Biome Paint")
	_assert(biome_layer != null, "biome layer creation failed")
	var biome_layer_id: String = String(biome_layer.get(&"layer_id"))
	_assert(not biome_layer_id.is_empty(), "biome layer id missing")
	_assert(session.add_biome_stroke(biome_layer_id, Vector3.UP, 14, 125.0, 0.6, 0.8), "biome stroke creation failed")
	_assert(int(biome_layer.call("stroke_count")) == 1, "biome stroke was not stored")
	var biome_layers: Array = terrain.get(&"biome_override_layers")
	_assert(biome_layers.size() == 1, "biome layer missing from terrain profile")

	var displacement_slot: Resource = session.create_terrain_shader_slot(SHADER_SLOT_SCRIPT.Domain.DISPLACEMENT, "CI Displacement")
	_assert(displacement_slot != null, "displacement slot creation failed")
	_assert(bool(displacement_slot.call("applies_to_clipmap", 0)), "default displacement slot must target L0")
	_assert(bool(displacement_slot.call("applies_to_clipmap", 7)), "default displacement slot must target L7")
	session.stage_action("CI clipmap mask", func() -> void:
		displacement_slot.call("set_clipmap_enabled", 7, false)
	, SESSION_SCRIPT.ApplyScope.GRAPH)
	_assert(not bool(displacement_slot.call("applies_to_clipmap", 7)), "clipmap mask edit failed")
	session.stage_set(displacement_slot, &"biome_mask_mode", SHADER_SLOT_SCRIPT.BiomeMaskMode.ONLY, SESSION_SCRIPT.ApplyScope.GRAPH, "CI biome mask")
	session.stage_set(displacement_slot, &"biome_ids", PackedInt32Array([7, 14]), SESSION_SCRIPT.ApplyScope.GRAPH, "CI biome selection")
	_assert(bool(displacement_slot.call("applies_to_biome", 7)), "selected biome was rejected by slot")
	_assert(not bool(displacement_slot.call("applies_to_biome", 11)), "unselected biome passed ONLY mask")
	var graph: Resource = displacement_slot.get(&"graph") as Resource
	_assert(graph != null, "displacement slot graph missing")
	var graph_nodes: Array = graph.get(&"nodes")
	_assert(graph_nodes.size() == 1, "default displacement graph should contain one output")
	var output_id: String = String((graph_nodes[0] as Dictionary).get("id", ""))
	var input_id: String = String(graph.call("add_node", "GAME_INPUT", Vector2(80.0, 120.0), {"source": "terrain_height_m"}))
	_assert(not input_id.is_empty(), "graph input node creation failed")
	_assert(bool(graph.call("connect_nodes", input_id, 0, output_id, 0)), "graph connection failed")
	var graph_links: Array = graph.get(&"links")
	_assert(graph_links.size() == 1, "graph link was not stored")

	var material_slot: Resource = session.create_terrain_shader_slot(SHADER_SLOT_SCRIPT.Domain.MATERIAL, "CI Material")
	_assert(material_slot != null, "material slot creation failed")
	_assert(int(material_slot.get(&"domain")) == SHADER_SLOT_SCRIPT.Domain.MATERIAL, "material slot domain was not preserved")

	var lake: Resource = session.create_water_feature(WATER_FEATURE_SCRIPT.FeatureType.LAKE, "CI Lake")
	_assert(lake != null, "lake creation failed")
	lake.call("add_lake_point", Vector3(1000000.0, 0.0, 0.0))
	lake.call("add_lake_point", Vector3(999990.0, 100.0, 0.0))
	lake.call("add_lake_point", Vector3(999990.0, 0.0, 100.0))
	var lake_polygon: PackedVector3Array = lake.get(&"lake_polygon_body_m")
	_assert(lake_polygon.size() == 3, "lake polygon did not store vertices")

	var river: Resource = session.create_water_feature(WATER_FEATURE_SCRIPT.FeatureType.RIVER, "CI River")
	_assert(river != null, "river creation failed")
	river.call("add_river_knot", Vector3(1000000.0, 0.0, 0.0), 20.0, 2.0, 1.5)
	river.call("add_river_knot", Vector3(999900.0, 250.0, 0.0), 35.0, 3.0, 2.5)
	_assert(int(river.call("river_segment_count")) == 1, "river did not create a Bezier segment")
	var river_midpoint: Vector3 = river.call("sample_river_segment", 0, 0.5)
	_assert(river_midpoint.length() > 1000.0, "river Bezier sampling returned an invalid point")
	_assert(is_equal_approx(float(river.call("sample_river_current", 0, 0.5)), 2.0), "river current interpolation failed")
	var water_features: Array = water.get(&"authored_features")
	_assert(water_features.size() == 2, "authored water features missing from profile")

	# Phase 2 sparse sculpt persistence. One serialized Deltas tile is 64x64 float32
	# samples (16384 bytes). The profile must survive history replacement and preset
	# round-trip without baking those offsets into the procedural generation profile.
	var sculpt_keys := PackedInt64Array([123456])
	var sculpt_blob := PackedByteArray()
	sculpt_blob.resize(64 * 64 * 4)
	var sculpt_float_bytes: PackedByteArray = PackedFloat32Array([1.25]).to_byte_array()
	for byte_index: int in sculpt_float_bytes.size():
		sculpt_blob[byte_index] = sculpt_float_bytes[byte_index]
	var sculpt_payload: Dictionary = {
		"version": 1,
		"keys": sculpt_keys,
		"tiles": sculpt_blob,
	}
	session.stage_action("CI sparse sculpt", func() -> void:
		terrain.call("set_sculpt_delta_serialized", sculpt_payload)
	, SESSION_SCRIPT.ApplyScope.TILES)
	_assert(int(terrain.call("sculpt_edited_tile_count")) == 1, "sparse sculpt tile was not stored")
	var stored_sculpt: Dictionary = terrain.call("sculpt_delta_serialized") as Dictionary
	_assert((stored_sculpt.get("tiles") as PackedByteArray).size() == 64 * 64 * 4, "sparse sculpt payload byte count changed")

	var asterra_id: String = String(body.get(&"body_id"))
	var created: Resource = session.create_body("CI Moon", BODY_SCRIPT.BodyType.MOON, asterra_id)
	_assert(created != null, "create body failed")
	bodies = session.staged_system.get(&"bodies")
	_assert(bodies.size() == 2, "created body missing from system")
	var moon_id: String = String(created.get(&"body_id"))
	_assert(String(session.active_body().get(&"body_id")) == moon_id, "created body was not selected")
	_assert(String(created.get(&"parent_body_id")) == asterra_id, "moon parent was not stored")
	session.select_body(asterra_id)
	_assert(not session.set_active_body_parent(moon_id), "celestial hierarchy accepted a parent cycle")
	_assert(String(session.active_body().get(&"parent_body_id")).is_empty(), "cycle rejection mutated the parent")

	var preset_path := "user://world_authoring/tests/phase1_roundtrip.tres"
	_assert(session.save_preset(preset_path) == OK, "preset save failed")
	session.revert()
	_assert(session.load_preset(preset_path) == OK, "preset load failed")
	bodies = session.staged_system.get(&"bodies")
	_assert(bodies.size() == 2, "preset did not round-trip body list")
	session.select_body(asterra_id)
	terrain = session.active_terrain_profile()
	water = session.active_water_profile()
	biome_layers = terrain.get(&"biome_override_layers")
	water_features = water.get(&"authored_features")
	var displacement_slots: Array = terrain.get(&"displacement_slots")
	var material_slots: Array = terrain.get(&"material_slots")
	_assert(biome_layers.size() == 1, "preset lost biome paint layers")
	_assert(displacement_slots.size() == 1, "preset lost displacement slots")
	_assert(material_slots.size() == 1, "preset lost material slots")
	_assert(water_features.size() == 2, "preset lost water features")
	_assert(int(terrain.call("sculpt_edited_tile_count")) == 1, "preset lost sparse sculpt tiles")
	var roundtrip_sculpt: Dictionary = terrain.call("sculpt_delta_serialized") as Dictionary
	_assert((roundtrip_sculpt.get("tiles") as PackedByteArray).size() == 64 * 64 * 4, "preset corrupted sparse sculpt payload")
	var roundtrip_graph: Resource = (displacement_slots[0] as Resource).get(&"graph") as Resource
	var roundtrip_links: Array = roundtrip_graph.get(&"links")
	_assert(roundtrip_links.size() == 1, "preset lost node graph links")
	session.apply()
	_assert(not session.dirty, "apply did not clear dirty state")

	var scene_resource: Resource = ResourceLoader.load("res://scenes/world_authoring/PlanetStudio.tscn")
	_assert(scene_resource is PackedScene, "PlanetStudio scene failed to load")
	print("WORLD_AUTHORING_PHASE0_OK")
	print("WORLD_AUTHORING_PHASE1_OK")
	print("WORLD_AUTHORING_PHASE2_SCULPT_OK")
	quit(0)

func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	push_error("WORLD_AUTHORING_PHASE0_FAILED: %s" % message)
	quit(1)
