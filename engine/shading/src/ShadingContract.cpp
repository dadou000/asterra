#include <orbit/shading/ShadingContract.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>

namespace orbit::shading
{
namespace
{
constexpr std::array<EnumEntry, 4> kShapes{{
    {static_cast<u8>(PreviewShape::Sphere), "sphere", "Sphere"},
    {static_cast<u8>(PreviewShape::Plane), "plane", "Plane"},
    {static_cast<u8>(PreviewShape::Cube), "cube", "Cube"},
    {static_cast<u8>(PreviewShape::Mesh), "mesh", "Mesh (.obj)"},
}};

constexpr std::array<EnumEntry, 5> kLightingPresets{{
    {static_cast<u8>(LightingPreset::Studio), "studio",
     "Studio (key, fill, rim)"},
    {static_cast<u8>(LightingPreset::Sun), "sun", "Sun and sky"},
    {static_cast<u8>(LightingPreset::Overcast), "overcast", "Overcast"},
    {static_cast<u8>(LightingPreset::Sunset), "sunset", "Sunset"},
    {static_cast<u8>(LightingPreset::Space), "space",
     "Space (single hard sun)"},
}};

constexpr std::array<EnumEntry, 4> kBackgrounds{{
    {static_cast<u8>(PreviewBackground::Environment), "environment",
     "Lighting environment"},
    {static_cast<u8>(PreviewBackground::Gradient), "gradient",
     "Dark gradient"},
    {static_cast<u8>(PreviewBackground::Gray), "gray", "Neutral gray"},
    {static_cast<u8>(PreviewBackground::Checker), "checker", "Checker"},
}};

template <typename Enum, std::size_t N>
[[nodiscard]] std::optional<Enum> ParseEnum(
    const std::array<EnumEntry, N>& table,
    const std::string_view key) noexcept
{
    for (const auto& entry : table)
    {
        if (entry.key == key)
        {
            return static_cast<Enum>(entry.value);
        }
    }
    return std::nullopt;
}

template <std::size_t N>
[[nodiscard]] std::string_view KeyOf(
    const std::array<EnumEntry, N>& table,
    const u8 value) noexcept
{
    for (const auto& entry : table)
    {
        if (entry.value == value)
        {
            return entry.key;
        }
    }
    return {};
}

[[nodiscard]] std::vector<std::string_view> Tokens(
    const std::string_view text)
{
    std::vector<std::string_view> result;
    std::size_t index = 0U;

    while (index < text.size())
    {
        while (index < text.size() &&
               (text[index] == ' ' || text[index] == '\t' ||
                text[index] == '\r'))
        {
            ++index;
        }

        const std::size_t start = index;
        while (index < text.size() &&
               text[index] != ' ' && text[index] != '\t' &&
               text[index] != '\r')
        {
            ++index;
        }

        if (index > start)
        {
            result.push_back(text.substr(start, index - start));
        }
    }

    return result;
}

[[nodiscard]] std::optional<f64> ParseNumber(
    const std::string_view token) noexcept
{
    f64 value = 0.0;
    const auto [end, error] = std::from_chars(
        token.data(), token.data() + token.size(), value);
    if (error != std::errc{} || end != token.data() + token.size() ||
        !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool IsIdentifier(const std::string_view name) noexcept
{
    if (name.empty() ||
        !(std::isalpha(static_cast<unsigned char>(name[0])) != 0 ||
          name[0] == '_'))
    {
        return false;
    }

    return std::ranges::all_of(
        name,
        [](const char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
                c == '_';
        });
}
} // namespace

std::span<const EnumEntry> Shapes() noexcept { return kShapes; }
std::span<const EnumEntry> LightingPresets() noexcept
{
    return kLightingPresets;
}
std::span<const EnumEntry> Backgrounds() noexcept { return kBackgrounds; }

std::optional<PreviewShape> ParseShape(const std::string_view key) noexcept
{
    return ParseEnum<PreviewShape>(kShapes, key);
}
std::optional<LightingPreset> ParseLightingPreset(
    const std::string_view key) noexcept
{
    return ParseEnum<LightingPreset>(kLightingPresets, key);
}
std::optional<PreviewBackground> ParseBackground(
    const std::string_view key) noexcept
{
    return ParseEnum<PreviewBackground>(kBackgrounds, key);
}

std::string_view ShapeKey(const PreviewShape value) noexcept
{
    return KeyOf(kShapes, static_cast<u8>(value));
}
std::string_view LightingKey(const LightingPreset value) noexcept
{
    return KeyOf(kLightingPresets, static_cast<u8>(value));
}
std::string_view BackgroundKey(const PreviewBackground value) noexcept
{
    return KeyOf(kBackgrounds, static_cast<u8>(value));
}

const ShaderParameterDecl* ShaderParameterLayout::Find(
    const std::string_view name) const noexcept
{
    for (const auto& parameter : parameters)
    {
        if (parameter.name == name)
        {
            return &parameter;
        }
    }
    return nullptr;
}

ShaderParameterLayout ParseShaderParameters(const std::string_view source)
{
    ShaderParameterLayout layout;
    u32 lineNumber = 0U;
    std::size_t cursor = 0U;

    while (cursor <= source.size())
    {
        const std::size_t end = source.find('\n', cursor);
        const std::string_view line = source.substr(
            cursor,
            end == std::string_view::npos ? std::string_view::npos
                                          : end - cursor);
        cursor = end == std::string_view::npos ? source.size() + 1U
                                               : end + 1U;
        ++lineNumber;

        const auto tokens = Tokens(line);
        if (tokens.size() < 2U || tokens[0] != "//" ||
            tokens[1] != "@param")
        {
            continue;
        }

        const auto problem = [&](const std::string& text)
        {
            layout.problems.push_back(
                "line " + std::to_string(lineNumber) + ": " + text);
        };

        if (tokens.size() < 4U)
        {
            problem("@param needs a name, a type and default values.");
            continue;
        }

        ShaderParameterDecl declaration;
        declaration.name = std::string(tokens[2]);

        if (!IsIdentifier(declaration.name))
        {
            problem("'" + declaration.name + "' is not a valid identifier.");
            continue;
        }

        if (layout.Find(declaration.name) != nullptr)
        {
            problem("'" + declaration.name + "' is declared twice.");
            continue;
        }

        const std::string_view type = tokens[3];
        if (type == "float") declaration.components = 1U;
        else if (type == "float2") declaration.components = 2U;
        else if (type == "float3") declaration.components = 3U;
        else if (type == "float4") declaration.components = 4U;
        else if (type == "color3")
        {
            declaration.components = 3U;
            declaration.isColor = true;
        }
        else
        {
            problem(
                "unknown type '" + std::string(type) +
                "' (use float, float2, float3, float4 or color3).");
            continue;
        }

        std::size_t index = 4U;
        bool bad = false;

        for (u32 component = 0U; component < declaration.components;
             ++component, ++index)
        {
            const auto number = index < tokens.size()
                ? ParseNumber(tokens[index])
                : std::nullopt;
            if (!number.has_value())
            {
                problem(
                    "'" + declaration.name + "' needs " +
                    std::to_string(declaration.components) +
                    " numeric default value(s).");
                bad = true;
                break;
            }
            declaration.defaults[component] = *number;
        }

        if (bad)
        {
            continue;
        }

        if (index < tokens.size())
        {
            const auto minimum = index + 2U < tokens.size() &&
                    tokens[index] == "|"
                ? ParseNumber(tokens[index + 1U])
                : std::nullopt;
            const auto maximum = minimum.has_value()
                ? ParseNumber(tokens[index + 2U])
                : std::nullopt;

            if (!minimum.has_value() || !maximum.has_value() ||
                !(*minimum < *maximum))
            {
                problem(
                    "range for '" + declaration.name +
                    "' must be '| <min> <max>' with min < max.");
                continue;
            }

            declaration.hasRange = true;
            declaration.minimum = *minimum;
            declaration.maximum = *maximum;
        }

        if (layout.totalFloats + declaration.components >
            kMaxParameterFloats)
        {
            problem(
                "'" + declaration.name + "' does not fit: a shader can "
                "declare at most " + std::to_string(kMaxParameterFloats) +
                " scalar parameter values (push-constant budget).");
            continue;
        }

        declaration.offset = layout.totalFloats;
        layout.totalFloats += declaration.components;
        layout.parameters.push_back(std::move(declaration));
    }

    return layout;
}

// ---------------------------------------------------------------------------
// HLSL prelude
// ---------------------------------------------------------------------------

std::string_view ShadingPreludeCommon() noexcept
{
    static const std::string prelude = std::string(R"(
// ===== Orbit shading contract v1 (engine-provided) =====
struct OrbitSurface
{
    float3 positionWS;
    float3 normalWS;
    float3 tangentWS;
    float3 bitangentWS;
    float2 uv;
    float3 viewWS;   // unit vector from the surface toward the camera
    float  time;     // seconds
};

struct OrbitLight
{
    float3 directionWS; // unit vector from the surface toward the light
    float3 radiance;    // scene-linear
};

struct OrbitLighting
{
    OrbitLight key;
    OrbitLight fill;
    OrbitLight rim;
    float3 ambientSky;
    float3 ambientGround;
};

static const float ORBIT_PI = 3.14159265359;

float3 OrbitSunDirection(float azimuthRadians, float elevationRadians)
{
    const float ce = cos(elevationRadians);
    return normalize(float3(
        ce * sin(azimuthRadians),
        sin(elevationRadians),
        ce * cos(azimuthRadians)));
}

float OrbitHash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float3 OrbitHash3(float3 p)
{
    return float3(
        OrbitHash(p),
        OrbitHash(p + float3(17.1, 3.7, 9.2)),
        OrbitHash(p + float3(5.3, 21.9, 13.1)));
}

// Sky radiance for a world direction under a lighting preset.
float3 OrbitEnvironmentFor(float3 d, uint preset, float3 sun)
{
    float3 horizon = float3(0.55, 0.58, 0.62);
    float3 zenith = float3(0.30, 0.36, 0.46);
    float3 ground = float3(0.12, 0.11, 0.10);
    float3 sunColor = float3(3.0, 2.9, 2.7);
    float discSharpness = 700.0;

    if (preset == 1u)
    {
        horizon = float3(0.75, 0.82, 0.95);
        zenith = float3(0.22, 0.42, 0.85);
        ground = float3(0.20, 0.17, 0.13);
        sunColor = float3(6.0, 5.6, 4.8);
    }
    else if (preset == 2u)
    {
        horizon = float3(0.78, 0.80, 0.83);
        zenith = float3(0.62, 0.65, 0.70);
        ground = float3(0.30, 0.30, 0.30);
        sunColor = float3(0.0, 0.0, 0.0);
    }
    else if (preset == 3u)
    {
        horizon = float3(1.10, 0.48, 0.20);
        zenith = float3(0.10, 0.14, 0.34);
        ground = float3(0.10, 0.06, 0.05);
        sunColor = float3(9.0, 4.4, 1.6);
        discSharpness = 300.0;
    }
    else if (preset == 4u)
    {
        horizon = float3(0.0, 0.0, 0.0);
        zenith = float3(0.0, 0.0, 0.0);
        ground = float3(0.0, 0.0, 0.0);
        sunColor = float3(12.0, 11.8, 11.2);
        discSharpness = 3000.0;
    }

    float3 sky = d.y >= 0.0
        ? lerp(horizon, zenith, pow(saturate(d.y), 0.55))
        : lerp(horizon, ground, saturate(-d.y * 3.0));

    const float disc = pow(saturate(dot(d, sun)), discSharpness);
    sky += sunColor * disc;

    if (preset == 4u)
    {
        // Sparse stars so the black sky still reads as a sky.
        const float star = step(0.9975, OrbitHash(floor(d * 360.0)));
        sky += star * float3(0.9, 0.95, 1.0) * 0.7;
    }

    return sky;
}

// What is behind the object under the preview's background setting (0 lighting
// environment, 1 dark gradient, 2 neutral gray, 3 checker). The background pass
// draws exactly this, so a glass shader that refracts it distorts what the
// viewer actually sees around the object.
float3 OrbitBackdropFor(float3 d, uint mode, uint preset, float3 sun)
{
    if (mode == 0u)
        return OrbitEnvironmentFor(d, preset, sun);

    if (mode == 1u)
        return lerp(float3(0.006, 0.008, 0.014), float3(0.035, 0.045, 0.070), saturate(d.y * 0.5 + 0.5));

    if (mode == 2u)
        return float3(0.18, 0.18, 0.18);

    // Checker: cells of longitude and latitude on the sphere of directions.
    const float2 cell = floor(float2(
        atan2(d.x, d.z) * (18.0 / ORBIT_PI),
        asin(clamp(d.y, -1.0, 1.0)) * (18.0 / ORBIT_PI)));
    const float parity = fmod(cell.x + cell.y + 128.0, 2.0);
    return lerp(float3(0.05, 0.05, 0.055), float3(0.32, 0.32, 0.34), parity);
}

OrbitLighting OrbitMakeLightingFor(uint preset, float3 sun)
{
    OrbitLighting l;
    const float3 side = normalize(float3(-sun.z, 0.0, sun.x) + 1.0e-4);

    l.key.directionWS = sun;
    l.fill.directionWS =
        normalize(float3(-sun.x, 0.25, -sun.z) + side * 0.6);
    l.rim.directionWS = normalize(float3(-sun.x, 0.55, -sun.z));

    l.key.radiance = float3(0.0, 0.0, 0.0);
    l.fill.radiance = float3(0.0, 0.0, 0.0);
    l.rim.radiance = float3(0.0, 0.0, 0.0);
    l.ambientSky = float3(0.0, 0.0, 0.0);
    l.ambientGround = float3(0.0, 0.0, 0.0);

    if (preset == 0u)
    {
        l.key.radiance = float3(3.0, 2.9, 2.7);
        l.fill.radiance = float3(0.55, 0.65, 0.85);
        l.rim.radiance = float3(1.2, 1.2, 1.3);
        l.ambientSky = float3(0.22, 0.25, 0.30);
        l.ambientGround = float3(0.09, 0.08, 0.07);
    }
    else if (preset == 1u)
    {
        l.key.radiance = float3(5.0, 4.7, 4.1);
        l.ambientSky = float3(0.32, 0.45, 0.75);
        l.ambientGround = float3(0.18, 0.15, 0.12);
    }
    else if (preset == 2u)
    {
        l.key.radiance = float3(0.5, 0.5, 0.5);
        l.ambientSky = float3(0.85, 0.88, 0.92);
        l.ambientGround = float3(0.32, 0.32, 0.32);
    }
    else if (preset == 3u)
    {
        l.key.radiance = float3(6.0, 3.0, 1.2);
        l.fill.radiance = float3(0.25, 0.30, 0.55);
        l.ambientSky = float3(0.20, 0.16, 0.28);
        l.ambientGround = float3(0.07, 0.04, 0.03);
    }
    else
    {
        l.key.radiance = float3(8.0, 7.8, 7.4);
        l.ambientSky = float3(0.003, 0.003, 0.004);
        l.ambientGround = float3(0.0, 0.0, 0.0);
    }

    return l;
}

// Scene-linear HDR -> display: filmic tone map (Narkowicz ACES fit) + gamma.
float3 OrbitDisplayEncode(float3 c)
{
    c = max(c, float3(0.0, 0.0, 0.0));
    const float3 mapped = saturate(
        (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14));
    return pow(mapped, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
}
)");
    return prelude;
}

// ---------------------------------------------------------------------------
// Templates
// ---------------------------------------------------------------------------

namespace
{
constexpr std::string_view kLitTemplate = R"(// Lit: Lambert diffuse + GGX specular, lit by the preset's key/fill/rim.
//
// Parameters are declared with // @param lines. They pack in declaration order
// and are read through the generated Param_<name>() functions.
//
// @param tint      color3 0.80 0.80 0.80
// @param roughness float  0.55 | 0.03 1
// @param metallic  float  0.0  | 0 1

float3 LitLight(OrbitSurface s, OrbitLight L, float3 albedo, float rough, float3 f0)
{
    const float3 n = s.normalWS;
    const float3 v = s.viewWS;
    const float3 h = normalize(L.directionWS + v);
    const float nl = saturate(dot(n, L.directionWS));
    const float nv = saturate(dot(n, v)) + 1.0e-4;
    const float nh = saturate(dot(n, h));
    const float vh = saturate(dot(v, h));

    const float a = rough * rough;
    const float a2 = a * a;
    const float d = nh * nh * (a2 - 1.0) + 1.0;
    const float D = a2 / (ORBIT_PI * d * d);
    const float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    const float G = (nl / (nl * (1.0 - k) + k)) * (nv / (nv * (1.0 - k) + k));
    const float3 F = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);

    const float3 specular = D * G * F / (4.0 * nl * nv + 1.0e-4);
    const float3 diffuse = albedo * (1.0 - F) / ORBIT_PI;
    return (diffuse + specular) * L.radiance * nl;
}

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float3 tint = Param_tint();
    const float rough = clamp(Param_roughness(), 0.03, 1.0);
    const float metal = saturate(Param_metallic());

    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), tint, metal);
    const float3 albedo = tint * (1.0 - metal);

