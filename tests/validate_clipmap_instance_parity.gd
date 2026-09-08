extends Node
## M1 verification: the visual-terrain stack can be built as a per-BodyRuntime
## instance whose GPU state (GPUPlanetContext.generation, its texture set) and
## terrain-edit store are fully independent of the resident autoloads -- while the
## autoload path itself is untouched (behaviour byte-identical to pre-M1).
##   godot --headless --path . res://tests/validate_clipmap_instance_parity.tscn

const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")
const PLANET_SAMPLER := preload("res://scripts/gen/planet_sampler_gpu_runtime.gd")
const CLIPMAP := preload("res://scripts/terrain/spherical_geometry_clipmap_phase31.gd")

var _failed := false


func _ready() -> void:
	await get_tree().process_frame

	# Baseline: the resident autoloads as the primary BodyRuntime wires them.
	var primary: BodyRuntime = Bodies.primary()
	var autoload_ctx_gen_before: int = int(PlanetContext.generation)
	var autoload_delta_tiles_before: int = Deltas.edited_tile_count()
	var frames_radius_before: float = Frames.planet_radius

	# --- A second body's own sampler + GPU context, from BodyRuntime factories ---
	var second: BodyRuntime = BodyRuntime.new(&"testworld")
	second.state = BodyRuntime.State.WARM

	var sampler: Node = PLANET_SAMPLER.new()
	sampler.name = "Sampler_testworld"
	add_child(sampler)
	second.sampler = sampler

	var ctx: Node = second.make_context(sampler)
	add_child(ctx)
	_assert(second.context == ctx, "make_context() stores the context on the runtime")
	_assert(int(ctx.generation) == 0, "a fresh secondary context starts at generation 0 (got %d)" % ctx.generation)

	# Bake a small world and adopt it on the SECOND sampler only.
	var cfg := GEN_CONFIG.new()
	cfg.face_res = 32
	cfg.erosion_iterations = 8
	cfg.world_seed = 0x5445535457524C44
	cfg.planet_radius = 640000.0
	var fields := PlanetBake.new(cfg).bake(Callable(), false)
	_assert(fields != null, "bake produced fields")

	sampler.adopt(fields)  # emits world_ready on THIS sampler -> only `ctx` listens
	# The context builds synchronously inside its _on_world_ready.
	_assert(int(ctx.generation) == 1,
		"the secondary context advanced its own generation on its sampler's world_ready (got %d)" % ctx.generation)
	_assert(bool(ctx.ready_state), "the secondary context finished building its texture set")
	_assert(ctx.soil_texture != null and ctx.biome_texture != null,
		"the secondary context holds its own Texture2DArray set")
	_assert(int(ctx.face_res) == fields.grid.res,
		"the secondary context face_res matches its own baked grid (%d vs %d)" % [ctx.face_res, fields.grid.res])

	# The resident autoload context must be completely unaffected.
	_assert(int(PlanetContext.generation) == autoload_ctx_gen_before,
		"the PlanetContext autoload generation is untouched (%d -> %d)" % [
			autoload_ctx_gen_before, PlanetContext.generation])

	# --- A second body's own terrain-edit store ---
	var store: Node = second.make_deltas()
	add_child(store)
	_assert(second.deltas == store, "make_deltas() stores the store on the runtime")
	_assert(store != Deltas, "the secondary delta store is a distinct object from the Deltas autoload")

	var lat: Array = store.dir_to_lattice(Vector3.RIGHT)
	store.add_offset(int(lat[0]), int(lat[1]), int(lat[2]), -3.5, -60.0, 60.0)
	_assert(store.edited_tile_count() == 1, "the secondary store recorded its own edit")
	_assert(Deltas.edited_tile_count() == autoload_delta_tiles_before,
		"the Deltas autoload is untouched by an edit on the secondary store (%d vs %d)" % [
			Deltas.edited_tile_count(), autoload_delta_tiles_before])

	# --- The clipmap script accepts a runtime binding without touching globals ---
	var clipmap: Node = CLIPMAP.new()  # not added to the tree -> no _ready / no GPU
	clipmap.bind_runtime(second)
	_assert(clipmap._planet() == sampler,
		"a runtime-bound clipmap resolves _planet() to that body's sampler, not the Planet autoload")
	_assert(clipmap._planet() != get_node_or_null(^"/root/Planet"),
		"a runtime-bound clipmap does not resolve to the Planet autoload")
	clipmap.free()

	# The autoload clipmap still points at the resident sampler.
	_assert(GroundGeometryClipmap._planet() == get_node_or_null(^"/root/Planet"),
		"the autoload clipmap still resolves _planet() to the Planet autoload")
	_assert(is_equal_approx(Frames.planet_radius, frames_radius_before)
		or is_equal_approx(Frames.planet_radius, cfg.planet_radius),
		"Frames.planet_radius only moved if the secondary sampler.adopt set it (expected -- documents the M3 coupling)")

	ctx.free()
	store.free()
	sampler.free()

	if _failed:
		get_tree().quit(1)
		return
	print("CLIPMAP_INSTANCE_PARITY_OK")
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("CLIPMAP_INSTANCE_PARITY_FAILED: %s" % message)
