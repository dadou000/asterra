from pathlib import Path


def replace_exact(text: str, old: str, new: str, expected: int, label: str) -> str:
    count = text.count(old)
    if count != expected:
        raise SystemExit(f"{label}: expected {expected}, found {count}")
    return text.replace(old, new)


# -----------------------------------------------------------------------------
# Native wind sampler: expose the selected local-nest layer as RGBAF u/v/speed.
# -----------------------------------------------------------------------------
p = Path("native/weather/src/weather_native.h")
s = p.read_text(encoding="utf-8")
s = replace_exact(
    s,
    "\tconst Atmosphere &get_global_atmosphere_cpp() const { return global_atm; }\n",
    "\tconst Atmosphere &get_global_atmosphere_cpp() const { return global_atm; }\n"
    "\tconst Atmosphere &get_local_atmosphere_cpp() const { return local_atm; }\n",
    1, "local atmosphere native accessor")
p.write_text(s, encoding="utf-8")

p = Path("native/weather/src/weather_wind_sampler.h")
s = p.read_text(encoding="utf-8")
s = replace_exact(
    s,
    "\tPackedFloat32Array get_global_wind_rgba(Object *weather, int layer) const;\n",
    "\tPackedFloat32Array get_global_wind_rgba(Object *weather, int layer) const;\n"
    "\tPackedFloat32Array get_local_wind_rgba(Object *weather, int layer) const;\n",
    1, "local wind sampler declaration")
p.write_text(s, encoding="utf-8")

p = Path("native/weather/src/weather_wind_sampler.cpp")
s = p.read_text(encoding="utf-8")
s = replace_exact(
    s,
    "\tClassDB::bind_method(\n"
    "\t\tD_METHOD(\"get_global_wind_rgba\", \"weather\", \"layer\"),\n"
    "\t\t&WeatherWindSampler::get_global_wind_rgba);\n",
    "\tClassDB::bind_method(\n"
    "\t\tD_METHOD(\"get_global_wind_rgba\", \"weather\", \"layer\"),\n"
    "\t\t&WeatherWindSampler::get_global_wind_rgba);\n"
    "\tClassDB::bind_method(\n"
    "\t\tD_METHOD(\"get_local_wind_rgba\", \"weather\", \"layer\"),\n"
    "\t\t&WeatherWindSampler::get_local_wind_rgba);\n",
    1, "local wind sampler binding")
insert = r'''

PackedFloat32Array WeatherWindSampler::get_local_wind_rgba(Object *weather, int layer) const {
	PackedFloat32Array out;
	const WeatherNative *native = Object::cast_to<WeatherNative>(weather);
	if (native == nullptr) return out;

	const WeatherNative::Atmosphere &a = native->get_local_atmosphere_cpp();
	if (a.cells != WeatherNative::LOCAL_W * WeatherNative::LOCAL_H) return out;
	const int selected = std::clamp(layer, 0, WeatherNative::LAYERS - 1);
	const int offset = a.layer_offset(selected);
	if (offset < 0 || size_t(offset + a.cells) > a.u.size()
			|| size_t(offset + a.cells) > a.v.size()) {
		return out;
	}

	out.resize(a.cells * 4);
	float *dst = out.ptrw();
	#pragma omp parallel for schedule(static)
	for (int c = 0; c < a.cells; ++c) {
		const float u = a.u[offset + c];
		const float v = a.v[offset + c];
		const float speed = std::sqrt(u * u + v * v);
		dst[c * 4 + 0] = u;
		dst[c * 4 + 1] = v;
		dst[c * 4 + 2] = speed;
		dst[c * 4 + 3] = 1.0f;
	}
	return out;
}
'''
s = replace_exact(
    s,
    "\n} // namespace godot\n",
    insert + "\n} // namespace godot\n",
    1, "local wind sampler implementation")
p.write_text(s, encoding="utf-8")


# -----------------------------------------------------------------------------
# Weather map: robust global pointer capture for warp + live-world cell vectors.
# -----------------------------------------------------------------------------
p = Path("scripts/ui/weather_map.gd")
s = p.read_text(encoding="utf-8")

