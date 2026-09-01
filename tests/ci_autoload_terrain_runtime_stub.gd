extends Node
## Compile-only stand-in for GPU/contact autoloads in the isolated script project.
##
## Planet Studio's live editor references TerrainContactSampler, whose production
## implementation addresses several global GPU query nodes by autoload name. The
## numerical tests do not call those paths, but the names must exist for GDScript
## compilation. Keeping this stub behaviorless ensures the CI test cannot
## accidentally pretend to validate GPU/contact behavior it does not exercise.
##
## Phase 27 is preloaded here intentionally because this stub is always loaded by
## the isolated script project. That gives the level-centric shader composer a hard
## Godot 4.7.1 parser/type-check gate even though Main.tscn is not copied into that
## isolated project.

const SHADER_COMPOSER_PHASE27 := preload(
	"res://scripts/world_authoring/world_authoring_editor_live_phase27.gd")
