extends Node
## M0 verification for the BodyRuntime extraction. Launched via a .tscn so the
## project autoloads (Bodies, Planet, Frames, ...) resolve as they do at runtime.
##
## Asserts the primary BodyRuntime is wired to the resident terrain singletons and
## carries the same datum as `Planet` / `Frames` -- i.e. the extraction is a pure
## seam with no behaviour change.
##   godot --headless --path . res://tests/validate_body_runtime_pool.tscn

const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")

var _failed := false


func _ready() -> void:
	await get_tree().process_frame

	# A configured Planet is all M0 needs -- no bake.
	var cfg: Resource = GEN_CONFIG.new()
	cfg.planet_radius = 1234567.0
	Planet.configure(cfg)

	var primary: BodyRuntime = Bodies.primary()
	_assert(primary != null, "Bodies.primary() must return a runtime")
	_assert(Bodies.active == primary, "the primary runtime is active at startup")
	_assert(Bodies.contact_body == primary, "the primary runtime owns contact at startup")
	_assert(Bodies.slots.size() == 1, "M0 registers exactly one slot (got %d)" % Bodies.slots.size())
	_assert(primary.is_primary, "the startup runtime is flagged primary")
	_assert(primary.state == BodyRuntime.State.HOT, "the primary runtime is HOT")
	_assert(String(primary.id) == "asterra", "the standalone body id is 'asterra' (got %s)" % primary.id)

	# Facade identity: the runtime references the *same* singleton objects, so
	# every existing `Planet.*` / `Deltas.*` call site keeps meaning "the active
	# body" for free.
	_assert(primary.sampler == Planet, "primary.sampler is the Planet autoload")
	_assert(primary.context == PlanetContext, "primary.context is the PlanetContext autoload")
	_assert(primary.deltas == Deltas, "primary.deltas is the Deltas autoload")
	_assert(primary.clipmap == GroundGeometryClipmap, "primary.clipmap is the GroundGeometryClipmap autoload")
	_assert(primary.ocean == OceanSystem, "primary.ocean is the OceanSystem autoload")
	_assert(primary.sampler.cfg == Planet.cfg, "primary.sampler.cfg is Planet.cfg")

	# Datum agreement with the pre-extraction sources of truth.
	_assert(is_equal_approx(primary.radius(), Planet.cfg.planet_radius),
		"primary.radius() == Planet.cfg.planet_radius (%s vs %s)" % [primary.radius(), Planet.cfg.planet_radius])
	_assert(is_equal_approx(primary.radius(), Frames.planet_radius),
		"primary.radius() == Frames.planet_radius (%s vs %s)" % [primary.radius(), Frames.planet_radius])
	_assert(is_equal_approx(Bodies.active_radius(), Frames.planet_radius),
		"Bodies.active_radius() tracks Frames.planet_radius")

	# Idempotency: a second primary() call returns the same object, no new slot.
	var again: BodyRuntime = Bodies.primary()
	_assert(again == primary, "Bodies.primary() is idempotent")
	_assert(Bodies.slots.size() == 1, "a repeat primary() call adds no slot")

	# The active-swap seam exists and emits, even though M0 never uses it.
	_assert(Bodies.slot(&"asterra") == primary, "Bodies.slot() resolves the primary by id")
	_assert(Bodies.slot(&"nope") == null, "Bodies.slot() returns null for an unknown id")

	if _failed:
		get_tree().quit(1)
		return
	print("BODY_RUNTIME_POOL_OK")
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("BODY_RUNTIME_POOL_FAILED: %s" % message)
