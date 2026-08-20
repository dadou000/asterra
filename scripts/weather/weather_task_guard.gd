extends Node
## Keeps WeatherSystem worker jobs alive until the owning node has finished
## shutting down. WorkerThreadPool callables may still be executing when a scene
## is stopped/reloaded; without this barrier the callable can resume on a freed
## WeatherSystem instance and fail from inside _simulate_step().

var _tracked: Dictionary = {}

func _ready() -> void:
	get_tree().node_added.connect(_on_node_added)
	_scan_existing(get_tree().root)

func _scan_existing(node: Node) -> void:
	if node is WeatherSystem:
		_track_weather(node as WeatherSystem)
	for child in node.get_children():
		_scan_existing(child)

func _on_node_added(node: Node) -> void:
	if node is WeatherSystem:
		_track_weather(node as WeatherSystem)

func _track_weather(weather: WeatherSystem) -> void:
	var instance_id: int = weather.get_instance_id()
	if _tracked.has(instance_id):
		return
	_tracked[instance_id] = weakref(weather)
	weather.tree_exiting.connect(_on_weather_tree_exiting.bind(weather), CONNECT_ONE_SHOT)

func _on_weather_tree_exiting(weather: WeatherSystem) -> void:
	if not is_instance_valid(weather):
		return

	# Stop scheduling new simulation steps first. Incrementing the generation also
	# invalidates any deferred result that was produced by the old scene/world.
	weather.set_process(false)
	weather._generation += 1

	# The crucial part: do not allow the WeatherSystem object to be freed while
	# its worker lambda is still inside _simulate_step() / _build_images().
	var task_id: int = weather._task_id
	if task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(task_id)
		weather._task_id = -1
	weather._step_in_flight = false

	_tracked.erase(weather.get_instance_id())
