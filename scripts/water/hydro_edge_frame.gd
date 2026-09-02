class_name HydroEdgeFrame
extends RefCounted
## Discrete tangent-frame transforms for same-level hydrology tile boundaries.
##
## Solver momentum is stored in each tile's local +u/+v tangent frame. Across a
## cube seam, destination (hu,hv) must be expressed in the source frame before the
## Riemann problem is solved. HydroTileTopology supplies the destination boundary
## and whether increasing edge index agrees or reverses.


static func edge_normal(direction: int) -> Vector2:
	match direction:
		HydroTileTopology.DIR_WEST: return Vector2(-1.0, 0.0)
		HydroTileTopology.DIR_EAST: return Vector2(1.0, 0.0)
		HydroTileTopology.DIR_SOUTH: return Vector2(0.0, -1.0)
		HydroTileTopology.DIR_NORTH: return Vector2(0.0, 1.0)
	return Vector2.ZERO


## Tangent points along increasing edge cell index: +v on W/E and +u on S/N.
static func edge_tangent(direction: int) -> Vector2:
	match direction:
		HydroTileTopology.DIR_WEST, HydroTileTopology.DIR_EAST:
			return Vector2(0.0, 1.0)
		HydroTileTopology.DIR_SOUTH, HydroTileTopology.DIR_NORTH:
			return Vector2(1.0, 0.0)
	return Vector2.ZERO


static func momentum_to_source(momentum_destination: Vector2,
		source_direction: int, destination_direction: int,
		edge_orientation: int) -> Vector2:
	var ns := edge_normal(source_direction)
	var ts := edge_tangent(source_direction)
	var nd := edge_normal(destination_direction)
	var td := edge_tangent(destination_direction)
	if ns == Vector2.ZERO or ts == Vector2.ZERO or nd == Vector2.ZERO or td == Vector2.ZERO:
		return Vector2.ZERO
	var qn := momentum_destination.dot(nd)
	var qt := momentum_destination.dot(td)
	var orientation := 1.0 if edge_orientation >= 0 else -1.0
	# Destination outward normal is physically -source outward normal.
	return (-qn) * ns + (orientation * qt) * ts


static func momentum_across_link(momentum_destination: Vector2,
		source_direction: int, topology_link: Dictionary) -> Vector2:
	if topology_link.is_empty():
		return Vector2.ZERO
	return momentum_to_source(momentum_destination, source_direction,
		int(topology_link.get("destination_direction", -1)),
		int(topology_link.get("edge_orientation", 1)))
