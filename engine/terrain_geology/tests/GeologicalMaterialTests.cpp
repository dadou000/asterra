#include <orbit/terrain_geology/GeologicalMaterial.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#ifndef ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR
#error ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR must point at M02 authority assets.
#endif

namespace
{
[[nodiscard]] bool Check(
    const bool condition,
    const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }

    return true;
}

[[nodiscard]] bool NearlyEqual(
    const orbit::f32 a,
    const orbit::f32 b,
    const orbit::f32 epsilon = 1.0e-5F)
{
    return std::abs(a - b) <= epsilon;
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::terrain_geology;

    const std::filesystem::path referenceDirectory{
        ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR
    };

    GeologicalMaterialLibrary library =
        LoadGeologicalMaterialDirectory(
            referenceDirectory);

    bool ok = true;

    ok &= Check(
        library.Size() == 5 &&
            library.Revision() == 5,
        "M02 must load the five initial geological materials from authored authority files.");

    std::unordered_set<RockTypeId> ids;

    for (const GeologicalMaterial& material :
         library.Materials())
    {
        ok &= Check(
            material.IsValid(),
            "Authored reference geological material failed validation.");
        ids.insert(material.id);
    }

    ok &= Check(
        ids.size() == library.Size(),
        "Authored geological materials must have unique stable RockTypeIds.");

    const GeologicalMaterial* basalt =
        library.Find(reference_rock::Basalt);
    const GeologicalMaterial* granite =
        library.Find(reference_rock::Granite);
    const GeologicalMaterial* sandstone =
        library.Find(reference_rock::Sandstone);
    const GeologicalMaterial* limestone =
        library.Find(reference_rock::Limestone);
    const GeologicalMaterial* ash =
        library.Find(reference_rock::VolcanicAsh);

    ok &= Check(
        basalt != nullptr &&
            granite != nullptr &&
            sandstone != nullptr &&
            limestone != nullptr &&
            ash != nullptr,
        "Authored reference records do not match the frozen stable RockTypeIds.");

    ok &= Check(
        library.FindByName("sandstone") ==
            sandstone,
        "Geological material name lookup should be case-insensitive.");

    if (basalt != nullptr &&
        sandstone != nullptr &&
        limestone != nullptr &&
        ash != nullptr)
    {
        const GeologicalErosionForcing forcing{
            .hydraulic = 1.0F,
            .aeolian = 1.0F,
            .chemical = 1.0F,
            .fracture = 1.0F
        };

        const auto basaltResponse =
            EvaluateIntrinsicErosionResponse(
                *basalt,
                forcing);
        const auto sandstoneResponse =
            EvaluateIntrinsicErosionResponse(
                *sandstone,
                forcing);
        const auto ashResponse =
            EvaluateIntrinsicErosionResponse(
                *ash,
                forcing);
        const auto limestoneResponse =
            EvaluateIntrinsicErosionResponse(
                *limestone,
                forcing);

        ok &= Check(
            basaltResponse.hydraulicDetachment <
                sandstoneResponse.hydraulicDetachment &&
            sandstoneResponse.hydraulicDetachment <
                ashResponse.hydraulicDetachment,
            "Identical hydraulic forcing must measurably differentiate authored basalt, sandstone and volcanic ash.");

        ok &= Check(
            basaltResponse.aeolianDetachment <
                sandstoneResponse.aeolianDetachment &&
            sandstoneResponse.aeolianDetachment <
                ashResponse.aeolianDetachment,
            "Identical aeolian forcing must measurably differentiate authored basalt, sandstone and volcanic ash.");

        ok &= Check(
            limestoneResponse.chemicalWeathering >
                basaltResponse.chemicalWeathering,
            "Authored limestone chemical weatherability must remain distinct from basalt under identical forcing.");
    }

    const GeologicalMaterialGpuTable gpu =
        library.BuildGpuTable();

    ok &= Check(
        gpu.materials.size() == 5 &&
            gpu.rockTypes.size() == 5,
        "M02 GPU table must contain one compact entry per authored material.");

    const auto basaltGpuIndex =
        gpu.IndexOf(reference_rock::Basalt);

    if (basalt != nullptr &&
        basaltGpuIndex.has_value())
    {
        const auto& packed =
            gpu.materials[*basaltGpuIndex];

        ok &= Check(
            NearlyEqual(
                packed.hardness,
                basalt->hardness) &&
            NearlyEqual(
                packed.hydraulicErodibility,
                basalt->hydraulicErodibility) &&
            NearlyEqual(
                packed.density,
                basalt->density),
            "CPU to GPU geological material packing changed authored coefficients.");
    }
    else
    {
        ok &= Check(
            false,
            "Basalt is missing from the compact GPU table.");
    }

    if (sandstone != nullptr)
    {
        const std::string serialized =
            SerializeGeologicalMaterialToml(
                *sandstone);

        const GeologicalMaterial roundTrip =
            ParseGeologicalMaterialToml(
                serialized);

        ok &= Check(
            roundTrip.id == sandstone->id &&
            roundTrip.name == sandstone->name &&
            NearlyEqual(
                roundTrip.hardness,
                sandstone->hardness) &&
            NearlyEqual(
                roundTrip.hydraulicErodibility,
                sandstone->hydraulicErodibility) &&
            NearlyEqual(
                roundTrip.chemicalWeatherability,
                sandstone->chemicalWeatherability) &&
            NearlyEqual(
                roundTrip.density,
                sandstone->density),
            "Authored geological material TOML did not round-trip.");

        const GeologicalMaterial directFileLoad =
            LoadGeologicalMaterialFile(
                referenceDirectory /
                "Sandstone.orbitgeologicalmaterial");

        ok &= Check(
            directFileLoad.id == sandstone->id &&
                directFileLoad.name == sandstone->name,
            "Direct authored geological material file loading disagrees with directory loading.");
    }

    const u64 beforeReplace =
        library.Revision();

    if (granite != nullptr)
    {
        GeologicalMaterial edited =
            *granite;
        edited.hardness = 0.81F;
        library.Upsert(edited);

        const GeologicalMaterial* replaced =
            library.Find(reference_rock::Granite);

        ok &= Check(
            library.Size() == 5 &&
                library.Revision() ==
                    beforeReplace + 1 &&
                replaced != nullptr &&
                NearlyEqual(
                    replaced->hardness,
                    0.81F),
            "Editing an authored geological material must replace by stable ID and invalidate derived consumers.");
    }

    bool rejectedInvalid = false;

    try
    {
        GeologicalMaterial invalid{
            .id = {
                .high = 1,
                .low = 1
            },
            .name = "Impossible rock",
            .hardness = 1.25F,
            .cohesion = 0.5F,
            .hydraulicErodibility = 0.5F,
            .aeolianErodibility = 0.5F,
            .permeability = 0.5F,
            .chemicalWeatherability = 0.5F,
            .fractureTendency = 0.5F,
            .density = 2'500.0F
        };

        library.Upsert(
            std::move(invalid));
    }
    catch (const std::invalid_argument&)
    {
        rejectedInvalid = true;
    }

    ok &= Check(
        rejectedInvalid,
        "Out-of-range authored geological coefficients must be rejected before entering authority state.");

    return ok ? 0 : 1;
}