s = replace_exact(
    s,
    "const FALLBACK_LAYER_SPEED := [1.000, 1.000, 1.000, 1.044, 1.097, 1.158, 1.228, 1.301, 1.383, 1.472, 1.552, 1.637, 1.728, 1.816, 1.899, 1.987, 2.080, 2.134, 2.189, 2.243, 2.298, 2.340, 2.340, 2.340, 2.340, 2.340, 2.340, 2.340, 2.340, 2.340]\n",
    "const FALLBACK_LAYER_SPEED := [1.000, 1.000, 1.000, 1.044, 1.097, 1.158, 1.228, 1.301, 1.383, 1.472, 1.552, 1.637, 1.728, 1.816, 1.899, 1.987, 2.080, 2.134, 2.189, 2.243, 2.298, 2.340, 2.340, 2.340, 2.340, 2.340, 2.340, 2.340, 2.340, 2.340]\n"
    "const LAYER_HEIGHT_M: Array[float] = [\n"
    "\t100.0, 250.0, 450.0, 700.0, 1000.0, 1350.0, 1750.0, 2200.0,\n"
    "\t2700.0, 3250.0, 3850.0, 4500.0, 5200.0, 5950.0, 6750.0, 7600.0,\n"
    "\t8500.0, 9400.0, 10300.0, 11200.0, 12100.0, 13000.0, 13900.0, 14800.0,\n"
    "\t15700.0, 16600.0, 17500.0, 18400.0, 19300.0, 20200.0,\n"
    "]\n",
    1, "layer height constants")

s = replace_exact(
    s,
    "const PARTICLE_COUNT := 32768\nconst PLANET_RADIUS_M := 3500000.0\n",
    "const PARTICLE_COUNT := 32768\n"
    "const LOCAL_W := 192\n"
    "const LOCAL_H := 192\n"
    "const LOCAL_CELL_M := 2200.0\n"
    "const WORLD_VECTOR_COUNT := LOCAL_W * LOCAL_H\n"
    "const PLANET_RADIUS_M := 3500000.0\n",
    1, "local vector grid constants")

s = replace_exact(
    s,
    "var _flow_check: CheckButton\nvar _source_label: Label\n",
    "var _flow_check: CheckButton\n"
    "var _world_vector_check: CheckButton\n"
    "var _source_label: Label\n",
    1, "world vector checkbox variable")

s = replace_exact(
    s,
    "var _wind_sampler: Object\n\nvar _panel: PanelContainer\n",
    "var _wind_sampler: Object\n"
    "var _world_vector_root: Node3D\n"
    "var _world_vector_instances: MultiMeshInstance3D\n"
    "var _world_vector_material: ShaderMaterial\n"
    "var _world_vector_texture: ImageTexture\n"
    "var _world_vectors_enabled := false\n"
    "var _world_vector_last_revision := -1\n"
    "var _world_vector_last_layer := -1\n\n"
    "var _panel: PanelContainer\n",
    1, "world vector runtime variables")

s = replace_exact(
    s,
    "\t_panel.offset_top = -324.0\n",
    "\t_panel.offset_top = -360.0\n",
    1, "panel height for vector toggle")

s = replace_exact(
    s,
    "\t_flow_check.toggled.connect(_on_flow_toggled)\n\tcolumn.add_child(_flow_check)\n\n\tvar warp_row := HBoxContainer.new()\n",
    "\t_flow_check.toggled.connect(_on_flow_toggled)\n"
    "\tcolumn.add_child(_flow_check)\n\n"
    "\t_world_vector_check = CheckButton.new()\n"
    "\t_world_vector_check.text = \"World cell wind vectors\"\n"
    "\t_world_vector_check.button_pressed = false\n"
    "\t_world_vector_check.tooltip_text = \"Draw one wind-vector arrow for every 2.2 km cell in the live 192×192 local nest. The selected Height layer controls the arrows.\"\n"
    "\t_world_vector_check.toggled.connect(_on_world_vectors_toggled)\n"
    "\tcolumn.add_child(_world_vector_check)\n\n"
    "\tvar warp_row := HBoxContainer.new()\n",
    1, "world vector toggle UI")

s = replace_exact(
    s,
    "\t_speed_slider.value_changed.connect(_on_speed_slider_changed)\n"
    "\t_speed_slider.drag_started.connect(_on_speed_slider_drag_started)\n"
    "\t_speed_slider.drag_ended.connect(_on_speed_slider_drag_ended)\n",
    "\t_speed_slider.value_changed.connect(_on_speed_slider_changed)\n",
    1, "warp native drag signal removal")

s = replace_exact(
    s,
    "func _process(delta: float) -> void:\n"
    "\tif not visible:\n"
    "\t\treturn\n",
    "func _process(delta: float) -> void:\n"
    "\tif _speed_slider_dragging and not Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT):\n"
    "\t\t_speed_slider_dragging = false\n"
    "\tif _world_vectors_enabled:\n"
    "\t\t_service_world_vectors()\n"
    "\tif not visible:\n"
    "\t\treturn\n",
    1, "world vectors process while map closed")

