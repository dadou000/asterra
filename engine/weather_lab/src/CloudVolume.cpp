#include <orbit/weather_lab/CloudVolume.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::weather_lab
{
const char* VolumeUpAxisName(const VolumeUpAxis axis) noexcept
{
    switch (axis)
    {
    case VolumeUpAxis::X: return "x";
    case VolumeUpAxis::Y: return "y";
    case VolumeUpAxis::Z: return "z";
    }
    return "y";
}

bool ParseVolumeUpAxis(const std::string& text, VolumeUpAxis& out)
{
    if (text == "x") { out = VolumeUpAxis::X; return true; }
    if (text == "y") { out = VolumeUpAxis::Y; return true; }
    if (text == "z") { out = VolumeUpAxis::Z; return true; }
    return false;
}

CloudVolumeGrid BuildCloudVolumeGrid(
    const WxHeader& h,
    const std::vector<float>& condensate,
    const CloudVolumeRequest& request)
{
    CloudVolumeGrid grid;
    if (h.nx == 0U || h.ny == 0U || h.nz < 2U || condensate.size() != h.CellCount()
        || h.centreHeight.size() != h.nz)
    {
        grid.error = "no complete storm frame to convert";
        return grid;
    }
    const double dz = static_cast<double>(h.centreHeight[1] - h.centreHeight[0]);
    const double simX = static_cast<double>(h.nx) * h.dx;
    const double simY = static_cast<double>(h.ny) * h.dy;
    const double simZ = static_cast<double>(h.nz) * dz;

    // Cache axis (x, y, z) sources: which simulation axis feeds each.
    // sim axes: 0 = east (x), 1 = north (y), 2 = up (z).
    int source[3] = {0, 1, 2};
    switch (request.upAxis)
    {
    case VolumeUpAxis::Z: break;
    case VolumeUpAxis::Y: source[0] = 0; source[1] = 2; source[2] = 1; break;
    case VolumeUpAxis::X: source[0] = 2; source[1] = 0; source[2] = 1; break;
    }
    const std::uint32_t simRes[3] = {h.nx, h.ny, h.nz};
    const double simSize[3] = {simX, simY, simZ};
    grid.resolutionX = simRes[source[0]];
    grid.resolutionY = simRes[source[1]];
    grid.resolutionZ = simRes[source[2]];
    grid.sizeX = simSize[source[0]];
    grid.sizeY = simSize[source[1]];
    grid.sizeZ = simSize[source[2]];
    grid.density.assign(h.CellCount(), 0.0F);

    const float gain = std::max(request.gainPerGramPerKg, 0.0F);
    float maxGrams = 0.0F;
    for (std::uint32_t sz = 0; sz < h.nz; ++sz)
    {
        for (std::uint32_t sy = 0; sy < h.ny; ++sy)
        {
            for (std::uint32_t sx = 0; sx < h.nx; ++sx)
            {
                const float grams = std::max(
                    condensate[(static_cast<std::size_t>(sz) * h.ny + sy) * h.nx + sx], 0.0F)
                    * 1000.0F;
                maxGrams = std::max(maxGrams, grams);
                const std::uint32_t sim[3] = {sx, sy, sz};
                const std::uint32_t cx = sim[source[0]];
                const std::uint32_t cy = sim[source[1]];
                const std::uint32_t cz = sim[source[2]];
                grid.density[(static_cast<std::size_t>(cz) * grid.resolutionY + cy)
                    * grid.resolutionX + cx] = 1.0F - std::exp(-gain * grams);
            }
        }
    }
    grid.maxCondensateGramsPerKg = maxGrams;
    grid.valid = true;
    return grid;
}
} // namespace orbit::weather_lab
