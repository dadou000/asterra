extends Node
## Compile-only stand-in for GPU/contact autoloads in the isolated script project.
##
## Planet Studio's live editor references TerrainContactSampler, whose production
## implementation addresses several global GPU query nodes by autoload name. The
## Phase 7 numerical test does not call those paths, but the names must exist for
## GDScript compilation. Keeping this stub empty ensures the CI test cannot
## accidentally pretend to validate GPU/contact behavior it does not exercise.
