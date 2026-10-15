#!/usr/bin/env python3
"""Compile-check the cloud shaders embedded in CloudRenderer.cpp without a Windows build.

Extracts the raw-string HLSL pieces, assembles the march/shadow/volume sources exactly like MarchSource() & co.,
builds the filtered-far-field table with the real noise bake, and runs glslangValidator (-D, Vulkan 1.3) on the
filtered-far-field variants. This checks syntax and types only; DXC may differ and nothing is rendered.

    python tools/check_cloud_shaders.py        (needs g++ and glslangValidator: apt install glslang-tools)
"""
import pathlib, re, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
CLOUDS = ROOT / "engine/celestial_clouds"
SRC = (CLOUDS / "src/CloudRenderer.cpp").read_text()

lits = {m.group(1): m.group(2).replace(')" R"(', "")
        for m in re.finditer(r'constexpr const char\* (k\w+) = R"\((.*?)\)";', SRC, re.S)}
common = ["kCloudCommon", "kCloudCommonB", "kCloudCommonC", "kCloudCommonD", "FAR", "kCloudCommonE", "kCloudCommonF", "kCloudLife"]
BINDINGS_OFF = "#define ORBIT_CLOUD_POINT_LIGHTS 0\n#define CLOUD_DEPTH_BINDING 3\n#define CLOUD_TRANSMITTANCE_BINDING 4\n#define CLOUD_MULTI_BINDING 5\n"
BINDINGS_ON = "#define ORBIT_CLOUD_POINT_LIGHTS 1\n#define CLOUD_DEPTH_BINDING 4\n#define CLOUD_TRANSMITTANCE_BINDING 5\n#define CLOUD_MULTI_BINDING 6\n"
variants = {
    "march": (common + ["kVolumeMath", "kDepthBindings", "kMarchBindings", "kVolumeRead", "kVolumeDebugRead", "kCloudLights", "kMarchMain", "kMarchGodrays"], "frag"),
    "shadow": (common + ["kDepthBindings", "kShadowMain"], "frag"),
    "volume": (common + ["kVolumeMath", "kVolumeCompute"], "comp"),
}

HARNESS = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <orbit/celestial_clouds/CloudFarField.hpp>
using f32 = float; using u32 = uint32_t; using i32 = int32_t;
constexpr u32 kNoiseGrid = 32U;
std::vector<f32> BakeBlueNoise() { return {}; }
#include "bake.inc"
int main(int, char** argv) {
    const auto noise = BakeCloudNoise();
    const auto block = orbit::celestial_clouds::FarFieldShaderBlock(orbit::celestial_clouds::BuildFarFieldLut(noise, kNoiseGrid));
    FILE* f = std::fopen(argv[1], "w"); std::fputs(block.c_str(), f); std::fclose(f);
}
'''

def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        a = SRC.index("[[nodiscard]] u32 HashCell")
        b = SRC.index("[[nodiscard]] std::vector<f32> BakeBlueNoise()")
        c = SRC.index("[[nodiscard]] std::vector<f32> BakeCloudNoise()")
        d = SRC.index("\n}\n", c) + 3
        (tmp / "bake.inc").write_text(SRC[a:b] + SRC[c:d])
        (tmp / "gen.cpp").write_text(HARNESS)
        subprocess.run(["g++", "-std=c++23", "-O2", f"-I{CLOUDS}/include", f"-I{ROOT}/engine/optical_transport/include",
                        str(tmp / "gen.cpp"), str(CLOUDS / "src/CloudFarField.cpp"),
                        str(ROOT / "engine/optical_transport/src/TransmittanceStats.cpp"), "-o", str(tmp / "gen")], check=True)
        subprocess.run([str(tmp / "gen"), str(tmp / "far.hlsl")], check=True)
        far = (tmp / "far.hlsl").read_text()
        bad = 0
        for flag in (1,):   # the filtered far field is the only variant now
            for lights in (0, 1):
                for name, (keys, stage) in variants.items():
                    if lights and name != "march":
                        continue   # only the march pipeline gains the light buffer and shifted bindings
                    text = (BINDINGS_ON if lights else BINDINGS_OFF) + "".join(
                        (far if flag else "") if k == "FAR" else lits[k] for k in keys)
                    path = tmp / f"{name}{flag}{lights}.hlsl"
                    path.write_text(text)
                    r = subprocess.run(["glslangValidator", "-D", "-V", "-S", stage, "-e", "main", "--target-env", "vulkan1.3", "-o", str(path.with_suffix(".spv")), str(path)],
                                       capture_output=True, text=True)
                    errors = [l for l in r.stdout.splitlines() if l.startswith("ERROR")]
                    print(f"far-field={flag} point-lights={lights} {name:6s} {'ok' if r.returncode == 0 else 'FAILED'}")
                    for l in errors[:5]:
                        print("   ", l)
                    bad += r.returncode != 0
        return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
