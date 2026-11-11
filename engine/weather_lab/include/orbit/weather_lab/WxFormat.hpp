#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// .orbitwx: the weather-lab interchange format shared by the CM1 exporter
// (tools/weather_lab/cm1_lab.py), the fast storm solver and the comparison
// tooling.
//
//   ORBITWX1\n
//   key value...\n          (ASCII header lines, blank line terminates)
//   <frames>                (frame-major, then field, float32 little-endian,
//                            x fastest, then y, then z)
//
// Required keys: nx ny nz dx dy, zh (nz mass-level heights), times, fields.
// Optional: source, move (frame translation u v), note.

namespace orbit::weather_lab
{
struct WxHeader
{
    std::string source;
    std::uint32_t nx = 0;
    std::uint32_t ny = 0;
    std::uint32_t nz = 0;
    float dx = 0.0F;
    float dy = 0.0F;
    float moveU = 0.0F;
    float moveV = 0.0F;
    std::vector<float> centreHeight;
    std::vector<float> times;
    std::vector<std::string> fields;

    [[nodiscard]] std::size_t CellCount() const
    {
        return static_cast<std::size_t>(nx) * ny * nz;
    }
    [[nodiscard]] int FieldIndex(const std::string& name) const;
};

class WxWriter
{
public:
    // Creates the file and writes the header; frames are appended with
    // AppendFrame in the header's field order.
    [[nodiscard]] bool Open(
        const std::filesystem::path& path,
        const WxHeader& header,
        std::string* error = nullptr);
    [[nodiscard]] bool AppendFrame(
        const std::vector<const float*>& fieldData);
    void Close();

private:
    std::ofstream stream_;
    std::size_t cells_ = 0;
    std::size_t fields_ = 0;
};

class WxReader
{
public:
    [[nodiscard]] bool Open(
        const std::filesystem::path& path, std::string* error = nullptr);

    [[nodiscard]] const WxHeader& Header() const { return header_; }
    [[nodiscard]] std::size_t FrameCount() const
    {
        return header_.times.size();
    }
    // Reads one field of one frame into `out` (resized to CellCount()).
    [[nodiscard]] bool ReadField(
        std::size_t frame,
        const std::string& field,
        std::vector<float>& out,
        std::string* error = nullptr);

private:
    std::filesystem::path path_;
    WxHeader header_;
    std::uint64_t dataOffset_ = 0;
};
} // namespace orbit::weather_lab
