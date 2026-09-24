#include <orbit/volume_fields/VolumeFieldStorage.hpp>

#define ORBIT_VOLUME_RENDER_BASE_IMPLEMENTATION 1
#define AddPasses AddLivePasses
#define Diagnostics LiveDiagnostics
#include "UniversalVolumeRendererBase.inc"
#undef Diagnostics
#undef AddPasses
#undef ORBIT_VOLUME_RENDER_BASE_IMPLEMENTATION
