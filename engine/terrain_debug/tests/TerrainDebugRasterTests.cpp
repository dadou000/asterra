#include <orbit/terrain_debug/TerrainDebugRaster.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M29 raster failure: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        Fail(message);
    }
}

void TestEveryRequiredFieldComposes()
{
    const std::array<f32, 4> scalar{
        -1.0F, 0.0F, 0.5F, 2.0F};
    const std::array<terrain_debug::TerrainDebugVector2, 4> vector{{
        {1.0F, 0.0F},
        {0.0F, 1.0F},
        {-1.0F, 0.0F},
        {0.0F, -2.0F}
    }};
    const std::array<u32, 4> category{
        1U, 2U, 1U, 4U};
    const std::array<u8, 4> boolean{
        0U, 1U, 1U, 0U};
    const std::array<u64, 4> revision{
        1U, 2U, 9U, 12U};
    const std::array<u8, 4> lod{
        0U, 1U, 2U, 3U};

    for (const auto& descriptor :
         terrain_debug::FieldCatalog())
    {
        terrain_debug::TerrainDebugRasterView view{
            .field = descriptor.field,
            .width = 2,
            .height = 2
        };

        switch (descriptor.valueClass)
        {
        case terrain_debug::TerrainDebugValueClass::Scalar:
        case terrain_debug::TerrainDebugValueClass::SignedScalar:
            view.scalar = scalar;
            break;
        case terrain_debug::TerrainDebugValueClass::Vector:
            view.vector = vector;
            break;
        case terrain_debug::TerrainDebugValueClass::Category:
            view.category = category;
            break;
        case terrain_debug::TerrainDebugValueClass::Boolean:
            view.boolean = boolean;
            break;
        case terrain_debug::TerrainDebugValueClass::Revision:
            view.revision = revision;
            break;
        case terrain_debug::TerrainDebugValueClass::Lod:
            view.lod = lod;
            break;
        }

        const auto rgba =
            terrain_debug::ComposeTerrainDebugRgba8(view);

        Require(
            rgba.size() == 16U,
            "Every required debug field must compose one RGBA8 texel per source texel.");
    }
}

void TestSignedScalarDivergesAroundZero()
{
    const std::array<f32, 3> values{
        -2.0F, 0.0F, 2.0F};

    const auto rgba =
        terrain_debug::ComposeTerrainDebugRgba8({
            .field =
                terrain_debug::TerrainDebugField::
                    ErosionDeposition,
            .width = 3,
            .height = 1,
            .scalar = values
        });

    Require(
        rgba[2] == 255U &&
        rgba[8] == 255U,
        "Negative and positive signed extremes must remain visually distinct.");

    Require(
        rgba[4] >= 250U &&
        rgba[5] >= 250U &&
        rgba[6] >= 250U,
        "Zero must map to the neutral center of the signed debug palette.");
}

void TestCategoryMappingIsStable()
{
    const std::array<u32, 4> categories{
        7U, 7U, 11U, 7U};

    const terrain_debug::TerrainDebugRasterView view{
        .field =
            terrain_debug::TerrainDebugField::
                BedrockType,
        .width = 4,
        .height = 1,
        .category = categories
    };

    const auto first =
        terrain_debug::ComposeTerrainDebugRgba8(view);
    const auto second =
        terrain_debug::ComposeTerrainDebugRgba8(view);

    Require(
        first == second,
        "Category debug colors must be deterministic.");

    Require(
        first[0] == first[4] &&
        first[1] == first[5] &&
        first[2] == first[6] &&
        first[0] == first[12] &&
        first[1] == first[13] &&
        first[2] == first[14],
        "Equal category identities must map to equal colors.");
}

void TestBrokenNumericStateIsVisible()
{
    const std::array<f32, 2> values{
        1.0F,
        std::numeric_limits<f32>::quiet_NaN()
    };

    const auto rgba =
        terrain_debug::ComposeTerrainDebugRgba8({
            .field =
                terrain_debug::TerrainDebugField::
                    Moisture,
            .width = 2,
            .height = 1,
            .scalar = values
        });

    Require(
        rgba[4] == 255U &&
        rgba[5] == 0U &&
        rgba[6] == 255U,
        "Non-finite process data must render as an explicit diagnostic color.");
}

void TestRejectsMismatchedBindings()
{
    const std::array<f32, 3> values{
        0.0F, 1.0F, 2.0F};

    bool threw = false;

    try
    {
        static_cast<void>(
            terrain_debug::ComposeTerrainDebugRgba8({
                .field =
                    terrain_debug::TerrainDebugField::
                        Soil,
                .width = 2,
                .height = 2,
                .scalar = values
            }));
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }

    Require(
        threw,
        "A debug page binding must not silently accept a source with the wrong texel count.");
}
} // namespace

int main()
{
    TestEveryRequiredFieldComposes();
    TestSignedScalarDivergesAroundZero();
    TestCategoryMappingIsStable();
    TestBrokenNumericStateIsVisible();
    TestRejectsMismatchedBindings();

    std::cout << "Orbit M29 debug raster tests passed.\n";
    return EXIT_SUCCESS;
}
