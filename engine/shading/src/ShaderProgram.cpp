#include <orbit/shading/ShaderProgram.hpp>

#include <chrono>
#include <exception>
#include <format>

namespace orbit::shading
{
namespace
{
// Layout shared by the object vertex and pixel stages: 40 dwords.
constexpr std::string_view kObjectPush = R"(
struct OrbitPush
{
    float4 vp0;        // view-projection matrix rows (row-vector convention)
    float4 vp1;
    float4 vp2;
    float4 vp3;
    float4 cameraTime; // xyz camera position (world), w time seconds
    float4 model;      // x cos(yaw), y sin(yaw), z scale, w shape id
    float4 lightState; // x preset id, y sun azimuth, z sun elevation, w exposure
    float4 params0;    // shader parameters 0..3
    float4 params1;    // shader parameters 4..7
    float4 misc;       // reserved
};
[[vk::push_constant]] OrbitPush g_orbit;
)";

constexpr std::string_view kObjectHelpers = R"(
float OrbitParam(uint i)
{
    return i < 4u ? g_orbit.params0[i] : g_orbit.params1[i - 4u];
}
float3 OrbitParam3(uint i) { return float3(OrbitParam(i), OrbitParam(i + 1u), OrbitParam(i + 2u)); }
float2 OrbitParam2(uint i) { return float2(OrbitParam(i), OrbitParam(i + 1u)); }
float4 OrbitParam4(uint i) { return float4(OrbitParam(i), OrbitParam(i + 1u), OrbitParam(i + 2u), OrbitParam(i + 3u)); }

// The background shown behind the object (see OrbitBackdropFor).
float3 OrbitBackdrop(float3 d)
{
    return OrbitBackdropFor(
        d,
        (uint)(g_orbit.misc.x + 0.5),
        (uint)(g_orbit.lightState.x + 0.5),
        OrbitSunDirection(g_orbit.lightState.y, g_orbit.lightState.z));
}

float3 OrbitEnvironment(float3 d)
{
    return OrbitEnvironmentFor(
        d,
        (uint)(g_orbit.lightState.x + 0.5),
        OrbitSunDirection(g_orbit.lightState.y, g_orbit.lightState.z));
}

// The preview shape being drawn: 0 Sphere, 1 Plane, 2 Cube, 3 Mesh (see
// PreviewShape in ShadingContract.hpp). A refractive shader needs this to
// know whether it has a real, closed-form volume to push a ray through.
uint OrbitShapeId() { return (uint)(g_orbit.model.w + 0.5); }

// The preview object is centred on the origin and only ever yawed, never
// translated or non-uniformly scaled, so object <-> world is a pure
// rotation and the same transform inverts both positions and directions.
float3 OrbitToLocal(float3 worldVector)
{
    const float c = g_orbit.model.x;
    const float s = g_orbit.model.y;
    return float3(
        worldVector.x * c - worldVector.z * s,
        worldVector.y,
        worldVector.x * s + worldVector.z * c);
}
float3 OrbitToWorld(float3 localVector)
{
    const float c = g_orbit.model.x;
    const float s = g_orbit.model.y;
    return float3(
        localVector.x * c + localVector.z * s,
        localVector.y,
        -localVector.x * s + localVector.z * c);
}

// Exact distance to where a ray leaves a unit-radius sphere centred on the
// origin, given a point `p` already ON that sphere (|p| == radius) and a
// unit direction `d` heading into it. |p + t*d|^2 = |p|^2 exactly when
// t*(t + 2*dot(p,d)) = 0, so the non-trivial root is t = -2*dot(p,d); no
// per-radius division needed.
float OrbitSphereExitDistance(float3 p, float3 d)
{
    return max(-2.0 * dot(p, d), 0.0);
}

// Exact distance to where a ray leaves an axis-aligned cube of half-extent
// `h` centred on the origin, given a point on its surface and a unit
// direction heading inward (the standard per-axis slab test, keeping only
// the face the ray is travelling toward on each axis).
float OrbitBoxExitDistance(float3 p, float3 d, float h)
{
    const float3 safeD = float3(
        abs(d.x) > 1.0e-6 ? d.x : 1.0e-6,
        abs(d.y) > 1.0e-6 ? d.y : 1.0e-6,
        abs(d.z) > 1.0e-6 ? d.z : 1.0e-6);
    const float3 texit = (sign(safeD) * h - p) / safeD;
    return max(min(min(texit.x, texit.y), texit.z), 0.0);
}
)";