old_input_start = '''func _input(event: InputEvent) -> void:
\tif not visible:
\t\treturn
\tif event is InputEventMouseButton:
\t\tvar over_panel := _panel != null and _panel.get_global_rect().has_point(event.position)
\t\tif event.pressed and over_panel:
\t\t\treturn
'''
new_input_start = '''func _input(event: InputEvent) -> void:
\tif not visible:
\t\treturn
\tif event is InputEventMouseButton:
\t\t# Capture the warp pointer ourselves. Godot's Range drag can stop receiving
\t\t# motion once the pointer leaves the knob/control; this capture keeps the
\t\t# slider live anywhere on the window until LMB is released.
\t\tif event.button_index == MOUSE_BUTTON_LEFT:
\t\t\tif event.pressed and _speed_slider_contains(event.position):
\t\t\t\t_speed_slider_dragging = true
\t\t\t\t_speed_slider.grab_focus()
\t\t\t\t_set_speed_slider_from_pointer(event.position.x)
\t\t\t\tget_viewport().set_input_as_handled()
\t\t\t\treturn
\t\t\telif not event.pressed and _speed_slider_dragging:
\t\t\t\t_set_speed_slider_from_pointer(event.position.x)
\t\t\t\t_speed_slider_dragging = false
\t\t\t\tget_viewport().set_input_as_handled()
\t\t\t\treturn
\t\tvar over_panel := _panel != null and _panel.get_global_rect().has_point(event.position)
\t\tif event.pressed and over_panel:
\t\t\treturn
'''
s = replace_exact(s, old_input_start, new_input_start, 1, "warp pointer capture mouse button")

s = replace_exact(
    s,
    "\telif event is InputEventMouseMotion and _dragging:\n"
    "\t\tvar sensitivity := 0.0060 * clampf(_camera_distance / 2.7, 0.34, 1.25)\n",
    "\telif event is InputEventMouseMotion and _speed_slider_dragging:\n"
    "\t\t_set_speed_slider_from_pointer(event.position.x)\n"
    "\t\tget_viewport().set_input_as_handled()\n"
    "\telif event is InputEventMouseMotion and _dragging:\n"
    "\t\tvar sensitivity := 0.0060 * clampf(_camera_distance / 2.7, 0.34, 1.25)\n",
    1, "warp pointer capture mouse motion")

s = replace_exact(
    s,
    "\tvisible = opened\n\t_dragging = false\n",
    "\tvisible = opened\n\t_dragging = false\n\t_speed_slider_dragging = false\n",
    1, "clear warp capture on open close")

s = replace_exact(
    s,
    "func _on_layer_selected(index: int) -> void:\n"
    "\twind_layer = clampi(index, 0, LAYER_NAMES.size() - 1)\n"
    "\t_last_wind_revision = -1\n",
    "func _on_layer_selected(index: int) -> void:\n"
    "\twind_layer = clampi(index, 0, LAYER_NAMES.size() - 1)\n"
    "\t_last_wind_revision = -1\n"
    "\t_world_vector_last_revision = -1\n"
    "\t_world_vector_last_layer = -1\n"
    "\t_update_world_vector_geometry_uniforms()\n",
    1, "world vectors follow selected layer")

