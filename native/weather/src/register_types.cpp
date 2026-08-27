#include "weather_native.h"
#include "weather_core_native.h"
#include "weather_dry_core_native.h"
#include "weather_wind_sampler.h"
#include <godot_cpp/godot.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void initialize_asterra_weather(ModuleInitializationLevel level) {
	if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
	GDREGISTER_CLASS(WeatherNative);
	GDREGISTER_CLASS(WeatherCoreNative);
	GDREGISTER_CLASS(WeatherDryCoreNative);
	GDREGISTER_CLASS(WeatherWindSampler);
}

void uninitialize_asterra_weather(ModuleInitializationLevel level) {
	if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
}

extern "C" {
GDExtensionBool GDE_EXPORT asterra_weather_library_init(
	GDExtensionInterfaceGetProcAddress get_proc_address,
	const GDExtensionClassLibraryPtr library,
	GDExtensionInitialization *initialization) {
	godot::GDExtensionBinding::InitObject init_obj(get_proc_address, library, initialization);
	init_obj.register_initializer(initialize_asterra_weather);
	init_obj.register_terminator(uninitialize_asterra_weather);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
}