constexpr std::string_view kObjectWrapper = R"(
struct OrbitPSInput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

float4 main(OrbitPSInput input) : SV_Target0
{
    OrbitSurface s;
    s.positionWS = input.worldPos;
    s.viewWS = normalize(g_orbit.cameraTime.xyz - input.worldPos);

    // Double-sided: flip toward the viewer so a plane seen from below shades.
    float3 n = normalize(input.worldNormal);
    if (dot(n, s.viewWS) < 0.0)
        n = -n;
    s.normalWS = n;

    const float3 helper = abs(n.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    s.tangentWS = normalize(cross(helper, n));
    s.bitangentWS = cross(n, s.tangentWS);
    s.uv = input.uv;
    s.time = g_orbit.cameraTime.w;

    const uint preset = (uint)(g_orbit.lightState.x + 0.5);
    const float3 sun = OrbitSunDirection(g_orbit.lightState.y, g_orbit.lightState.z);
    const OrbitLighting lighting = OrbitMakeLightingFor(preset, sun);

    const float4 shaded = Shade(s, lighting);
    return float4(OrbitDisplayEncode(shaded.rgb * g_orbit.lightState.w), saturate(shaded.a));
}
)";

constexpr std::string_view kPreviewVertex = R"(
struct OrbitPush
{
    float4 vp0;
    float4 vp1;
    float4 vp2;
    float4 vp3;
    float4 cameraTime;
    float4 model;
    float4 lightState;
    float4 params0;
    float4 params1;
    float4 misc;
};
[[vk::push_constant]] OrbitPush g_orbit;

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

VSOutput main(VSInput v)
{
    const float c = g_orbit.model.x;
    const float s = g_orbit.model.y;
    const float scale = g_orbit.model.z;

    const float3 p = float3(
        v.position.x * c + v.position.z * s,
        v.position.y,
        -v.position.x * s + v.position.z * c) * scale;
    const float3 n = float3(
        v.normal.x * c + v.normal.z * s,
        v.normal.y,
        -v.normal.x * s + v.normal.z * c);

    VSOutput o;
    o.worldPos = p;
    o.worldNormal = n;
    o.uv = v.uv;
    o.position =
        p.x * g_orbit.vp0 + p.y * g_orbit.vp1 + p.z * g_orbit.vp2 + g_orbit.vp3;
    return o;
}
)";

constexpr std::string_view kBackgroundVertex = R"(
struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput main(uint id : SV_VertexID)
{
    // One oversized triangle covering the viewport.
    const float2 p = float2((id << 1) & 2, id & 2);
    VSOutput o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
)";

constexpr std::string_view kBackgroundPixel = R"(
struct BgPush
{
    float4 forward;
    float4 right;
    float4 up;
    float4 params;  // x tan(fov/2), y aspect, z background id, w preset id
    float4 sun;     // x azimuth, y elevation, z exposure, w time
    float4 screen;  // x width, y height
};
[[vk::push_constant]] BgPush g_bg;

struct BgInput
{
    float4 position : SV_Position;
};

float4 main(BgInput input) : SV_Target0
{
    // Framebuffer pixel coordinates have y growing downward; convert to
    // screen-up-positive so the mapping is independent of clip-space flips.
    const float2 uv = input.position.xy / max(g_bg.screen.xy, float2(1.0, 1.0));
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    const float3 ray = normalize(
        g_bg.forward.xyz +
        g_bg.right.xyz * (ndc.x * g_bg.params.y * g_bg.params.x) +
        g_bg.up.xyz * (ndc.y * g_bg.params.x));

    const uint mode = (uint)(g_bg.params.z + 0.5);
    const uint preset = (uint)(g_bg.params.w + 0.5);
    const float3 sun = OrbitSunDirection(g_bg.sun.x, g_bg.sun.y);

    const float3 color = OrbitBackdropFor(ray, mode, preset, sun);

    return float4(OrbitDisplayEncode(color * g_bg.sun.z), 1.0);
}
)";