    float3 color = LitLight(s, l.key, albedo, rough, f0)
        + LitLight(s, l.fill, albedo, rough, f0)
        + LitLight(s, l.rim, albedo, rough, f0);

    color += lerp(l.ambientGround, l.ambientSky, s.normalWS.y * 0.5 + 0.5) * albedo;
    color += OrbitEnvironment(reflect(-s.viewWS, s.normalWS))
        * f0 * (1.0 - rough * 0.85);

    return float4(color, 1.0);
}
)";

constexpr std::string_view kUnlitTemplate = R"(// Unlit: a flat emissive colour. Useful as the smallest possible shader.
//
// @param color color3 1.0 0.45 0.10
// @param pulse float  0.0 | 0 1

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float wave = 1.0 - Param_pulse() * (0.5 + 0.5 * sin(s.time * 3.0));
    return float4(Param_color() * wave, 1.0);
}
)";

constexpr std::string_view kNormalsTemplate = R"(// Debug: world-space normals as colour. No parameters.

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    return float4(s.normalWS * 0.5 + 0.5, 1.0);
}
)";

constexpr std::string_view kRegolithTemplate = R"(// Lunar regolith: Lommel-Seeliger photometry with an opposition surge and
// procedural craters. Airless-body shading: a single hard sun, no ambient.
//
// Try the "Space" lighting preset with the Sphere shape.
//
// @param albedo      color3 0.11 0.105 0.10
// @param freshBoost  float  0.80 | 0 3
// @param craters     float  0.65 | 0 1
// @param opposition  float  0.90 | 0 4
// @param oppWidth    float  0.05 | 0.005 0.3

