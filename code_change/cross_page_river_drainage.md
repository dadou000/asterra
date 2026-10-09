# Cross-page drainage and basin convergence

## Behavior
Resident physical pages exchange real M09 boundary discharge and terminal basin identity. Upstream area/discharge and basin labels converge across adjacent pages as each ready page publishes.

## Existing owner
- M09 DrainagePage remains the flow, accumulation and terminal label authority.
- StudioTerrainPhysicalPageService owns resident page publication and schedules boundary-dependent rebuilds.
- M27 dependency graph remains the rebuild authority.

## Primary insertion point
- BuildDrainageHalo consumes published neighbor DrainagePage::BoundaryCell data.
- PublishReady updates the shared boundary registry and invalidates only resident edge neighbors when exported values change.
- M09 exports and propagates a stable outlet-cell fingerprint; M16 consumes it for RiverBasinId.

## State authority
M08 surface, M09 drainage page and M16 river network stay canonical/derived in their current owners. Boundary registry entries are immutable M09 snapshots keyed by physical page address.

## Must not be implemented in
- Legacy regional HydrologyGrid/RiverGraph.
- A camera-owned global drainage atlas or a second scheduler.

## Data/control flow
`resident neighbor M09 -> boundary halo -> M09 accumulation + basin terminal -> M16 network -> changed boundary publication -> resident neighbor rebuild`

## Validation
- [ ] Boundary flow orientation is transformed across cube-face edges
- [ ] Downstream pages receive upstream area/discharge
- [ ] Basin terminal IDs propagate from outlet back through resident page chains
- [ ] Unchanged boundary payloads do not trigger rebuild loops
- [ ] Changes are scoped to resident edge neighbors and retain hot-generation safety
