#pragma once

#include <orbit/terrain/BakedRivers.hpp>
#include <orbit/terrain/BakedTectonics.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace orbit::terrain_bake
{
// Everything a planet bake holds. One file, one container: the tectonic
// rasters and the global river graph (own section, optional on load so older
// files stay readable), so a planet's baked terrain structure is one artifact.
struct PlanetBakeContents
{
    std::shared_ptr<const terrain::BakedTectonicRasters> tectonics;
    std::shared_ptr<const terrain::BakedRiverNetwork> rivers;
};

// Section container: magic, version, then {tag, byte size, FNV-1a checksum,
// payload} sections. Unknown sections are skipped on load so newer files stay
// readable by older builds up to the sections they understand.
//
// Save writes to a temporary file and renames it over `path`, so a crash
// mid-save never leaves a truncated bake behind.
void SavePlanetBake(
    const std::filesystem::path& path,
    const PlanetBakeContents& contents);

// Returns nullopt (and fills `error`) for a missing, truncated, corrupt or
// unsupported file; never throws for bad data.
[[nodiscard]] std::optional<PlanetBakeContents> LoadPlanetBake(
    const std::filesystem::path& path,
    std::string* error = nullptr);
} // namespace orbit::terrain_bake
