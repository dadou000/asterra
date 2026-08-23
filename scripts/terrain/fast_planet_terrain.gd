class_name FastPlanetTerrain
extends SparsePlanetTerrain
## Compatibility alias retained for the existing Phase-1 harness.
##
## The former FastPlanetTerrain implementation was the CPU-heavy visual quadtree:
## it continuously launched ChunkBuilder workers, rebuilt normals/topology, and
## uploaded unique meshes while the player moved. That runtime path is removed.
##
## SparsePlanetTerrain now owns the visual planet. It uses one immutable GPU grid
## per cube face plus the camera-local sparse-page geometry clipmap. Physics keeps
## its independent TerrainCollisionStreamer and terrain editing keeps Deltas.
