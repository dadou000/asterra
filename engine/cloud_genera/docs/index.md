+++
path = "/rendering/cloud-genera"
title = "Cloud genera: one table for storms, layers and the planet's weather"
kind = "subsystem"
status = "experimental"
summary = """
The ten WMO cloud genera (cirrus, cirrocumulus, cirrostratus, altocumulus, altostratus, nimbostratus, stratocumulus, stratus, cumulus, \
cumulonimbus) live in one table: height band, extinction, ice flag, phase, base brightness, typical element size. The Weather Lab layer \
generator, the Volume renderer (Cloud Type 1-10) and the planet cloud shell (celestial_clouds) all read it, so a cirrocumulus means the same \
thing in a storm volume and in global weather."""
owner_module = "OrbitCloudGenera"
keywords = ["cloud genera", "cirrus", "cirrocumulus", "cirrostratus", "altocumulus", "altostratus", "nimbostratus", "stratocumulus", "stratus", "cumulus", "cumulonimbus", "cloud type", "WMO", "cloud layer", "halo"]
sources = [
  "engine/cloud_genera/include/orbit/cloud_genera/CloudGenera.hpp",
  "engine/cloud_genera/src/CloudGenera.cpp",
  "engine/weather_lab/src/CloudLayers.cpp",
  "engine/volume_render/src/UniversalVolumeRenderer.cpp",
  "engine/celestial_clouds/src/CloudRenderer.cpp",
]
symbols = ["CloudGenus", "CloudGenusInfo", "CloudGeneraTable", "CloudGeneraHlsl", "CloudTypeCode", "kCloudGenusLight", "kCloudGenusBand", "ErodeGenus", "MidDeck", "GenerateCloudLayerDensity"]
applies_to = ["engine/cloud_genera/**", "engine/weather_lab/src/CloudLayers.cpp"]
invariants = [
  "CloudGeneraTable() is the single source of genus numbers. Both shaders get it as HLSL arrays (CloudGeneraHlsl, marker //@CLOUD_GENERA@) indexed by the Volume Cloud Type code: 0 = simulated storm typed by height and density, 1 cirrus, 2 cirrocumulus, 3 cirrostratus, 4 altocumulus, 5 altostratus, 6 nimbostratus, 7 stratocumulus, 8 stratus, 9 cumulus, 10 cumulonimbus (treated as 0). Changing a height or extinction there changes the Volume layers and the planet decks together.",
  "Generated layers (every genus but cumulonimbus) store layout in a baked density; the Volume renderer adds sub-voxel character per genus in ErodeGenus (fibres, grains, cloudlet rims, billows) and lights by table: ice genera get a 22 degree halo and no dark base, others darken toward their base by baseBrightness. Layers that need fine detail (Cloud Type 1-4) are marched at full resolution, the rest at half.",
  "The planet shell draws the same genera where it can: the high deck is cirrus / cirrostratus / cirrocumulus by a low-frequency noise, and MidDeck adds altostratus (stratiform weather, cloud type < ~0.4) or altocumulus (unstable air) in the table's mid-level bands, driven by the weather column coverage. Stratus (.05), stratocumulus (.2), nimbostratus (.32), cumulus (.5) and congestus (.72) on the type axis are drawn by StratusDeck, CumulusDeck and NimbostratusDeck, with blend weights wStratus/wStrato/wNimbo/wCumulus in Extinction and only cumulonimbus left on the tower model. CumulusDeck, an HLSL port of the Weather Lab cumulus / stratocumulus generator (jittered cells, flat-based core plus smooth-unioned lobes, ridged billows, table extinction); the altocumulus part of MidDeck is a port of the altocumulus generator. StratusDeck and NimbostratusDeck port the Stratus() and Nimbostratus() generators (sheet mask driven by weather coverage, mottle, ragged base, scud); the baked noise is low contrast so each sample is stretched about 0.5 (Stretch). Nimbostratus also swallows the mid-level deck (MidDeck is multiplied by 1 - wNimbo). The shell base is metres above sea level (Cloud Layer Base Altitude, default 1500): on a high plateau raise it, or the low cloud starts below the terrain.",
  "Look variants: the volume renderer has eight switchable looks (volume.look_variant 0-7, bit 0 domain warp + finer octave, bit 1 ridged noise, bit 2 high-contrast lighting; 8 = auto). Auto uses the variant chosen per genus in the table (cirrocumulus 2, altocumulus 6, nimbostratus 4, stratocumulus 6, cumulus 7) after an eight-way comparison against photographs; the warp variants turn cirrus into smoke curls and are wrong for it. The generator has a matching style (0-7; cumulus defaults to 6: towering, sharp lobed crowns).",
  "One extinction model for every cloud: CloudGeneraHlsl emits CloudBoost(d) = 1 - (1 - d)^3 and CloudSigma(code, d) = table extinction x CloudBoost(d), and every renderer calls it. The volume renderer applies it to baked density (Volume Extinction Scale is initialised from the table by volume.generate_layer); the planet shell applies it in StratusDeck, NimbostratusDeck, CumulusDeck, MidDeck, the high deck (cirrus 1 / cirrocumulus 2 / cirrostratus 3 by amplitude 0.5 / 0.75 / 0.2) and the cumulonimbus tower and anvil (genus 10, density scaled by the lab / weather density). Do not add a per-path optical-depth constant; change the table or the density instead.",
  "A layer Volume must size itself to its genus: use volume.generate_layer place=true, which applies the recommended half width (cumulus 24 km, altocumulus 32 km, cirrocumulus 24 km: the grid must resolve the elements), thickness and Extinction Scale. A storm-sized Extinction Scale (0.8) turns any layer into an opaque ceiling.",
]
related = ["/rendering/clouds", "/rendering/volumes", "/world/weather-lab"]
depends_on = ["/rendering/clouds"]
used_by = ["/world/weather-lab", "/rendering/clouds"]
verify = [
  "ctest -R \"Orbit.CloudGenera|Orbit.CloudLayers\"",
  "Per genus: volume.generate_layer place+save on a Volume, disable the others, spawn.go, raise the camera and compare the capture with photographs (Wikimedia Commons categories 'Cirrus clouds' ... 'Cumulus humilis clouds').",
]
verified = "022bc07b"

