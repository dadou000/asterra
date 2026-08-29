class_name AuthoringOrbitDefinition
extends Resource
## Persistent Keplerian orbit data for Planet Studio.
## Distances are centre-to-centre metres and angles are degrees.

@export var semi_major_axis_m: float = 0.0
@export_range(0.0, 0.999999, 0.000001) var eccentricity: float = 0.0
@export var inclination_deg: float = 0.0
@export var longitude_ascending_node_deg: float = 0.0
@export var argument_periapsis_deg: float = 0.0
@export var mean_anomaly_at_epoch_deg: float = 0.0
@export var epoch_s: float = 0.0

func periapsis_m() -> float:
	return semi_major_axis_m * (1.0 - eccentricity)

func apoapsis_m() -> float:
	return semi_major_axis_m * (1.0 + eccentricity)

func set_apsides(periapsis_m_value: float, apoapsis_m_value: float) -> void:
	var peri := maxf(0.0, periapsis_m_value)
	var apo := maxf(peri, apoapsis_m_value)
	var total := peri + apo
	if total <= 1e-9:
		semi_major_axis_m = 0.0
		eccentricity = 0.0
		return
	semi_major_axis_m = total * 0.5
	eccentricity = clampf((apo - peri) / total, 0.0, 0.999999)

func is_bound_orbit() -> bool:
	return semi_major_axis_m > 0.0 and eccentricity >= 0.0 and eccentricity < 1.0
