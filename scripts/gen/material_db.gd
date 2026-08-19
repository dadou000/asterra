class_name MaterialDB
extends RefCounted
## Minimal material database (a Phase 0 core service, used here by Phase 1).
##
## Excavated ground has to become *something physical*: a volume of material with
## a density, a bulking factor and an angle of repose. Those three numbers are
## what turn "I dug a hole" into "I now own 14 m^3 of loose silt that will not
## stand steeper than 34 degrees".

const SOIL_O := 0
const SOIL_A := 1
const SOIL_B := 2
const SOIL_C := 3
const ROCK := 4
const SAND := 5
const GRAVEL := 6
const CLAY := 7

## id -> { name, density (kg/m3 in place), swell (loose volume / in-place volume),
##         repose (deg), cohesion (kPa), diggability (0..1, 1 = hand shovel) }
const ENTRIES := {
	0: {"name": "Organic topsoil", "density": 1150.0, "swell": 1.28, "repose": 30.0, "cohesion": 6.0, "diggability": 1.00},
	1: {"name": "Topsoil", "density": 1400.0, "swell": 1.25, "repose": 33.0, "cohesion": 10.0, "diggability": 0.92},
	2: {"name": "Subsoil", "density": 1650.0, "swell": 1.22, "repose": 35.0, "cohesion": 18.0, "diggability": 0.72},
	3: {"name": "Weathered parent material", "density": 1850.0, "swell": 1.30, "repose": 37.0, "cohesion": 26.0, "diggability": 0.48},
	4: {"name": "Bedrock", "density": 2650.0, "swell": 1.55, "repose": 42.0, "cohesion": 900.0, "diggability": 0.06},
	5: {"name": "Sand", "density": 1600.0, "swell": 1.12, "repose": 31.0, "cohesion": 0.0, "diggability": 1.00},
	6: {"name": "Gravel", "density": 1900.0, "swell": 1.14, "repose": 38.0, "cohesion": 0.0, "diggability": 0.85},
	7: {"name": "Clay", "density": 1750.0, "swell": 1.35, "repose": 27.0, "cohesion": 45.0, "diggability": 0.62},
}

static func get_entry(id: int) -> Dictionary:
	return ENTRIES.get(id, ENTRIES[4])

static func display_name(id: int, rock_family: int = -1) -> String:
	if id == ROCK and rock_family >= 0:
		return PlanetFields.ROCK_NAMES[rock_family]
	return ENTRIES[id]["name"]

## Blend a soil horizon's nominal properties with its actual texture, so a clayey
## B horizon behaves differently from a sandy one.
static func with_texture(id: int, sand: float, silt: float, clay: float, organic: float) -> Dictionary:
	var e := get_entry(id).duplicate()
	if id > SOIL_C:
		return e
	e["density"] = lerpf(e["density"], 1600.0, sand) * (1.0 - organic * 0.25)
	e["repose"] = lerpf(e["repose"], 31.0, sand)
	e["repose"] = lerpf(e["repose"], 27.0, clay)
	e["cohesion"] = e["cohesion"] * (0.25 + clay * 2.6 + organic * 0.8) + silt * 3.0
	e["swell"] = lerpf(e["swell"], 1.35, clay)
	e["diggability"] = clampf(e["diggability"] * (1.15 - clay * 0.35), 0.02, 1.0)
	return e
