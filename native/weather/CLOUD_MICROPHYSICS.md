# Sparse cloud microphysics

The Oklahoma/Great-Plains weather regime uses a single-owner cloud-water closure in `weather_native_oklahoma.cpp`.

The legacy core cloud branch is disabled while the transport/momentum pass runs. This prevents its old ~68% RH sub-grid condensation source from competing with the sparse-cloud closure and avoids continually converting climatological humidity into a persistent condensate reservoir.

The global climatological humidity nudger is retained only as a weak long-timescale anchor (12% of its requested tuning strength); the coupled surface evaporation/dew path is the primary physical lower-boundary water source. The local nest does not use climatological humidity restoration and receives large-scale moisture through its parent boundary conditions.

New fair-weather sub-grid cloud formation is controlled by **vapour relative humidity**, not vapour plus pre-existing condensate. Typical onset is ~90% RH in the lower atmosphere and ~93% aloft. Existing condensate therefore cannot increase its own formation probability. Total vapour + liquid + ice is used only to preserve saturated equilibrium and mass through condensation/evaporation.

Activated convection lowers the effective RH onset and increases the retained condensate fraction, allowing sparse cells to become optically deep while ordinary humid air remains mostly clear. Fair liquid cloud clears on minute-scale timescales; active storm liquid and ice/anvils retain substantially longer memory.