// Returns 0 outside a crater, up to 1 at the centre of the deepest bowl.
float CraterMask(float3 p, float density)
{
    const float scale = 3.2;
    const float3 cell = floor(p * scale);
    float best = 0.0;

    for (int x = -1; x <= 1; ++x)
    for (int y = -1; y <= 1; ++y)
    for (int z = -1; z <= 1; ++z)
    {
        const float3 c = cell + float3(x, y, z);
        const float3 center = c + OrbitHash3(c);
        const float radius = 0.18 + 0.32 * OrbitHash(c + 41.0);
        const float present = step(1.0 - density, OrbitHash(c + 7.0));
        const float d = length(p * scale - center) / radius;
        best = max(best, present * saturate(1.0 - d * d));
    }

    return best;
}

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float3 n = s.normalWS;
    const float3 v = s.viewWS;
    const float3 sun = l.key.directionWS;

    const float crater = CraterMask(normalize(s.positionWS), Param_craters());
    const float fine = OrbitHash(floor(normalize(s.positionWS) * 260.0));
    // Fresh ejecta is the same regolith, less space-weathered: brighter.
    const float3 albedo = lerp(Param_albedo(), Param_albedo() * (1.0 + Param_freshBoost()), crater)
        * (0.85 + 0.3 * fine);

    const float mu0 = saturate(dot(n, sun));
    const float mu = saturate(dot(n, v));
    const float lommelSeeliger = mu0 / (mu0 + mu + 1.0e-4);

    const float phase = acos(clamp(dot(sun, v), -1.0, 1.0));
    const float opposition =
        1.0 + Param_opposition() / (1.0 + tan(phase * 0.5) / max(Param_oppWidth(), 1.0e-4));

    const float3 color = albedo * lommelSeeliger * opposition * l.key.radiance * 0.9
        + albedo * l.ambientSky * 0.5;
    return float4(color, 1.0);
}
)";

