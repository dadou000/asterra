# Asterra ecological scatter plan

Status: **0.1 acquisition/catalog foundation implemented; runtime renderer migration pending optimized meshes.**

The target is a dense, coherent environment in which ground cover, shrubs, trees,
deadwood, stones and boulders are consequences of the same climate, soil,
hydrology and geology fields that generate the terrain. Scatter must not look like
uniform random decoration.

## 1. Current baseline

The current production scatter autoload is `gpu_terrain_scatter_global.gd`. It
keeps the proven vertex-GPU candidate path authoritative because the indirect
RenderingDevice compaction path has been unreliable on the current runtime.
The renderer is still hard-coded to three families: grass, geologic stone and
river stone.

The existing foundation is worth keeping:

- deterministic absolute tangent-space cells;
- GPU-side candidate jitter and acceptance;
- GPU sampling of biome, climate, soil, geology, hydrology and macro elevation;
- persistent terrain edit and active deformation bindings;
- floating-origin-safe placement.

The rewrite should generalize those mechanisms rather than replace them.

## 2. Asset policy

Primary source: **Poly Haven CC0 photoreal models**. The selected source models
are tracked in `assets/scatter/asset_manifest.json`. The manifest is the source of
truth for acquisition and ecological assignment.

Source downloads are authoring inputs, not runtime assets. Use 1K glTF for the
first integration pass; retain higher-resolution source only when a close-range
hero asset demonstrably needs it. Large trees and grass scans can contain millions
of source triangles, so raw source geometry must never become the final runtime
representation by accident.

Run:

```bash
python tools/fetch_scatter_assets.py --list
python tools/fetch_scatter_assets.py --all --priority core
```

The downloader uses Poly Haven's `/files/{id}` API, downloads glTF dependencies,
verifies provider MD5 hashes, and writes `source.json` provenance beside each
asset.

## 3. Biome recipes

Asterra currently has 18 macro biomes. Hydrology stays orthogonal, so a river or
wet patch modifies a biome recipe rather than replacing it.

| Biome | Ground layer | Mid layer | Major layer | Geological layer |
| --- | --- | --- | --- | --- |
| Ocean | none in v1 | none | none | terrain/bathymetry only; benthic pack is an explicit gap |
| Shelf sea | pebble/stone patches | none in v1 | coastal ledges | coastal rocks, submerged boulders; kelp/seagrass gap |
| Ice cap | almost none | none | none | rare exposed stone/boulder |
| Tundra | moss, short/dry grass, weed | very low shrub | none | stones, occasional boulders |
| Taiga | moss, grass, fern | shrubs, fir/pine saplings | fir/pine canopy, logs/stumps | mossy stones, boulders |
| Cold desert | sparse dry grass | dry shrub/succulent | rare dead trunk | quartz stones, desert boulders |
| Temperate grassland | dense mixed grass, weed, wildflower | sparse shrubs | almost no trees | small stones, rare boulders |
| Temperate forest | grass/moss/fern | shrubs, saplings | broadleaf + some conifer canopy, deadwood | mossy stones and boulders |
| Temperate rainforest | dense moss/fern | dense shrubs/saplings | dense wet broadleaf/windswept canopy, deadwood | mossy rocks |
| Mediterranean | dry grass | dense scrub | sparse pine/windswept trees | warm stones/boulders, dry debris |
| Steppe | patchy dry grass/weeds | sparse low shrubs | none/very sparse trees | stones and occasional boulders |
| Hot desert | tiny succulent patches | desert shrubs | rare desert trees/dead trunks | quartz stones and desert boulders |
| Savanna | continuous-to-patchy grass | scattered shrubs | sparse broadleaf/desert-form trees | exposed boulders, deadwood |
| Tropical seasonal forest | grass in openings | tropical shrubs + fern | broadleaf canopy | roots, logs, stones |
| Tropical rainforest | moss/fern/tropical groundcover | very dense tropical shrubs | dense multi-tier broadleaf canopy | roots, deadwood, mossy rocks |
| Wetland | wet grass/weed/fern | shrubs | sparse moisture-tolerant trees | low rock density; reeds/cattails gap |
| Alpine | sheltered moss/short grass | almost none | no canopy above treeline | dominant stone/scree/boulders |
| Bare rock | rare crevice moss/grass only | none | none | geology-driven stones, boulders, ledges |

