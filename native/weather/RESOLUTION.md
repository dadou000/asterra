# Weather resolution baseline

`weather/0.0.5` now uses the parent atmosphere that weather tuning should target rather than the former prototype grid.

## L0 parent

- Horizontal grid: **1024 x 512** equirectangular cells.
- Equatorial spacing on Asterra (R = 3500 km): approximately **21.5 km**.
- Vertical coordinate: **30 non-uniform sigma levels** from roughly **0.10 km to 20.2 km**.
- The lowest kilometre is sampled densely (about 0.10, 0.25, 0.45, 0.70 and 1.00 km) so the surface boundary layer is no longer represented by a single ~450 m level.
- Pressure-thickness layer weights are used for vertically integrated condensate products. Display/optical integrations are explicitly normalized from the former six-level calibration so increasing vertical resolution does not multiply apparent cloud amount.
- Saturation thermodynamics now permit pressure down to 6 kPa; the former 12 kPa numerical floor was only valid for the old ~12.8 km model top and would artificially favor upper-level ice at the new ~20 km top.

## Runtime

The native core remains AVX2/FMA and now additionally uses OpenMP worker parallelism for the major row/column-independent atmospheric passes and output packing. Both the global parent and the existing 2.2 km local nest use the 30-level coordinate.

`WeatherSystem` checks the native DLL's width, height and layer count at startup. An older 256 x 128 x 6 DLL is rejected with a rebuild warning rather than being interpreted with the new texture dimensions.

The weather globe exposes all 30 wind levels. Global procedural surface-field construction is performed through the worker pool so the 1024 x 512 geography upload does not block the main thread for the full build.

## Adaptive hierarchy target

The parent is the permanent L0 tuning baseline. The refinement planner still targets approximately 5 km / 48 levels for L1, 1 km / 64 levels for L2, 250 m / 84 levels for L3 and 75 m / 112 levels for L4. Those child numerical solvers are separate work; the parent resolution should no longer be retuned from the old ~86 km / six-level prototype.
