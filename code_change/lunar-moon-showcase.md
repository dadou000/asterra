# Lunar Moon showcase content

## Behavior
The MCP smoke example's Luna should be a useful, recognizably lunar body rather than a Moon-sized sphere with only generic terrain settings.

## Existing owner
- Module: `world_model` and `terrain_impacts`
- Class/service: `SurfaceAuthoringModel` and the M07 `ImpactField`
- Canonical state: Luna's existing terrain object in `Main.orbitworld`, including its persisted geological event history property.

## Primary insertion point
- File: `examples/mcp-smoke/Worlds/Main.orbitworld`
- Symbol/function: Luna's `Terrain Surface` object properties
- Reason: the example already owns Luna, its physical properties and terrain. Add its impact chronology to that terrain authority instead of creating another body or parallel terrain source.

## Secondary touch points
- `examples/mcp-smoke/README.md`: describe the new lunar sample content.

## Must not be implemented in
- Engine rendering or UI: this is authored sample content using existing terrain processes, not a new renderer or editor feature.
- A second lunar body: Luna already exists in this example.

## Data/control flow
`Main.orbitworld terrain property -> SurfaceAuthoringModel / terrain composition -> ImpactField -> terrain samples`

## Validation
- [x] Existing Luna and terrain objects remain the canonical authority.
- [x] Impact history parses as TOML and contains the authored crater and resurfacing records.
- [x] No duplicate body, terrain object, or editor operation was introduced.