constexpr std::string_view kGlassLowTemplate = R"TPL(// Glass — Low (LOD2, the cheap tier). No shape query, no local-space volume
// trace: a single reflection sample and a single naive one-bounce refraction
// sample, blended by Fresnel, tinted by a flat authored `thickness` instead
// of a real path length. That is a fixed 2 OrbitBackdrop samples per pixel
// (a little more with `frost`, see below) on every shape, including Plane
// and Mesh where Glass_Medium/Glass_High have no real geometry to fall back
// on either — this tier never branches on the shape at all. Use it for
// background glass and anything off a hero object; step up to
// Glass_Medium.shade.hlsl for correct edge bending on Sphere/Cube, or
// Glass_High.shade.hlsl for dispersion.
//
// The naive refraction assumes the far face is parallel to the near one (a
// thin-slab approximation, same idea as the Plane/Mesh fallback in the other
// tiers) so it barely deflects the transmitted ray and mostly just tints it —
// correct for a window pane, an approximation everywhere else, and the
// reason this tier is for background objects, not the one glass the camera
// is looking straight at.
//
// @param tint       color3 0.88 0.96 1.00
// @param ior        float  1.50 | 1.0 2.4
// @param absorption float  0.35 | 0 4
// @param thickness  float  0.6 | 0.05 3
// @param frost      float  0.0 | 0 1

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float3 n = s.normalWS;
    const float3 v = s.viewWS;
    const float ior = max(Param_ior(), 1.0);
    const float cosi = saturate(dot(n, v));

    const float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    const float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosi, 5.0);

    // A single extra jitter tap stands in for frost here instead of an
    // averaged blur: cheap, but grainier than Glass_Medium/Glass_High.
    const float frost = Param_frost();
    float3 jitter = float3(0.0, 0.0, 0.0);
    if (frost > 0.01)
        jitter = (OrbitHash3(s.positionWS * 61.0) - 0.5) * frost * 1.6;

    const float3 reflected = OrbitBackdrop(normalize(reflect(-v, n) + jitter));

    const float eta = 1.0 / ior;
    const float3 entered = refract(-v, n, eta);
    float3 exited = dot(entered, entered) < 1.0e-6
        ? reflect(-v, n) // total internal reflection
        : refract(entered, n, ior);
    if (dot(exited, exited) < 1.0e-6)
        exited = reflect(entered, -n);
    float3 transmitted = OrbitBackdrop(normalize(exited + jitter));

    transmitted *= exp(-(1.0 - Param_tint()) * Param_absorption() * Param_thickness());

    float3 color = lerp(transmitted, reflected, fresnel);

    // One cheap highlight instead of the three-light rig the other tiers sum.
    color += l.key.radiance * pow(saturate(dot(n, normalize(l.key.directionWS + v))), 200.0) * (0.25 + fresnel);

    return float4(color, 1.0);
}
)TPL";

