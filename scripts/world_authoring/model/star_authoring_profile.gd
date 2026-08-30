class_name StarAuthoringProfile
extends Resource
## Persistent visual/physical authoring controls for stellar bodies.
##
## This resource is intentionally independent from the terrestrial planet profile:
## stars are first-class celestial definitions and can be edited/saved even while
## the current one-planet terrain runtime cannot yet preview their photospheres.

enum SpectralClass {
	O,
	B,
	A,
	F,
	G,
	K,
	M,
	CUSTOM,
}

@export_enum("O", "B", "A", "F", "G", "K", "M", "Custom") var spectral_class: int = SpectralClass.G
@export var effective_temperature_k: float = 5772.0
@export var luminosity_solar: float = 1.0
@export var metallicity_dex: float = 0.0

@export var photosphere_color: Color = Color(1.0, 0.93, 0.82, 1.0)
@export var photosphere_intensity: float = 1.0
@export var limb_darkening: float = 0.55
@export var granulation_scale: float = 1.0
@export var granulation_contrast: float = 0.22
@export var granulation_speed: float = 1.0

@export var sunspot_coverage: float = 0.002
@export var sunspot_scale: float = 1.0
@export var sunspot_contrast: float = 0.65
@export var facula_strength: float = 0.25
@export var differential_rotation: float = 0.20

@export var flare_activity: float = 0.15
@export var flare_frequency: float = 1.0
@export var flare_energy_scale: float = 1.0
@export var prominence_activity: float = 0.25

@export var corona_color: Color = Color(0.72, 0.84, 1.0, 1.0)
@export var corona_intensity: float = 1.0
@export var corona_extent_radii: float = 2.5
@export var solar_wind_strength: float = 1.0

@export var light_color: Color = Color(1.0, 0.956, 0.89, 1.0)
@export var light_energy: float = 1.0
@export var angular_light_radius_deg: float = 0.27

@export var variability_amplitude: float = 0.001
@export var variability_period_s: float = 0.0
@export_file("*.gdshader") var surface_shader_path: String = ""

func ensure_valid() -> void:
	spectral_class = clampi(spectral_class, SpectralClass.O, SpectralClass.CUSTOM)
	effective_temperature_k = clampf(effective_temperature_k, 500.0, 100000.0)
	luminosity_solar = maxf(luminosity_solar, 0.0)
	metallicity_dex = clampf(metallicity_dex, -8.0, 3.0)
	photosphere_intensity = maxf(photosphere_intensity, 0.0)
	limb_darkening = clampf(limb_darkening, 0.0, 1.0)
	granulation_scale = maxf(granulation_scale, 0.001)
	granulation_contrast = clampf(granulation_contrast, 0.0, 4.0)
	granulation_speed = maxf(granulation_speed, 0.0)
	sunspot_coverage = clampf(sunspot_coverage, 0.0, 1.0)
	sunspot_scale = maxf(sunspot_scale, 0.001)
	sunspot_contrast = clampf(sunspot_contrast, 0.0, 1.0)
	facula_strength = maxf(facula_strength, 0.0)
	differential_rotation = clampf(differential_rotation, 0.0, 1.0)
	flare_activity = maxf(flare_activity, 0.0)
	flare_frequency = maxf(flare_frequency, 0.0)
	flare_energy_scale = maxf(flare_energy_scale, 0.0)
	prominence_activity = maxf(prominence_activity, 0.0)
	corona_intensity = maxf(corona_intensity, 0.0)
	corona_extent_radii = maxf(corona_extent_radii, 1.0)
	solar_wind_strength = maxf(solar_wind_strength, 0.0)
	light_energy = maxf(light_energy, 0.0)
	angular_light_radius_deg = clampf(angular_light_radius_deg, 0.0, 45.0)
	variability_amplitude = maxf(variability_amplitude, 0.0)
	variability_period_s = maxf(variability_period_s, 0.0)
