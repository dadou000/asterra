#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/terrain_debug/TerrainDebugLivePages.hpp>
#include <orbit/terrain_debug/TerrainDebugPageData.hpp>
#include <orbit/terrain_debug/TerrainDebugSeam.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
using namespace orbit;

static_assert(
    terrain_debug::kRequiredTerrainDebugFieldCount == 21U,
    "M29 Studio acceptance must cover the complete required field catalog.");

void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Studio viewport presentation test failed.\n";
        std::exit(EXIT_FAILURE);
    }
}

terrain_debug::TerrainDebugPageStamp MakeStamp(
    const terrain::PhysicalTerrainPageAddress address)
{
    return {
        .address = address,
        .physicalLod = 2,
        .revisions = {
            .geology = 11,
            .climate = 12,
            .authoring = 13,
            .biome = 14,
            .water = 15,
            .processes = 16
        },
        .cacheResident = true,
        .invalidationRevision = 17
    };
}

std::shared_ptr<terrain_debug::TerrainDebugPageData>
MakeCompleteDebugPage(
    const terrain::PhysicalTerrainPageAddress address)
{
    auto page =
        std::make_shared<
            terrain_debug::TerrainDebugPageData>(
                MakeStamp(address),
                4,
                4);

    const std::vector<f32> scalar(
        16,
        0.0F);
    const std::vector<
        terrain_debug::TerrainDebugVector2>
        vector(
            16,
            {});
    const std::vector<u32> category(
        16,
        7U);

    for (const auto& descriptor :
         terrain_debug::FieldCatalog())
    {
        if (page->Has(descriptor.field))
        {
            continue;
        }

        switch (descriptor.valueClass)
        {
        case terrain_debug::TerrainDebugValueClass::Scalar:
        case terrain_debug::TerrainDebugValueClass::SignedScalar:
            page->SetScalar(
                descriptor.field,
                scalar);
            break;

        case terrain_debug::TerrainDebugValueClass::Vector:
            page->SetVector(
                descriptor.field,
                vector);
            break;

        case terrain_debug::TerrainDebugValueClass::Category:
            page->SetCategory(
                descriptor.field,
                category);
            break;

        case terrain_debug::TerrainDebugValueClass::Boolean:
        case terrain_debug::TerrainDebugValueClass::Revision:
        case terrain_debug::TerrainDebugValueClass::Lod:
            Check(false);
            break;
        }
    }

    return page;
}

void TestPresentationPolicy()
{
    using studio_session::ViewportMode;
    using studio_ui::SelectStudioViewportPresentation;
    using studio_ui::StudioViewportPresentation;

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::Perspective,
            true,
            true,
            false,
            false,
            false) ==
        StudioViewportPresentation::ProductionTerrain);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::Perspective,
            true,
            false,
            false,
            false,
            false) ==
        StudioViewportPresentation::BodyPreview);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::BodyMap,
            true,
            true,
            false,
            false,
            false) ==
        StudioViewportPresentation::BodyPreview);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::BodyMap,
            true,
            false,
            true,
            false,
            false) ==
        StudioViewportPresentation::MacroGlobe);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::BodyMap,
            false,
            false,
            false,
            false,
            false) ==
        StudioViewportPresentation::Blank);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::Debug,
            true,
            false,
            false,
            true,
            true) ==
        StudioViewportPresentation::TerrainDebug);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::Debug,
            true,
            false,
            false,
            false,
            false) ==
        StudioViewportPresentation::TerrainDebugUnavailable);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::Debug,
            true,
            false,
            false,
            true,
            false) ==
        StudioViewportPresentation::TerrainDebugUnavailable);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::Debug,
            false,
            false,
            false,
            true,
            true) ==
        StudioViewportPresentation::TerrainDebug);
}

void TestM29StudioAcceptance()
{
    using studio_session::ViewportMode;
    using studio_ui::SelectStudioViewportPresentation;
    using studio_ui::StudioViewportPresentation;

    const terrain::PhysicalTerrainPageAddress sourceAddress{
        .planet = {
            .high = 0x4D32395354554449ULL,
            .low = 0x4F41434345505401ULL
        },
        .tile = {
            .face = world::CubeFace::PositiveZ,
            .level = 3,
            .x = 3,
            .y = 3
        }
    };

    const auto source =
        MakeCompleteDebugPage(
            sourceAddress);

    terrain_debug::TerrainDebugLivePages live;
    static_cast<void>(
        live.Publish(source));

    for (u8 raw = 0U; raw < 4U; ++raw)
    {
        const auto edge =
            static_cast<world::TileEdge>(raw);

        static_cast<void>(
            live.Publish(
                MakeCompleteDebugPage(
                    terrain_debug::ExpectedNeighbor(
                        sourceAddress,
                        edge))));
    }

    Check(
        terrain_debug::FieldCatalog().size() ==
            terrain_debug::kRequiredTerrainDebugFieldCount);

    for (const auto& descriptor :
         terrain_debug::FieldCatalog())
    {
        Check(!descriptor.upstream.empty());

        for (const auto stage :
             descriptor.upstream)
        {
            Check(
                terrain_debug::StageName(stage) !=
                std::string_view{"Unknown"});
        }

        Check(source->Has(descriptor.field));

        const auto view =
            source->View(
                descriptor.field);
        const auto rgba =
            terrain_debug::
                ComposeTerrainDebugRgba8(
                    view);

        Check(
            rgba.size() ==
                static_cast<std::size_t>(
                    source->Width()) *
                source->Height() *
                4U);

        Check(
            SelectStudioViewportPresentation(
                ViewportMode::Debug,
                true,
                false,
                false,
                true,
                source->Has(
                    descriptor.field)) ==
            StudioViewportPresentation::TerrainDebug);

        const auto seams =
            terrain_debug::InspectTerrainDebugSeams(
                *source,
                descriptor.field,
                live);

        for (const auto& seam :
             seams)
        {
            Check(
                seam.state ==
                terrain_debug::
                    TerrainDebugSeamState::
                        Continuous);
        }
    }
}
} // namespace

int main()
{
    TestPresentationPolicy();
    TestM29StudioAcceptance();

    return EXIT_SUCCESS;
}