## 4. Selected asset library

The initial library contains realistic representatives for these functional
families:

- grasses: `grass_medium_01`, `grass_medium_02`, `grass_bermuda_01`;
- micro ground cover: `moss_01`, `weed_plant_02`, `celandine_01`, `nettle_plant`, `fern_02`;
- temperate/general shrubs: `shrub_01` through `shrub_04`;
- arid plants: `didelta_spinosa`, `cheiridopsis_succulent`, `othonna_cerarioides`;
- tropical understory: `anthurium_botany_01`, `pachira_aquatica_01`;
- conifer regeneration: `fir_sapling`, `fir_sapling_medium`, `pine_sapling_small`;
- canopy: `fir_tree_01`, `pine_tree_01`, `tree_small_02`, `jacaranda_tree`, `island_tree_01..03`, `quiver_tree_01..02`;
- forest debris: `dead_tree_trunk`, `dead_tree_trunk_02`, `tree_stump_01..02`, `single_root`;
- dry debris: `dead_quiver_branch_01`, `dry_quiver_leaf`, `bark_debris_01`, `dead_quiver_trunk`;
- rocks: `boulder_01`, three Namaqualand boulders, `namaqualand_rocks_01`, two mossy stone sets, `stone_01`;
- coast/shallow shelf: `sand_rocks_small_01`, `coast_rocks_01`, `coast_rocks_05`.

These are **visual archetypes**. Asterra does not need to claim that an Earth
species named in a source filename literally exists on the planet. Runtime UI and
gameplay should use Asterra species/archetype names later.

## 5. Runtime representation tiers

A single render strategy is not appropriate for every scatter scale.

### Tier A - micro scatter

Grass blades, moss clumps, tiny plants, leaves/debris and small stones.

- GPU candidate grid/compaction;
- no physics bodies;
- no individual CPU transforms;
- aggressive screen-size fade;
- wind animation only for vegetation;
- usually no shadows beyond 20-40 m.

Target visible counts can be very high because the vertex/index cost per accepted
instance is low after authoring optimization.

### Tier B - ground and mid scatter

Shrubs, fern clumps, succulent clusters, medium stone sets and branch debris.

- GPU generated and compacted;
- 2-3 geometry LODs;
- per-instance scale/yaw/lean variation;
- short-range shadows;
- no collision unless promoted by gameplay.

### Tier C - major props

Boulders, fallen logs, roots, stumps and saplings.

- GPU placement database or deterministic cells;
- hierarchical LOD;
- collision only inside an interaction radius;
- promote to a real physics entity when disturbed, excavated or detached.

### Tier D - canopy

Trees require their own hierarchy.

1. close mesh;
2. simplified LOD1;
3. aggressive LOD2/card foliage;
4. tree impostor;
5. distant forest-canopy macro contribution.

LOD selection must ultimately be projected-screen-size based, not a single fixed
world-space distance.

## 6. Ecological classification

Candidate acceptance should be approximately:

```text
asset weight =
    biome affinity
  * vegetation biomass
  * temperature response
  * precipitation/humidity response
  * soil-depth/texture response
  * hydrology response
  * slope response
  * geology response (rocks)
  * clustered noise / succession pattern
  * disturbance mask
```

The existing `PlanetContext` textures already expose most of these fields. The
next scatter shader should evaluate an **archetype descriptor** rather than call a
separate hard-coded function for every asset.

Important spatial structure:

