class_name PlanetBake
extends RefCounted
## Orchestrates the Phase 1 generation pipeline and caches the result.
##
## Order matters and is the roadmap's own dependency order:
##   macro geography -> geology -> provisional climate -> erosion -> hydrology
##   -> final climate -> soil -> biomes -> infrastructure suitability
##
## Climate runs twice on purpose: erosion needs to know where it rains, and the
## final climate needs the eroded mountains to cast their rain shadows.

signal finished(fields: PlanetFields)

var cfg: GenConfig
var fields: PlanetFields
var grid: PlanetGrid

var _thread: Thread
var _mutex := Mutex.new()
var _stage: String = ""
var _fraction: float = 0.0
var _stage_index: int = 0
var _done: bool = false

const STAGES := 9

func _init(p_cfg: GenConfig) -> void:
	cfg = p_cfg

func cache_path() -> String:
	return "user://asterra/worlds/%d_%s.bake" % [cfg.world_seed, cfg.cache_key()]

## Synchronous bake. `progress` is called as progress.call(stage_name, 0..1).
func bake(progress: Callable = Callable(), use_cache: bool = true) -> PlanetFields:
	if use_cache:
		var cached := PlanetFields.load_from(cache_path(), cfg)
		if cached != null:
			fields = cached
			grid = cached.grid
			_done = true
			return fields

	grid = PlanetGrid.new(cfg.face_res, cfg.planet_radius)
	fields = PlanetFields.new(cfg, grid)
	var router := FlowRouter.new(grid)

	_stage_index = 0
	_run_stage(progress, "Macro geography", func(p): PassMacro.new(fields).run(p))
	_run_stage(progress, "Geology", func(p): PassGeology.new(fields).run(p))
	_run_stage(progress, "Provisional climate", func(p): PassClimate.new(fields).run(p))
	_run_stage(progress, "Erosion", func(p): PassErosion.new(fields, router).run(p))
	_run_stage(progress, "Hydrology", func(p): PassHydrology.new(fields, router).run(p))
	_run_stage(progress, "Climate", func(p): PassClimate.new(fields).run(p))
	_run_stage(progress, "Soil", func(p): PassSoil.new(fields).run(p))
	_run_stage(progress, "Biomes", func(p): PassBiome.new(fields).run(p))
	_run_stage(progress, "Infrastructure suitability", func(p): PassSuitability.new(fields).run(p))

	fields.save_to(cache_path())
	_done = true
	return fields

func _run_stage(progress: Callable, name: String, body: Callable) -> void:
	var idx := _stage_index
	var inner := func(sub_name: String, frac: float) -> void:
		_mutex.lock()
		_stage = sub_name
		_fraction = (float(idx) + clampf(frac, 0.0, 1.0)) / float(STAGES)
		_mutex.unlock()
		if progress.is_valid():
			progress.call(sub_name, (float(idx) + clampf(frac, 0.0, 1.0)) / float(STAGES))
	inner.call(name, 0.0)
	body.call(inner)
	_stage_index += 1

## Background bake. Poll status() from _process and connect to `finished`.
func bake_async(use_cache: bool = true) -> void:
	_thread = Thread.new()
	_thread.start(func():
		var f := bake(Callable(), use_cache)
		call_deferred("_emit_finished", f))

func _emit_finished(f: PlanetFields) -> void:
	if _thread != null and _thread.is_started():
		_thread.wait_to_finish()
		_thread = null
	finished.emit(f)

func status() -> Dictionary:
	_mutex.lock()
	var r := {"stage": _stage, "fraction": _fraction, "done": _done}
	_mutex.unlock()
	return r