[[diagnose]]
symptom = "a generated layer is an opaque grey ceiling or invisible"
steps = [
  "Read the Volume's Extinction Scale: ice should be ~1e-4 to 1e-3, puffs ~1e-2. Regenerate with volume.generate_layer place=true to reset it from the table.",
  "Check Cloud Type (1-9) and Baked Cache Path; SyncVolumeCaches re-attaches the cache every 30 frames.",
  "Lower coverage: stacked layers over planet clouds are overcast by design of the coverage numbers, not a renderer fault.",
]
docs = ["/rendering/volumes"]

[[diagnose]]
symptom = "fibres or cloudlets look blurry"
steps = [
  "Elements must span several grid cells: size the Volume to the genus' recommended half width. Fibre and grain detail below one cell comes from ErodeGenus, which is aliased by half-resolution marching (Cloud Type 1-4 already march at full resolution).",
]
docs = ["/rendering/volumes"]
+++

## Reference traits (what each genus must look like)

Judged against five photographs per genus (Wikimedia Commons categories, e.g. Category:Cirrus clouds).

| Genus | Look to match | Where it is built |
| --- | --- | --- |
| Cirrus | hair-thin white strands and hooks, translucent, fading tails, bright against blue | `Cirrus()` + fibre erosion |
| Cirrocumulus | tiny white grains in tidy rows, gaps of blue, almost no shading | `Cirrocumulus()` + grain erosion |
| Cirrostratus | milky featureless veil, faint streaks, 22 degree halo around the sun | `Cirrostratus()` + halo term |
| Altocumulus | rounded cloudlets in rows/patches, bright tops, grey undersides, gaps | `Altocumulus()` |
| Altostratus | even grey sheet, sun a dim disc, darker toward the horizon | `Altostratus()` |
| Nimbostratus | deep dark deck, ragged scud beneath, no sun | `Nimbostratus()` |
| Stratocumulus | big soft lumps with dark undersides and bright rims, rolls with gaps | `Stratocumulus()` |
| Stratus | low smooth uniform grey sheet, hazy base | `Stratus()` |
| Cumulus | white cauliflower heads, flat grey bases, blue between | `Cumulus()` + billow erosion |
| Cumulonimbus | towering lobes, dark rain base, fibrous anvil | simulated by the Weather Lab |

## Known gaps

- Cirrus: combed fibres read well from low angles, but real hooks and wisps are still missing.
- Cumulus: lobed and fluffy when sun-facing, but clouds are still domed loaves with flat bases, and stratocumulus is the least convincing genus.
- Planet cumulus has the generator's lobed structure with ridged billows at two scales (the fine one ~120 m; the baked noise is low contrast, so CumulusDeck stretches it by 3.2) and shows creased undersides; sunlit tops are untested because the camera cannot yet fly level with the deck without entering the mid-level cloud. Planet nimbostratus is dark because it shades itself: far samples take their sun optical depth from AltitudeSunTau (the real Extinction integrated through altitude slabs) instead of a per-type guess. No colour is painted. To judge it, lock exposure (display.eye_set min = max exposure scale): auto-exposure brightens a frame that is mostly dark cloud and hides the difference. Its underside is still mostly smooth; the raggedness shows only at the deck's edges.
