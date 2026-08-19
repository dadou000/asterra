# Planet layer maps

Equirectangular exports of the default world (`world_seed = 0x4153544552524100`,
1000 km radius, 192² cells per cube face). Regenerate with:

```
godot --headless --path . res://tests/Preview.tscn
```

| File | Layer |
|---|---|
| `00_elevation.png` | elevation after erosion |
| `01_tectonic_plates.png` | plate partition |
| `02_bedrock_geology.png` | surface bedrock family |
| `03_resources.png` | R = iron/copper, G = quartz, B = petroleum/coal |
| `04_sediment_and_erosion.png` | sediment thickness vs tectonic uplift |
| `05_drainage_network.png` | discharge (log scale) over shaded relief |
| `06_watersheds.png` | catchments by outlet |
| `07_temperature.png` | mean annual temperature |
| `08_precipitation.png` | mean annual rainfall |
| `09_prevailing_winds.png` | zonal/meridional wind components |
| `10_severe_weather.png` | severe-weather likelihood |
| `11_soil.png` | R = sand, G = organic, B = clay, brightness = depth |
| `12_biomes.png` | derived biomes |
| `13_buildability.png` | general buildability |
| `14_transport_corridors.png` | natural transport corridors |

Read them together: the rain shadows in `08` sit behind the belts in `00`, the
rivers in `05` thread the valleys the erosion pass cut, the deserts in `12` sit
under the subtropical highs, and the corridors in `14` follow both.
