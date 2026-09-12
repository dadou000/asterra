extends Node
## CPU-only regression gate for frontier failure ownership semantics.
##
## A coarse preseed alone is exactly reversible through the persistent ownership
## store. The first successful source-edge handoff changes that: source GPU state
## has already been debited and no reverse handoff exists, so the destination must
## remain solver-visible even if a later handoff fails.

var _failures: Array[String] = []


func _ready() -> void:
	_expect(not HydroFrontierActivationPipeline.owns_irreversible_frontier_water(-3),
		"negative handoff count must be treated as reversible")
	_expect(not HydroFrontierActivationPipeline.owns_irreversible_frontier_water(0),
		"coarse-only/zero-edge destination must be restorable and cancellable")
	_expect(HydroFrontierActivationPipeline.owns_irreversible_frontier_water(1),
		"one successful edge transfer must force destination preservation")
	_expect(HydroFrontierActivationPipeline.owns_irreversible_frontier_water(2),
		"multiple successful edge transfers must force destination preservation")
	_expect(HydroFrontierActivationPipeline.owns_irreversible_frontier_water(1024),
		"large successful edge count must remain preservation-required")
	_finish()


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _finish() -> void:
	if _failures.is_empty():
		print("HYDRO_FRONTIER_FAILURE_POLICY: PASS")
		get_tree().quit(0)
		return
	for failure in _failures:
		push_error("HYDRO_FRONTIER_FAILURE_POLICY: " + failure)
	get_tree().quit(1)
