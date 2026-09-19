#include <orbit/terrain_debug/TerrainDebugLivePages.hpp>

#include <mutex>
#include <stdexcept>

namespace orbit::terrain_debug
{
std::shared_ptr<const TerrainDebugPageData>
CaptureLiveTerrainDebugPage(
    const TerrainDebugLivePageInputs& inputs)
{
    TerrainDebugPageStamp stamp{
        .address = inputs.address,
        .physicalLod = inputs.physicalLod,
        .revisions = inputs.revisions,
        .cacheResident = inputs.cacheResident,
        .invalidationRevision =
            inputs.invalidationRevision
    };

    if (!stamp.IsValid())
    {
        throw std::invalid_argument(
            "Live terrain debug capture requires a valid physical page address.");
    }

    if (inputs.width == 0U ||
        inputs.height == 0U)
    {
        throw std::invalid_argument(
            "Live terrain debug capture dimensions must be non-zero.");
    }

    const bool hasBiomeWeights =
        !inputs.dominantBiomeWeights.empty();
    const bool hasBiomeIds =
        !inputs.dominantBiomes.empty();

    if (hasBiomeWeights != hasBiomeIds)
    {
        throw std::invalid_argument(
            "Live terrain debug biome capture requires weights and resolved IDs together.");
    }

    const bool hasAeolianForcing =
        !inputs.aeolianForcing.empty();

    if (hasAeolianForcing !=
        (inputs.aeolian != nullptr))
    {
        throw std::invalid_argument(
            "Live terrain debug aeolian capture requires forcing and result together.");
    }

    if (inputs.scatterRequest == nullptr &&
        !inputs.scatterInstances.empty())
    {
        throw std::invalid_argument(
            "Live terrain debug scatter instances require their canonical page request.");
    }

    auto page =
        std::make_shared<TerrainDebugPageData>(
            stamp,
            inputs.width,
            inputs.height);

    if (inputs.materialColumn != nullptr)
    {
        page->CaptureMaterialColumn(
            *inputs.materialColumn);
    }

    if (inputs.drainage != nullptr)
    {
        page->CaptureDrainage(
            *inputs.drainage);
    }

    if (inputs.hydraulic != nullptr)
    {
        page->CaptureHydraulic(
            *inputs.hydraulic);
    }

    if (inputs.sedimentExchange != nullptr)
    {
        page->CaptureSedimentExchange(
            *inputs.sedimentExchange);
    }

    if (!inputs.macroGeology.empty())
    {
        page->CaptureMacroGeology(
            inputs.macroGeology);
    }

    if (!inputs.stratigraphy.empty())
    {
        page->CaptureStratigraphy(
            inputs.stratigraphy);
    }

    if (inputs.aeolian != nullptr)
    {
        page->CaptureAeolian(
            inputs.aeolianForcing,
            *inputs.aeolian);
    }

    if (hasBiomeWeights)
    {
        page->CaptureBiomeResolution(
            inputs.dominantBiomeWeights,
            inputs.dominantBiomes);
    }

    if (inputs.scatterRequest != nullptr)
    {
        page->CaptureScatterDensity(
            *inputs.scatterRequest,
            inputs.scatterInstances);
    }

    return page;
}

std::size_t TerrainDebugLivePages::AddressHash::operator()(
    const terrain::PhysicalTerrainPageAddress& address) const noexcept
{
    u64 value =
        terrain::StableCombine64(
            0x4d32394c49564550ULL,
            address.planet.high);
    value =
        terrain::StableCombine64(
            value,
            address.planet.low);
    value =
        terrain::StableCombine64(
            value,
            static_cast<u64>(
                address.tile.face));
    value =
        terrain::StableCombine64(
            value,
            address.tile.level);
    value =
        terrain::StableCombine64(
            value,
            address.tile.x);
    value =
        terrain::StableCombine64(
            value,
            address.tile.y);

    return static_cast<std::size_t>(value);
}

void TerrainDebugLivePages::Publish(
    std::shared_ptr<const TerrainDebugPageData> page)
{
    if (page == nullptr ||
        !page->Stamp().IsValid())
    {
        throw std::invalid_argument(
            "Terrain debug live-page publication requires a valid immutable page.");
    }

    const auto address =
        page->Stamp().address;

    std::unique_lock lock(mutex_);
    pages_.insert_or_assign(
        address,
        std::move(page));
}

std::shared_ptr<const TerrainDebugPageData>
TerrainDebugLivePages::Find(
    const terrain::PhysicalTerrainPageAddress& address) const
{
    std::shared_lock lock(mutex_);

    const auto found =
        pages_.find(address);

    return found == pages_.end()
        ? nullptr
        : found->second;
}

bool TerrainDebugLivePages::Erase(
    const terrain::PhysicalTerrainPageAddress& address)
{
    std::unique_lock lock(mutex_);
    return pages_.erase(address) > 0U;
}

void TerrainDebugLivePages::Clear()
{
    std::unique_lock lock(mutex_);
    pages_.clear();
}

std::size_t TerrainDebugLivePages::Size() const
{
    std::shared_lock lock(mutex_);
    return pages_.size();
}
} // namespace orbit::terrain_debug
