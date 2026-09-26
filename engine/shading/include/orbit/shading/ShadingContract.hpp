#pragma once

#include <orbit/core/Types.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Shading tab's public vocabulary: preview shapes, lighting presets,
// backgrounds, the HLSL contract a shader is written against, and the
// `// @param` annotations that declare a shader's parameters.
//
// Nothing here touches the GPU or the content service, so it is shared by the
// UI, the RPC/MCP surface and the tests. See docs/ORBIT_SHADING.md.
namespace orbit::shading
{
enum class PreviewShape : u8
{
    Sphere,
    Plane,
    Cube,
    // A Wavefront .obj asset from Content, chosen in PreviewState::mesh.
    Mesh
};

enum class LightingPreset : u8
{
    Studio,
    Sun,
    Overcast,
    Sunset,
    Space
};

enum class PreviewBackground : u8
{
    Environment,
    Gradient,
    Gray,
    Checker
};

struct EnumEntry
{
    u8 value{0};
    std::string_view key;
    std::string_view label;
};

[[nodiscard]] std::span<const EnumEntry> Shapes() noexcept;
[[nodiscard]] std::span<const EnumEntry> LightingPresets() noexcept;
[[nodiscard]] std::span<const EnumEntry> Backgrounds() noexcept;

[[nodiscard]] std::optional<PreviewShape> ParseShape(
    std::string_view key) noexcept;
[[nodiscard]] std::optional<LightingPreset> ParseLightingPreset(
    std::string_view key) noexcept;
[[nodiscard]] std::optional<PreviewBackground> ParseBackground(
    std::string_view key) noexcept;

[[nodiscard]] std::string_view ShapeKey(PreviewShape value) noexcept;
[[nodiscard]] std::string_view LightingKey(LightingPreset value) noexcept;
[[nodiscard]] std::string_view BackgroundKey(PreviewBackground value) noexcept;

struct PreviewCamera
{
    f32 yawRadians{0.65F};
    f32 pitchRadians{0.22F};
    f32 distance{3.4F};
    f32 verticalFovRadians{0.75F};
};

struct PreviewState
{
    PreviewShape shape{PreviewShape::Sphere};
    // Project-relative path of the .obj shown by PreviewShape::Mesh. Empty (or
    // a mesh that failed to load) draws a sphere instead.
    std::string mesh;
    LightingPreset lighting{LightingPreset::Studio};
    PreviewBackground background{PreviewBackground::Environment};
    PreviewCamera camera{};
    // Direction of the key/sun light, used by every preset that has one.
    f32 sunAzimuthDegrees{-35.0F};
    f32 sunElevationDegrees{38.0F};
    f32 exposure{1.0F};
    // Spin of the previewed object about its vertical axis.
    f32 modelYawDegrees{0.0F};
    // When true the preview clock advances so time-based shaders animate.
    bool animate{true};
};

// The push-constant budget bounds how many scalar parameters a shader can
// declare (see ShaderParameterLayout). It is an engine limit, not a UI one.
inline constexpr u32 kMaxParameterFloats = 8U;

struct ShaderParameterDecl
{
    std::string name;
    // 1 (float) to 4 (float4). `color3` is a float3 for the purposes of
    // packing and is shown as a colour where the UI supports it.
    u32 components{1U};
    bool isColor{false};
    std::array<f64, 4> defaults{};
    bool hasRange{false};
    f64 minimum{0.0};
    f64 maximum{1.0};
    // First scalar slot this parameter occupies in the packed block.
    u32 offset{0U};
};

struct ShaderParameterLayout
{
    std::vector<ShaderParameterDecl> parameters;
    u32 totalFloats{0U};
    // One line per problem (unknown type, duplicate name, budget exceeded...),
    // each prefixed with the source line number.
    std::vector<std::string> problems;

    [[nodiscard]] const ShaderParameterDecl* Find(
        std::string_view name) const noexcept;
};

// Parses `// @param <name> <type> <defaults...> [| <min> <max>]` lines.
//
//   // @param tint    color3 0.8 0.8 0.8
//   // @param rough   float  0.6 | 0 1
//
// Types: float, float2, float3, float4, color3. Parameters pack in declaration
// order into kMaxParameterFloats scalar slots.
[[nodiscard]] ShaderParameterLayout ParseShaderParameters(
    std::string_view source);

// HLSL that precedes user code (types, lighting presets, environment, tone
// mapping) and is shared by the object and background pixel shaders.
[[nodiscard]] std::string_view ShadingPreludeCommon() noexcept;

// Names of the shader templates offered by "New Shader".
[[nodiscard]] std::span<const std::string_view> ShaderTemplateNames() noexcept;
// Source for a template; nullopt for an unknown name.
[[nodiscard]] std::optional<std::string_view> ShaderTemplateSource(
    std::string_view name) noexcept;
} // namespace orbit::shading
