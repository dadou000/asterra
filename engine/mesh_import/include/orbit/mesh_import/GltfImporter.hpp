#pragma once

#include <orbit/mesh_import/MeshAsset.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>

namespace orbit::mesh_import
{
// Thrown for a file that cannot be imported at all (not glTF, truncated,
// requires Draco/meshopt, no geometry). Recoverable problems become
// MeshAsset::warnings instead.
class MeshImportError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct ImportOptions
{
    // Triangle budget guard: a primitive set larger than this is refused
    // instead of exhausting memory.
    u64 maximumTriangles{64'000'000ULL};
    // Largest single image accepted, in encoded bytes.
    u64 maximumImageBytes{256ULL * 1024ULL * 1024ULL};
};

// Imports a glTF 2.0 file (.glb or .gltf with external/data-URI buffers).
// Walks the default scene's node hierarchy, bakes node transforms into the
// vertices, generates missing normals/tangents, and groups geometry by
// material. Supports sparse accessors, normalized/quantized attributes,
// triangle strips and fans, KHR_materials_emissive_strength and
// KHR_mesh_quantization; ignores (with a warning) other extensions.
[[nodiscard]] MeshAsset ImportGltfFile(
    const std::filesystem::path& path,
    const ImportOptions& options = {});

// Imports a .glb held in memory. baseDirectory resolves external URIs in
// the rare .gltf passed this way; it may be empty for a self-contained GLB.
[[nodiscard]] MeshAsset ImportGltfMemory(
    std::span<const std::byte> bytes,
    const std::filesystem::path& baseDirectory = {},
    const ImportOptions& options = {});
} // namespace orbit::mesh_import