constexpr std::string_view kGlassMediumTemplate = R"TPL(// Glass — Medium (LOD1, the balanced tier). Keeps the exact ray-traced
// entry-to-exit path on Sphere and Cube (correct edge bending, correct
// per-pixel absorption thickness) but drops the two priciest parts of
// Glass_High.shade.hlsl: no per-channel dispersion trace (one refraction
// instead of three) and a 3-tap frost blur instead of 6. Worst case that is
// 6 OrbitBackdrop samples per pixel instead of High's 24 — no rainbow
// fringing, but a correctly bent, correctly tinted piece of glass. This is
// the right default for most glass in a scene; reach for Glass_High.shade.hlsl
// only for a hero object and Glass_Low.shade.hlsl for background dressing.
//
// Plane and Mesh have no known back-face to ray-trace against, so this falls
// back to a thin parallel-faced slab there, same as the other tiers: it barely
// deflects the transmitted ray and only tints it by the authored `thickness`.
//
// @param tint       color3 0.88 0.96 1.00
// @param ior        float  1.50 | 1.0 2.4
// @param absorption float  0.35 | 0 4
// @param frost      float  0.0 | 0 1
// @param thickness  float  0.6 | 0.05 3

// Backdrop lookup, blurred by `frost` (3-tap jittered average).
float3 GlassSee(float3 d, float frost, float3 seed)
{
    if (frost < 0.01)
        return OrbitBackdrop(d);

    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        const float3 j = OrbitHash3(seed * 61.0 + (float)i * 11.7) - 0.5;
        sum += OrbitBackdrop(normalize(d + j * frost * 1.6));
    }
    return sum / 3.0;
}