// Shown when there is nothing valid to shade with: a magenta/black checker so
// a broken shader is obvious, as in Blender and Unreal.
constexpr std::string_view kErrorShader = R"(
float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float2 cell = floor(s.uv * 12.0);
    const float parity = fmod(cell.x + cell.y, 2.0);
    return float4(lerp(float3(0.02, 0.0, 0.02), float3(1.0, 0.0, 1.0), parity) * 2.0, 1.0);
}
)";

[[nodiscard]] std::string ParameterAccessors(
    const ShaderParameterLayout& layout)
{
    std::string text = "\n// Generated from // @param annotations.\n";

    for (const auto& parameter : layout.parameters)
    {
        switch (parameter.components)
        {
        case 1U:
            text += std::format(
                "float Param_{}() {{ return OrbitParam({}u); }}\n",
                parameter.name, parameter.offset);
            break;
        case 2U:
            text += std::format(
                "float2 Param_{}() {{ return OrbitParam2({}u); }}\n",
                parameter.name, parameter.offset);
            break;
        case 3U:
            text += std::format(
                "float3 Param_{}() {{ return OrbitParam3({}u); }}\n",
                parameter.name, parameter.offset);
            break;
        default:
            text += std::format(
                "float4 Param_{}() {{ return OrbitParam4({}u); }}\n",
                parameter.name, parameter.offset);
            break;
        }
    }

    return text;
}
} // namespace

u64 HashSource(const std::string_view text) noexcept
{
    u64 value = 14695981039346656037ULL;
    for (const unsigned char c : text)
    {
        value ^= c;
        value *= 1099511628211ULL;
    }
    return value;
}

std::string_view PreviewVertexSource() noexcept { return kPreviewVertex; }
std::string_view BackgroundVertexSource() noexcept { return kBackgroundVertex; }
std::string_view ErrorShaderSource() noexcept { return kErrorShader; }

std::string BackgroundPixelSource()
{
    std::string source(ShadingPreludeCommon());
    source += kBackgroundPixel;
    return source;
}

std::string BuildObjectPixelSource(
    const std::string_view assetName,
    const std::string_view userSource,
    const ShaderParameterLayout& layout)
{
    std::string source(ShadingPreludeCommon());
    source += kObjectPush;
    source += kObjectHelpers;
    source += ParameterAccessors(layout);

    // Everything above is engine text; restart line numbering so compiler
    // messages point at the user's own file and lines.
    source += std::format("\n#line 1 \"{}\"\n", assetName);
    source += userSource;
    source += "\n#line 1 \"orbit-shading-wrapper\"\n";
    source += kObjectWrapper;
    return source;
}

ShadingProgram CompileShadingProgram(
    const shader::Compiler& compiler,
    const std::string_view assetName,
    const std::string_view userSource)
{
    ShadingProgram program;
    program.sourceHash = HashSource(userSource);
    program.layout = ParseShaderParameters(userSource);

    if (!program.layout.problems.empty())
    {
        for (const auto& problem : program.layout.problems)
        {
            program.diagnostics += std::string(assetName) + ": " + problem + "\n";
        }
        return program;
    }

    const auto started = std::chrono::steady_clock::now();

    try
    {
        const std::string source =
            BuildObjectPixelSource(assetName, userSource, program.layout);

        program.pixel = compiler.Compile({
            .source = source,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false});
        program.ok = true;
    }
    catch (const std::exception& exception)
    {
        program.diagnostics = exception.what();
    }

    program.compileMilliseconds =
        std::chrono::duration<f64, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();
    return program;
}
} // namespace orbit::shading