s = replace_exact(
    s,
    "func _on_flow_toggled(enabled: bool) -> void:\n"
    "\tif _wind_particles != null:\n"
    "\t\t_wind_particles.visible = enabled\n\n\nfunc _update_product() -> void:\n",
    r'''func _on_flow_toggled(enabled: bool) -> void:
\tif _wind_particles != null:
\t\t_wind_particles.visible = enabled


func _on_world_vectors_toggled(enabled: bool) -> void:
\t_world_vectors_enabled = enabled
\tif enabled:
\t\t_ensure_world_vector_overlay()
\t\t_world_vector_last_revision = -1
\t\t_world_vector_last_layer = -1
\t\t_service_world_vectors()
\telif _world_vector_root != null and is_instance_valid(_world_vector_root):
\t\t_world_vector_root.visible = false


func _make_world_vector_arrow_mesh() -> ArrayMesh:
\tvar mesh := ArrayMesh.new()
\tvar arrays := []
\tarrays.resize(Mesh.ARRAY_MAX)
\t# Unit arrow along +X. The shader scales X to vector magnitude and Y to a
\t# fixed fraction of one 2.2 km weather cell.
\tarrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
\t\tVector3(0.00, -0.09, 0.0), Vector3(0.66, -0.09, 0.0),
\t\tVector3(0.66,  0.09, 0.0), Vector3(0.00,  0.09, 0.0),
\t\tVector3(0.57, -0.30, 0.0), Vector3(1.00,  0.00, 0.0),
\t\tVector3(0.57,  0.30, 0.0),
\t])
\tarrays[Mesh.ARRAY_INDEX] = PackedInt32Array([
\t\t0, 1, 2, 0, 2, 3,
\t\t4, 5, 6,
\t])
\tmesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
\treturn mesh


func _ensure_world_vector_overlay() -> void:
\tvar scene := get_tree().current_scene
\tif scene == null:
\t\treturn
\tif _world_vector_root != null and is_instance_valid(_world_vector_root):
\t\tif _world_vector_root.get_parent() == scene:
\t\t\treturn
\t\t_world_vector_root.queue_free()

\t_world_vector_root = Node3D.new()
\t_world_vector_root.name = "WeatherWindVectorDebug"
\tscene.add_child(_world_vector_root)

\t_world_vector_instances = MultiMeshInstance3D.new()
\t_world_vector_instances.name = "CellVectors"
\t_world_vector_instances.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
\t_world_vector_root.add_child(_world_vector_instances)

\t_world_vector_material = ShaderMaterial.new()
\t_world_vector_material.shader = load("res://shaders/weather_world_wind_vectors.gdshader")
\tvar arrow := _make_world_vector_arrow_mesh()
\tarrow.surface_set_material(0, _world_vector_material)

\tvar multimesh := MultiMesh.new()
\tmultimesh.transform_format = MultiMesh.TRANSFORM_3D
\tmultimesh.use_custom_data = true
\tmultimesh.mesh = arrow
\tmultimesh.instance_count = WORLD_VECTOR_COUNT
\t# Shader displacement covers the complete ~422 km curved local nest.
\tmultimesh.custom_aabb = AABB(Vector3(-270000.0, -270000.0, -270000.0), Vector3(540000.0, 540000.0, 540000.0))
\tfor y in LOCAL_H:
\t\tfor x in LOCAL_W:
\t\t\tvar i := x + y * LOCAL_W
\t\t\tmultimesh.set_instance_transform(i, Transform3D.IDENTITY)
\t\t\tmultimesh.set_instance_custom_data(i, Color(
\t\t\t\t(float(x) + 0.5) / float(LOCAL_W),
\t\t\t\t(float(y) + 0.5) / float(LOCAL_H), 0.0, 1.0))
\t_world_vector_instances.multimesh = multimesh

\tvar neutral := Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF)
\tneutral.fill(Color(0.0, 0.0, 0.0, 1.0))
\t_world_vector_texture = ImageTexture.create_from_image(neutral)
\t_world_vector_material.set_shader_parameter("u_local_wind", _world_vector_texture)
\t_world_vector_material.set_shader_parameter("u_cell_size_m", LOCAL_CELL_M)
\t_update_world_vector_geometry_uniforms()


func _update_world_vector_geometry_uniforms() -> void:
\tif _world_vector_material == null:
\t\treturn
\tvar center := WeatherSystem.local_center
\tif center.length_squared() < 0.5:
\t\tcenter = Vector3.UP
\tcenter = center.normalized()
\tvar east := WeatherSystem.local_east.normalized()
\tvar north := WeatherSystem.local_north.normalized()
\tvar height := LAYER_HEIGHT_M[clampi(wind_layer, 0, LAYER_HEIGHT_M.size() - 1)]
\t_world_vector_material.set_shader_parameter("u_center_dir", center)
\t_world_vector_material.set_shader_parameter("u_local_east", east)
\t_world_vector_material.set_shader_parameter("u_local_north", north)
\t_world_vector_material.set_shader_parameter("u_planet_radius_m", PLANET_RADIUS_M)
\t_world_vector_material.set_shader_parameter("u_local_span_m", WeatherSystem.local_span_m)
\t_world_vector_material.set_shader_parameter("u_layer_height_m", height)
\tif _world_vector_root != null and is_instance_valid(_world_vector_root):
\t\t# Keep the GPU geometry in the live world's floating-origin frame. Cell
\t\t# offsets remain small floats in the shader while the anchor is rebased here.
\t\t_world_vector_root.global_position = Frames.to_render(
\t\t\tVec3D.from_v3(center * (PLANET_RADIUS_M + height + 35.0)))


func _service_world_vectors() -> void:
\t_ensure_world_vector_overlay()
\tif _world_vector_root == null or not is_instance_valid(_world_vector_root):
\t\treturn
\tvar local_active := WeatherSystem.simulation_speed <= WeatherSystem.HIGH_WARP_LOCAL_THRESHOLD
\t_world_vector_root.visible = _world_vectors_enabled and local_active
\tif not _world_vector_root.visible:
\t\treturn
\t_update_world_vector_geometry_uniforms()
\tif WeatherSystem.native_worker_busy():
\t\treturn
\tif _world_vector_last_revision == WeatherSystem.local_state_revision \
\t\t\tand _world_vector_last_layer == wind_layer:
\t\treturn
\t_ensure_wind_sampler()
\tvar native: Variant = WeatherSystem.get("_native")
\tif _wind_sampler == null or native == null or not (native is Object):
\t\treturn
\tif not _wind_sampler.has_method(&"get_local_wind_rgba"):
\t\treturn
\tvar result: Variant = _wind_sampler.call(&"get_local_wind_rgba", native, wind_layer)
\tif not (result is PackedFloat32Array):
\t\treturn
\tvar values: PackedFloat32Array = result
\tif values.size() != WORLD_VECTOR_COUNT * 4:
\t\treturn
\tvar image := Image.create_from_data(
\t\tLOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF, values.to_byte_array())
\tif _world_vector_texture == null:
\t\t_world_vector_texture = ImageTexture.create_from_image(image)
\telse:
\t\t_world_vector_texture.update(image)
\t_world_vector_material.set_shader_parameter("u_local_wind", _world_vector_texture)
\t_world_vector_last_revision = WeatherSystem.local_state_revision
\t_world_vector_last_layer = wind_layer


func _update_product() -> void:
''',
    1, "world vector functions")