// Exact single-channel refraction through a solid Sphere or Cube (see
// Glass_High.shade.hlsl for the per-channel dispersive version).
float3 GlassThroughSolid(uint shape, float3 posWS, float3 nWS, float3 v, float eta, out float thickness)
{
    const float3 entered = refract(-v, nWS, eta);
    if (dot(entered, entered) < 1.0e-6)
    {
        thickness = 0.0;
        return reflect(-v, nWS);
    }

    const float3 localP = OrbitToLocal(posWS);
    const float3 localD = OrbitToLocal(entered);
    float3 localExit;
    float3 exitN;

    if (shape == 0u) // Sphere: radius 1 (AddSphere).
    {
        thickness = OrbitSphereExitDistance(localP, localD);
        localExit = localP + localD * thickness;
        exitN = normalize(localExit);
    }
    else // Cube: half-extent 0.85 (AddCube).
    {
        thickness = OrbitBoxExitDistance(localP, localD, 0.85);
        localExit = localP + localD * thickness;
        const float3 a = abs(localExit);
        exitN = (a.x >= a.y && a.x >= a.z) ? float3(sign(localExit.x), 0.0, 0.0)
              : (a.y >= a.z)               ? float3(0.0, sign(localExit.y), 0.0)
                                           : float3(0.0, 0.0, sign(localExit.z));
    }

    float3 exited = refract(localD, -exitN, 1.0 / eta);
    if (dot(exited, exited) < 1.0e-6)
        exited = reflect(localD, exitN);
    return OrbitToWorld(exited);
}

