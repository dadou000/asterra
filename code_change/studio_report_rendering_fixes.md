# Studio reports R-0010 through R-0013

## Behavior
Remove camera-centred cloud integration rings and atmospheric terminator bands.
Opaque terrain must block direct stellar light at the surface and at scattering
points in air/clouds, while indirect sky illumination remains independent.

## Existing owner and state authority
CloudRenderer owns cloud integration; AtmosphereRenderer owns air integration.
Lighting's TerrainSunShadowField is the existing body-fixed derived visibility
field. TerrainSource, body shape, stellar angular radius and atmosphere parameters
remain authoritative. Do not create another weather, terrain or visibility model.

## Primary insertion points
- CloudRenderer.cpp: choose a represented interval before sampling density and
  use that interval for filtering and Beer-Lambert integration; consume the shared
  terrain visibility field and actual stellar angular radius.
- AtmosphereRenderer.cpp: integrate finite-disc visibility across each segment,
  including the penumbra instead of clipping at the point-sun eclipse boundary.
- StudioViewportRenderer/AtmospherePass: request and pin the same derived terrain
  field for direct lighting, air and clouds.
- DirectLighting.cpp: apply terrain visibility in the normal surface path.

## Secondary touch points
Existing shader descriptors, per-view controls/RPC, documentation and regression
checks. Preserve the diagnostic controls and their shared UI/RPC authority.

## Validation
Replay all four captured poses/times over RPC in a workspace project copy;
capture before/after, inspect the results, compare GPU/frame costs and a higher
sampling reference. Test sunlight blockage by an offscreen ridge and partial-disc
visibility. Run affected CTest targets and documentation validation.

## Hot iteration
Embedded shaders/native changes use the central automatic incremental Studio
generation handoff. Candidates compile/load before activation; failed candidates
leave the current Studio alive. Graph closures pin the terrain buffer through GPU
submission, and project/camera/clock state uses the existing handoff snapshot.
