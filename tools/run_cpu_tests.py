#!/usr/bin/env python3
"""Build and run the CPU-only Orbit tests on Linux, without the (Windows-only) CMake build.

Each entry lists the sources to compile directly with g++ (-std=c++23) plus any extra flags. Include directories are every
engine/*/include. This covers libraries that need no GPU, window or Vulkan; GPU-bound code is covered by the compile
checks in tools/check_cloud_shaders.py and tools/check_glass_shaders.py.

    python tools/run_cpu_tests.py                 run everything
    python tools/run_cpu_tests.py optical storm   run entries whose name contains one of the words
    python tools/run_cpu_tests.py --list

Exit code is the number of failing entries.
"""
import pathlib, subprocess, sys, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
E = "engine/"

TESTS = {
    "optical_transport": [E + "optical_transport/src/*.cpp", E + "optical_transport/tests/OpticalTransportTests.cpp"],
    "storm_electrification": [E + "storm_electrification/src/*.cpp", E + "optical_transport/src/DischargeGraph.cpp",
                              E + "storm_electrification/tests/StormElectrificationTests.cpp"],
    "lighting_local_lights": [E + "lighting/src/LocalLightRegistry.cpp", E + "lighting/src/LightingView.cpp",
                              E + "lighting/tests/LocalLightRegistryTests.cpp"],
    "lighting_transient_lights": [E + "lighting/src/TransientLights.cpp", E + "lighting/src/LocalLightRegistry.cpp",
                                  E + "lighting/src/LightingView.cpp", E + "optical_transport/src/DischargeGraph.cpp",
                                  E + "lighting/tests/TransientLightsTests.cpp"],
    "lighting_city_light_tree": [E + "lighting/src/CityLightTree.cpp", E + "lighting/src/LocalLightRegistry.cpp",
                                 E + "lighting/src/LightingView.cpp", E + "optical_transport/src/EmitterTree.cpp",
                                 E + "lighting/tests/CityLightTreeTests.cpp"],
    "celestial_lighting": [E + "celestial_lighting/src/CelestialLighting.cpp", E + "celestial_radiometry/src/Radiometry.cpp",
                           E + "celestial_lighting/tests/CelestialLightingTests.cpp"],
    "celestial_atmosphere_refraction": [E + "celestial_atmosphere/src/AtmosphereRefraction.cpp", E + "optical_transport/src/Refraction.cpp",
                                        E + "celestial_atmosphere/tests/AtmosphereRefractionTests.cpp"],
    "celestial_clouds_far_field": [E + "celestial_clouds/src/CloudFarField.cpp", E + "optical_transport/src/TransmittanceStats.cpp",
                                   E + "celestial_clouds/tests/CloudFarFieldTests.cpp"],
    "celestial_clouds_weather_timewarp": [E + "celestial_clouds/src/WeatherModel.cpp", E + "celestial_clouds/tests/WeatherTimewarpTests.cpp"],
    "mesh_render_glass": ["-include", "{shim}", E + "mesh_render/src/GlassSurface.cpp", E + "mesh_render/tests/GlassSurfaceTests.cpp"],
}

# MSVC-only helper used by a few sources (see the Windows build); a tiny portable stand-in for the Linux build.
SHIM = """#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
inline int _dupenv_s(char** out, size_t* len, const char* name) {
    const char* v = std::getenv(name);
    if (!v) { *out = nullptr; if (len) *len = 0; return 0; }
    *out = static_cast<char*>(std::malloc(std::strlen(v) + 1)); std::strcpy(*out, v); if (len) *len = std::strlen(v) + 1; return 0;
}
"""

def expand(patterns):
    out = []
    for p in patterns:
        if "*" in p:
            out += sorted(str(f) for f in ROOT.glob(p))
        else:
            out.append(str(ROOT / p) if (ROOT / p).exists() else p)
    return out

def main() -> int:
    args = sys.argv[1:]
    if "--list" in args:
        print("\n".join(TESTS))
        return 0
    selected = {k: v for k, v in TESTS.items() if not args or any(a in k for a in args)}
    includes = [f"-I{d}" for d in sorted((ROOT / "engine").glob("*/include"))]
    failed = []
    with tempfile.TemporaryDirectory() as tmp:
        shim = pathlib.Path(tmp) / "shim.h"
        shim.write_text(SHIM)
        for name, parts in selected.items():
            exe = pathlib.Path(tmp) / name
            sources = [p.replace("{shim}", str(shim)) for p in expand(parts)]
            start = time.time()
            build = subprocess.run(["g++", "-std=c++23", "-O2", "-w", *includes, *sources, "-o", str(exe)], capture_output=True, text=True)
            if build.returncode != 0:
                print(f"BUILD FAIL  {name}\n" + "\n".join(build.stderr.splitlines()[:8]))
                failed.append(name)
                continue
            built = time.time()
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            status = "ok  " if run.returncode == 0 else "FAIL"
            print(f"{status}        {name:36s} build {built - start:5.1f}s run {time.time() - built:5.1f}s")
            if run.returncode != 0:
                failed.append(name)
                print("\n".join((run.stdout + run.stderr).splitlines()[-8:]))
    print(f"{len(selected) - len(failed)}/{len(selected)} passed")
    return len(failed)

if __name__ == "__main__":
    sys.exit(main())