// Plane / Mesh fallback: a thin parallel-faced slab (see Glass_High.shade.hlsl).
float3 GlassThroughThin(float3 nWS, float3 v, float eta)
{
    const float3 entered = refract(-v, nWS, eta);
    if (dot(entered, entered) < 1.0e-6)
        return reflect(-v, nWS);
    float3 exited = refract(entered, nWS, 1.0 / eta);
    if (dot(exited, exited) < 1.0e-6)
        exited = reflect(entered, -nWS);
    return exited;
}

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float3 n = s.normalWS;
    const float3 v = s.viewWS;
    const float ior = max(Param_ior(), 1.0);
    const float cosi = saturate(dot(n, v));

    const float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    const float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosi, 5.0);

    const float frost = Param_frost();
    const float3 seed = s.positionWS;
    const uint shape = OrbitShapeId();
    const bool solid = (shape == 0u) || (shape == 2u);

    const float3 reflected = GlassSee(reflect(-v, n), frost, seed);

    const float eta = 1.0 / ior;
    float3 transmitted;
    float thickness;
    if (solid)
    {
        transmitted = GlassSee(GlassThroughSolid(shape, s.positionWS, n, v, eta, thickness), frost, seed);
    }
    else
    {
        transmitted = GlassSee(GlassThroughThin(n, v, eta), frost, seed);
        thickness = Param_thickness();
    }

    const float3 absorb = exp(-(1.0 - Param_tint()) * Param_absorption() * thickness);
    transmitted *= absorb;

    float3 color = lerp(transmitted, reflected, fresnel);

    const float shininess = lerp(300.0, 30.0, frost);
    float3 highlights = float3(0.0, 0.0, 0.0);
    highlights += l.key.radiance * pow(saturate(dot(n, normalize(l.key.directionWS + v))), shininess);
    highlights += l.fill.radiance * pow(saturate(dot(n, normalize(l.fill.directionWS + v))), shininess);
    color += highlights * (0.25 + fresnel);

    return float4(color, 1.0);
}
)TPL";

constexpr std::string_view kGlassHighTemplate = R"TPL(// Glass — High (LOD0, the hero tier). Ray-traces the *exact* entry-to-exit
// path through the medium on the Sphere and Cube preview shapes (they are
// analytic primitives centred on the origin, so OrbitSphereExitDistance /
// OrbitBoxExitDistance give the real path length), traces that path three
// times for per-channel dispersion, and blurs both the reflection and the
// transmission with a 6-tap jittered average for frost. Worst case (frost
// and dispersion both active) that is up to 24 OrbitBackdrop samples per
// pixel — use this tier for a hero shot or a close-up gem, not for a scene
// full of glass. See Glass_Medium.shade.hlsl and Glass_Low.shade.hlsl for
// cheaper tiers, and pick per-material, not by editing this file down.
//
// Plane and Mesh have no known back-face to ray-trace against, so this falls
// back to a thin parallel-faced slab there: parallel faces barely deflect the
// transmitted ray (correctly — a window pane doesn't either), only tinting it
// by the authored `thickness`.
//
// The preview blends opaquely and cannot read the scene behind the object, so
// even the exact path refracts into OrbitBackdrop() — the environment or the
// checker behind the object — not other objects in front of it.
//
// @param tint       color3 0.88 0.96 1.00
// @param ior        float  1.50 | 1.0 2.4
// @param absorption float  0.35 | 0 4
// @param dispersion float  0.015 | 0 0.08
// @param frost      float  0.0 | 0 1
// @param thickness  float  0.6 | 0.05 3

// Backdrop lookup, blurred by `frost` (6-tap jittered average).
float3 GlassSee(float3 d, float frost, float3 seed)
{
    if (frost < 0.01)
        return OrbitBackdrop(d);

    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        const float3 j = OrbitHash3(seed * 61.0 + (float)i * 7.13) - 0.5;
        sum += OrbitBackdrop(normalize(d + j * frost * 1.6));
    }
    return sum / 6.0;
}

// Exact double refraction through a solid Sphere or Cube: bend on entry,
// ray-trace to the real exit point, bend again there. `eta` is n_air/n_glass
// for this colour channel (dispersion perturbs it per channel).
float3 GlassThroughSolid(uint shape, float3 posWS, float3 nWS, float3 v, float eta, out float thickness)
{
    const float3 entered = refract(-v, nWS, eta);
    if (dot(entered, entered) < 1.0e-6)
    {
        thickness = 0.0;
        return reflect(-v, nWS); // total internal reflection on entry
    }

    const float3 localP = OrbitToLocal(posWS);
    const float3 localD = OrbitToLocal(entered);
    float3 localExit;
    float3 exitN; // outward normal at the exit point, in local space

    if (shape == 0u) // Sphere: radius 1 (AddSphere).
    {
        thickness = OrbitSphereExitDistance(localP, localD);
        localExit = localP + localD * thickness;
        exitN = normalize(localExit);
    }
    else // Cube: half-extent 0.85 (AddCube).
    {
        thickness = OrbitBoxExitDistance(localP, localD, 0.85);
        localExit = localP + localD * thickness;
        const float3 a = abs(localExit);
        exitN = (a.x >= a.y && a.x >= a.z) ? float3(sign(localExit.x), 0.0, 0.0)
              : (a.y >= a.z)               ? float3(0.0, sign(localExit.y), 0.0)
                                           : float3(0.0, 0.0, sign(localExit.z));
    }

    // Exit is glass -> air, the reverse of entry's eta.
    float3 exited = refract(localD, -exitN, 1.0 / eta);
    if (dot(exited, exited) < 1.0e-6)
        exited = reflect(localD, exitN); // total internal reflection on exit
    return OrbitToWorld(exited);
}

