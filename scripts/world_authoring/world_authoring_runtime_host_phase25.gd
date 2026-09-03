extends "res://scripts/world_authoring/world_authoring_runtime_host_phase24.gd"
## Phase 25 keeps Phase 24's persistent root-detail / multi-body preview rules.
##
## Editor construction deliberately lives only in WorldAuthoringRuntimeHost. Main
## references this subclass, and a second editor factory here once drifted to an
## old authoring phase. Inheriting the one factory prevents the runtime and the
## packed PlanetStudio scene from selecting different editor generations again.
