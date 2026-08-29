class_name AtmosphereAuthoringProfile
extends Resource
## Authoring-side atmosphere values. Runtime binding arrives in Phase 6.

@export var enabled: bool = true
@export var atmosphere_height_m: float = 60000.0
@export var rayleigh_strength: float = 1.0
@export var mie_strength: float = 1.0
@export var ozone_strength: float = 1.0
@export var aerosol_density: float = 1.0
@export var ground_haze: float = 1.0
@export var sky_tint: Color = Color(0.52, 0.67, 1.0, 1.0)
@export var horizon_tint: Color = Color(0.78, 0.84, 0.92, 1.0)
@export var cloud_coverage: float = 0.45
@export var cloud_density: float = 1.0
@export var cloud_altitude_m: float = 2200.0
