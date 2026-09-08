class_name AtmosphereMedium
extends RefCounted
## The gas the player / a vehicle is moving through.
##
## For a gas giant this is the GasGiantModel envelope: drag rises with depth (a
## free fall reaches terminal velocity), buoyancy can float a light object, and a
## crush/heat boundary deep down shoves you back up. Everywhere else it is vacuum
## -- every query returns the no-effect value, so nothing changes on a normal
## planet or moon.

const GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")

## Drag coefficient x cross-section / mass for an unaugmented person (m^2/kg).
## Tuned so falling into a Jool-like envelope settles to tens of m/s over a few
## km -- not instant, not never.
const PERSON_BALLISTIC := 0.010
## Displaced-volume / mass (m^3/kg). A bare person is slightly buoyant in dense
## gas; an inflated "balloon" carry item floats high in the deck.
const PERSON_BUOYANCY := 0.016
const BALLOON_BUOYANCY := 0.10

var _model: GasGiantModel = null        ## null => vacuum, no effect
var _reference_radius_m: float = 0.0    ## the cloud tops -- the altitude datum
var _gravity_m_s2: float = 9.62
var _body_id: StringName = &""


## (Re)bind to the pool's current contact body. Cheap and idempotent when the
## contact body has not changed. `system` is the CelestialSystemDefinition (from
## main._system); pass null to force vacuum.
func refresh(bodies: Node, system: Resource) -> void:
	var contact: Object = bodies.get(&"contact_body") if bodies != null else null
	var id: StringName = StringName(contact.id) if contact != null else &""
	if id == _body_id:
		return
	_body_id = id
	_model = null
	_reference_radius_m = 0.0
	if contact == null or system == null:
		return
	var body: Resource = system.call("find_body", String(id))
	if body == null or not bool(body.call("is_gas_giant")):
		return
	_model = GENERATOR.gas_giant_model_for(body)
	_reference_radius_m = float(body.get(&"radius_m"))
	_gravity_m_s2 = maxf(float(contact.surface_gravity()), 0.01)


func active() -> bool:
	return _model != null


func reference_radius_m() -> float:
	return _reference_radius_m


## One-line HUD / status readout for the current contact body's envelope (GG5).
## Empty string in vacuum -- the caller falls back to its normal altitude line.
func readout(world_pos: Vec3D) -> String:
	if _model == null:
		return ""
	return _model.readout_at(world_pos.length())


## Altitude of a canonical position relative to the cloud tops (negative = inside
## the envelope).
func altitude_of(world_pos: Vec3D) -> float:
	return world_pos.length() - _reference_radius_m


## Gas density (kg/m^3) at `alt_m` above the cloud tops.
func density_at(alt_m: float) -> float:
	if _model == null:
		return 0.0
	return _model.density_at(_reference_radius_m + alt_m)


## 1.0 in vacuum / above the cloud tops, ramping toward a floor as the gas
## thickens -- for the kinematic FLY controller, which has no velocity for real
## drag to bite on.
func speed_factor(alt_m: float) -> float:
	if _model == null:
		return 1.0
	return clampf(1.0 / (1.0 + density_at(alt_m) * 6.0), 0.02, 1.0)


## Quadratic-drag deceleration magnitude (m/s^2) opposing motion at `speed_m_s`.
func drag_decel(speed_m_s: float, alt_m: float, ballistic: float = PERSON_BALLISTIC) -> float:
	if _model == null:
		return 0.0
	return 0.5 * density_at(alt_m) * speed_m_s * speed_m_s * ballistic


## Upward buoyant acceleration (m/s^2) for displaced-volume/mass `vm`.
func buoyancy_accel(alt_m: float, vm: float = PERSON_BUOYANCY) -> float:
	if _model == null:
		return 0.0
	return density_at(alt_m) * _gravity_m_s2 * vm


## True below the crush/heat boundary -- deeper than this the observer is pushed
## back up (and a HUD warning fires).
func lethal(alt_m: float) -> bool:
	return _model != null and (_reference_radius_m + alt_m) < _model.deadly_radius_m


## Altitude (relative to the cloud tops) of the crush boundary; huge negative when
## there is no gas-giant medium.
func lethal_altitude() -> float:
	return (_model.deadly_radius_m - _reference_radius_m) if _model != null else -1.0e30


## Altitude where buoyancy for `vm` balances gravity -- a bisection over the
## envelope. Returns lethal_altitude() when even the crush boundary is not dense
## enough to hold it up.
func neutral_altitude(vm: float = PERSON_BUOYANCY) -> float:
	if _model == null:
		return 0.0
	var hi: float = 0.0                                   ## cloud tops (thin)
	var lo: float = _model.core_radius_m - _reference_radius_m  ## core (dense)
	for _i in 40:
		var mid := (hi + lo) * 0.5
		if buoyancy_accel(mid, vm) < _gravity_m_s2:
			hi = mid                                     ## too thin -> go deeper
		else:
			lo = mid
	return maxf((hi + lo) * 0.5, lethal_altitude())
