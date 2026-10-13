# Impact ray support and chronology corrections

Existing owner: `terrain_impacts::ImpactField`; primary insertion points are
`SampleImpact`, prepared impact geometry, and the `.orbitimpacts` codec.
The authored recipe remains canonical; prepared geometry, regional event
batches, CPU/GPU rasters and `.orbitbake` files remain disposable caches.

Add optional long, irregular material rays independently of the massive ejecta
blanket. Extend the existing spherical queries, recipe hash and regional
invalidation bounds so distant rays cannot be omitted or left stale. Mirror
the same evaluation in `GeologyCompute.hpp`, using the existing packet slots.
Precompute incidence/azimuth geometry once per event instead of per texel.

Correct the scaling angle to match the existing morphology convention (zero
from the surface normal means vertical). Preserve zero exposure age after new
resurfacing rather than treating it as an untouched ancient surface. Keep
tectonic overprinting of deposits and rays consistent between CPU and GPU.

Authoring uses the existing undoable `SetImpactHistoryToml` operation through
Surface Tools and `terrain.impacts_set`/MCP; no second editor operation, event
store, watcher, cache or renderer is introduced. C++ and embedded HLSL changes
use the central native-generation refresh path. Algorithm versioning rejects
stale geology products after activation; failed candidate builds preserve the
running generation and persisted authoring records.

`StudioViewportPanels::DrawCraterRecord` exposes the two new fields alongside
the existing ray controls. `BuildImpactHistoryInvalidations` consumes the
geological owner's old/new influence caps instead of maintaining a second
event hash/bounds algorithm. A physical-page footprint wider than the current
64-tile bounded scope uses global invalidation; the geological raster still
uses its own region-local dirty-tile path.