- grass occurs in patches, not independent white noise;
- saplings cluster around viable canopy/recruitment zones;
- wet understory follows drainage, shade proxy and moisture;
- deadwood frequency follows forest biomass and disturbance age;
- scree follows steep/exposed geology;
- river stones align with channels and floodplains;
- large boulders correlate with rock type, slope and erosional exposure.

## 7. Geology-aware rock families

Asterra already carries 13 bedrock families. The final rock asset library should
expand beyond the initial generic scans and tag each rock asset by visual geology.
At runtime, rock choice should primarily follow `rock_id`, weathering/exposure,
sediment and slope. This prevents granite, shale, sandstone and basalt provinces
from receiving the same random boulder palette.

The initial CC0 rocks are sufficient to implement the system, but not sufficient
for final geological diversity. This is a planned acquisition pass, not a reason
to block the first ecological renderer.

## 8. Terrain deformation interaction

Scatter must follow the authoritative rendered terrain stack.

- micro vegetation disappears when soil is excavated;
- freshly deformed/exposed soil temporarily suppresses vegetation;
- loose small scatter can be culled rather than simulated;
- large boulders/logs/trees promote to physical entities when their support changes;
- tree removal/destruction writes a persistent disturbance record so it does not
  immediately respawn from deterministic scatter;
- regrowth can later be derived from time, climate and succession state.

## 9. Implementation stages

### 0.1 - acquisition/catalog foundation (this change)

- realistic CC0 asset manifest;
- all 18 current biomes explicitly planned;
- authoring downloader with provenance and checksum verification;
- data-driven `ScatterEcologyCatalog` with render-budget defaults;
- no change to the current authoritative scatter autoload.

### 0.2 - authoring optimization

Build an offline optimizer (Blender CLI is the preferred route) that produces:

- normalized origin/scale/orientation;
- LOD0/1/2 meshes with foliage-safe simplification;
- alpha-tested foliage materials;
- packed ARM/normal/albedo maps at runtime resolutions;
- simplified collision mesh for major props;
- impostor data for trees;
- a small generated metadata file per runtime archetype.

Do not ship the raw scan geometry.

### 0.3 - ecological GPU ground scatter

Replace the fixed grass/geo-stone/river-stone assumption with descriptor-driven
batches for micro + ground tiers. Keep the current stable candidate fallback until
the indirect RenderingDevice path is verified on the target Godot build.

First visual integration set:

- 3 grasses;
- moss + weed + fern;
- 4 shrubs;
- small stone + mossy stone set;
- generic + desert boulders;
- dry branch/debris.

This alone should radically improve ground-level richness.

### 0.4 - trees and major props

- canopy placement;
- sapling recruitment clusters;
- LOD/impostor hierarchy;
- near-player collision activation;
- deadwood/root/stump placement.

### 0.5 - compaction + culling

Repair/re-enable the RenderingDevice compact path, then add:

- accepted-instance compaction;
- frustum/Hi-Z or coarse terrain occlusion where profitable;
- per-LOD indirect draws;
- stable cross-fades/dithered transitions;
- per-quality density and distance budgets.

### 0.6 - persistence and disturbance

Integrate terrain deformation, construction clear zones, tree removal, fire/storm
damage and long-term regrowth.

## 10. Explicit gaps after this acquisition pass

The initial library intentionally leaves these gaps visible rather than filling
them with poor assets:

1. realistic CC0 kelp/seagrass/coral/benthic meshes for ocean and shelf-sea;
2. realistic reeds/cattails/rushes for wetlands;
3. enough distinct broadleaf tree species for convincing temperate and tropical
   canopy diversity;
4. rock scans categorized across all 13 Asterra bedrock families;
5. dedicated leaf-litter geometry variants (surface textures are deferred as
   requested; close-range leaf geometry can be added with the same micro tier).

Those should be separate acquisition passes. Quality is more important than
forcing a weak asset into every slot.