# Replace the broken release-only warp behavior with continuous updates and a
# pointer-to-range mapping that continues when the mouse leaves the control.
start = s.index("func _on_speed_slider_drag_started() -> void:")
end = s.index("func _update_speed_label(value: float) -> void:", start)
new_warp = r'''func _speed_slider_contains(point: Vector2) -> bool:
\treturn _speed_slider != null and _speed_slider.get_global_rect().has_point(point)


func _set_speed_slider_from_pointer(global_x: float) -> void:
\tif _speed_slider == null:
\t\treturn
\tvar rect := _speed_slider.get_global_rect()
\tif rect.size.x <= 1.0:
\t\treturn
\tvar t := clampf((global_x - rect.position.x) / rect.size.x, 0.0, 1.0)
\tvar raw := lerpf(_speed_slider.min_value, _speed_slider.max_value, t)
\tvar snapped := round(raw / _speed_slider.step) * _speed_slider.step
\t_speed_slider.value = clampf(snapped, _speed_slider.min_value, _speed_slider.max_value)


func _on_speed_slider_changed(slider_value: float) -> void:
\t# Apply every step immediately. The previous implementation only committed on
\t# drag_ended, which made the control appear broken and could miss a release
\t# after the pointer left the knob.
\tvar speed := _slider_to_speed(slider_value)
\tWeatherSystem.set_simulation_speed(speed)
\t_update_speed_label(speed)


func _on_external_speed_changed(value: float) -> void:
\tif _speed_slider != null and not _speed_slider_dragging:
\t\t_speed_slider.set_value_no_signal(_speed_to_slider(value))
\t_update_speed_label(value)


func _step_warp(steps: int) -> void:
\tvar slider_value := clampf(
\t\t_speed_to_slider(WeatherSystem.simulation_speed) + float(steps),
\t\t0.0, float(WARP_SPEEDS.size() - 1))
\tif _speed_slider != null:
\t\t_speed_slider.value = slider_value
\telse:
\t\tWeatherSystem.set_simulation_speed(_slider_to_speed(slider_value))


func _slider_to_speed(slider_value: float) -> float:
\tvar index := clampi(int(round(slider_value)), 0, WARP_SPEEDS.size() - 1)
\treturn WARP_SPEEDS[index]


func _speed_to_slider(speed: float) -> float:
\tif speed <= 0.01:
\t\treturn 0.0
\tvar best_index := 1
\tvar best_error := 1.0e30
\tfor i in range(1, WARP_SPEEDS.size()):
\t\tvar error := absf(log(maxf(speed, 0.001)) - log(WARP_SPEEDS[i]))
\t\tif error < best_error:
\t\t\tbest_error = error
\t\t\tbest_index = i
\treturn float(best_index)


'''
s = s[:start] + new_warp + s[end:]

p.write_text(s, encoding="utf-8")

print("weather world-vector + warp patch applied")