// Plane / Mesh fallback: no known back-face to ray-trace against, so this
// treats the medium as a thin slab with the entry face's own normal on both
// sides. Parallel faces barely deflect the transmitted ray (correctly, for a
// window pane); `thickness` still drives absorption below.
float3 GlassThroughThin(float3 nWS, float3 v, float eta)
{
    const float3 entered = refract(-v, nWS, eta);
    if (dot(entered, entered) < 1.0e-6)
        return reflect(-v, nWS);
    float3 exited = refract(entered, nWS, 1.0 / eta);
    if (dot(exited, exited) < 1.0e-6)
        exited = reflect(entered, -nWS);
    return exited;
}

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float3 n = s.normalWS;
    const float3 v = s.viewWS;
    const float ior = max(Param_ior(), 1.0);
    const float cosi = saturate(dot(n, v));

    // Fresnel (Schlick with the dielectric F0 from the index of refraction).
    const float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    const float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosi, 5.0);

    const float frost = Param_frost();
    const float3 seed = s.positionWS;
    const uint shape = OrbitShapeId();
    const bool solid = (shape == 0u) || (shape == 2u); // Sphere or Cube

    // Reflection off the front face.
    const float3 reflected = GlassSee(reflect(-v, n), frost, seed);

    // Refraction with per-channel dispersion, and per-channel path length
    // through the medium for absorption.
    const float d = Param_dispersion();
    const float eta = 1.0 / ior;
    float3 transmitted;
    float3 thicknessRGB;
    if (solid)
    {
        transmitted.r = GlassSee(GlassThroughSolid(shape, s.positionWS, n, v, eta * (1.0 - d), thicknessRGB.r), frost, seed).r;
        transmitted.g = GlassSee(GlassThroughSolid(shape, s.positionWS, n, v, eta,             thicknessRGB.g), frost, seed).g;
        transmitted.b = GlassSee(GlassThroughSolid(shape, s.positionWS, n, v, eta * (1.0 + d), thicknessRGB.b), frost, seed).b;
    }
    else
    {
        transmitted.r = GlassSee(GlassThroughThin(n, v, eta * (1.0 - d)), frost, seed).r;
        transmitted.g = GlassSee(GlassThroughThin(n, v, eta),             frost, seed).g;
        transmitted.b = GlassSee(GlassThroughThin(n, v, eta * (1.0 + d)), frost, seed).b;
        thicknessRGB = Param_thickness().xxx;
    }

    // Beer-Lambert absorption over the real path length.
    const float3 absorb = exp(-(1.0 - Param_tint()) * Param_absorption() * thicknessRGB);
    transmitted *= absorb;

    float3 color = lerp(transmitted, reflected, fresnel);

    // Sharp highlights from the rig; frost widens them.
    const float shininess = lerp(400.0, 30.0, frost);
    float3 highlights = float3(0.0, 0.0, 0.0);
    highlights += l.key.radiance * pow(saturate(dot(n, normalize(l.key.directionWS + v))), shininess);
    highlights += l.fill.radiance * pow(saturate(dot(n, normalize(l.fill.directionWS + v))), shininess);
    highlights += l.rim.radiance * pow(saturate(dot(n, normalize(l.rim.directionWS + v))), shininess);
    color += highlights * (0.25 + fresnel);

    // Light focused through the body lands on the far side: a soft bright
    // spot opposite the key light, seen through the glass.
    const float3 focus = normalize(-l.key.directionWS + n * 0.35);
    color += l.key.radiance * Param_tint() * pow(saturate(dot(focus, v)), 20.0)
        * 0.12 * (1.0 - fresnel);

    return float4(color, 1.0);
}
)TPL";

constexpr std::array<std::string_view, 7> kTemplateNames{
    "lit", "unlit", "glass_low", "glass_medium", "glass_high",
    "lunar_regolith", "debug_normals"};
} // namespace

std::span<const std::string_view> ShaderTemplateNames() noexcept
{
    return kTemplateNames;
}

std::optional<std::string_view> ShaderTemplateSource(
    const std::string_view name) noexcept
{
    if (name == "lit") return kLitTemplate;
    if (name == "unlit") return kUnlitTemplate;
    if (name == "glass_low") return kGlassLowTemplate;
    if (name == "glass_medium") return kGlassMediumTemplate;
    if (name == "glass_high") return kGlassHighTemplate;
    if (name == "lunar_regolith") return kRegolithTemplate;
    if (name == "debug_normals") return kNormalsTemplate;
    return std::nullopt;
}
} // namespace orbit::shading
