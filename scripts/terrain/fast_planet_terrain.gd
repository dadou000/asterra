class_name FastPlanetTerrain
extends SparsePlanetTerrain
## Compatibility alias retained for the existing Phase-1 harness.
##
## The former FastPlanetTerrain CPU visual quadtree is removed. The autoload
## GroundGeometryClipmap now renders the complete visible planet as one spherical
## L0-L14 geometry clipmap from ~0.75 m spacing to the horizon/orbit cap.
##
## SparsePlanetTerrain owns only observer statistics and CPU collision streaming;
## neither class creates visual terrain meshes or launches ChunkBuilder work.
