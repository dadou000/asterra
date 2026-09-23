#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::volume_representation
{
inline constexpr u32 kVolumeCacheSchemaVersion = 1U;
inline constexpr std::string_view kVolumeCacheExtension = ".orbitvol";

struct VolumeCacheBakeSettings
{
    u32 resolution{32U};
    u64 fieldMask{
        static_cast<u64>(world_model::VolumeField::Density) |
        static_cast<u64>(world_model::VolumeField::Emission)};
};

struct VolumeCacheDescriptor
{
    u32 schemaVersion{kVolumeCacheSchemaVersion};
    scene::ObjectId volume{};
    u64 authoredFingerprint{0U};
    u32 resolutionX{0U};
    u32 resolutionY{0U};
    u32 resolutionZ{0U};
    u64 fieldMask{0U};
    math::Double3 centerMeters{};
    math::Double3 halfExtentsMeters{};
};

struct VolumeCacheData
{
    VolumeCacheDescriptor descriptor{};
    std::vector<f32> density;
    std::vector<f32> emission;
    u64 payloadFingerprint{0U};
    std::string sourcePath;

    [[nodiscard]] u64 ByteSize() const noexcept
    {
        return static_cast<u64>(
            density.size() + emission.size()) *
            sizeof(f32);
    }
};

enum class VolumeCacheLoadStatus : u8
{
    Ok = 0U,
    IoError,
    InvalidMagic,
    UnsupportedVersion,
    InvalidDimensions,
    Truncated,
    CorruptPayload
};

struct VolumeCacheLoadResult
{
    VolumeCacheLoadStatus status{VolumeCacheLoadStatus::IoError};
    std::string message;
    std::optional<VolumeCacheData> cache;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return status == VolumeCacheLoadStatus::Ok &&
            cache.has_value();
    }
};

[[nodiscard]] u64 ComputeVolumeAuthoredFingerprint(
    const world_model::ResolvedVolumeDomain& domain,
    std::span<const world_model::ResolvedVolumeInput> inputs,
    const VolumeCacheBakeSettings& settings) noexcept;

[[nodiscard]] VolumeCacheData BakeVolumeCache(
    const world_model::ResolvedVolumeDomain& domain,
    std::span<const world_model::ResolvedVolumeInput> inputs,
    const VolumeCacheBakeSettings& settings = {});

[[nodiscard]] bool SaveVolumeCache(
    std::string_view path,
    const VolumeCacheData& cache,
    std::string* error = nullptr);

[[nodiscard]] VolumeCacheLoadResult LoadVolumeCache(
    std::string_view path);

[[nodiscard]] bool IsVolumeCacheCurrent(
    const VolumeCacheData& cache,
    const world_model::ResolvedVolumeDomain& domain,
    std::span<const world_model::ResolvedVolumeInput> inputs,
    const VolumeCacheBakeSettings& settings,
    std::string* reason = nullptr) noexcept;

[[nodiscard]] f32 SampleVolumeCacheDensity(
    const VolumeCacheData& cache,
    f64 u,
    f64 v,
    f64 w) noexcept;

[[nodiscard]] f32 SampleVolumeCacheEmission(
    const VolumeCacheData& cache,
    f64 u,
    f64 v,
    f64 w) noexcept;

class VolumeCacheRegistry
{
public:
    void Attach(
        scene::ObjectId volume,
        VolumeCacheData cache);

    [[nodiscard]] const VolumeCacheData* Find(
        scene::ObjectId volume) const noexcept;

    [[nodiscard]] bool Has(
        scene::ObjectId volume) const noexcept;

    void Detach(
        scene::ObjectId volume) noexcept;

    void RemoveMissing(
        const scene::ObjectStore& objects);

private:
    std::map<scene::ObjectId, VolumeCacheData> caches_;
};

[[nodiscard]] VolumeCacheRegistry& VolumeCaches() noexcept;

[[nodiscard]] std::string_view VolumeCacheLoadStatusName(
    VolumeCacheLoadStatus status) noexcept;
} // namespace orbit::volume_representation
