#include <orbit/weather_lab/WxFormat.hpp>

#include <algorithm>
#include <sstream>

namespace orbit::weather_lab
{
namespace
{
constexpr const char* kMagic = "ORBITWX1";

void SetError(std::string* error, std::string message)
{
    if (error != nullptr)
    {
        *error = std::move(message);
    }
}

template <typename T>
void ReadList(std::istringstream& line, std::vector<T>& out)
{
    out.clear();
    T value{};
    while (line >> value)
    {
        out.push_back(value);
    }
}
} // namespace

int WxHeader::FieldIndex(const std::string& name) const
{
    const auto it = std::find(fields.begin(), fields.end(), name);
    return it == fields.end()
        ? -1
        : static_cast<int>(it - fields.begin());
}

bool WxWriter::Open(
    const std::filesystem::path& path,
    const WxHeader& header,
    std::string* error)
{
    stream_.open(path, std::ios::binary | std::ios::trunc);
    if (!stream_)
    {
        SetError(error, "cannot create " + path.string());
        return false;
    }
    cells_ = header.CellCount();
    fields_ = header.fields.size();
    stream_.precision(9);
    stream_ << kMagic << '\n';
    stream_ << "source " << header.source << '\n';
    stream_ << "nx " << header.nx << "\nny " << header.ny
            << "\nnz " << header.nz << '\n';
    stream_ << "dx " << header.dx << "\ndy " << header.dy << '\n';
    stream_ << "move " << header.moveU << ' ' << header.moveV << '\n';
    stream_ << "zh";
    for (const float z : header.centreHeight)
    {
        stream_ << ' ' << z;
    }
    stream_ << "\ntimes";
    for (const float t : header.times)
    {
        stream_ << ' ' << t;
    }
    stream_ << "\nfields";
    for (const auto& f : header.fields)
    {
        stream_ << ' ' << f;
    }
    stream_ << "\n\n";
    return static_cast<bool>(stream_);
}

bool WxWriter::AppendFrame(const std::vector<const float*>& fieldData)
{
    if (fieldData.size() != fields_)
    {
        return false;
    }
    for (const float* data : fieldData)
    {
        stream_.write(
            reinterpret_cast<const char*>(data),
            static_cast<std::streamsize>(cells_ * sizeof(float)));
    }
    return static_cast<bool>(stream_);
}

void WxWriter::Close()
{
    if (stream_.is_open())
    {
        stream_.close();
    }
}

bool WxReader::Open(const std::filesystem::path& path, std::string* error)
{
    path_ = path;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        SetError(error, "cannot open " + path.string());
        return false;
    }
    std::string line;
    if (!std::getline(in, line) || line != kMagic)
    {
        SetError(error, "not an .orbitwx file: " + path.string());
        return false;
    }
    header_ = {};
    while (std::getline(in, line) && !line.empty())
    {
        std::istringstream tokens(line);
        std::string key;
        tokens >> key;
        if (key == "source") { tokens >> header_.source; }
        else if (key == "nx") { tokens >> header_.nx; }
        else if (key == "ny") { tokens >> header_.ny; }
        else if (key == "nz") { tokens >> header_.nz; }
        else if (key == "dx") { tokens >> header_.dx; }
        else if (key == "dy") { tokens >> header_.dy; }
        else if (key == "move") { tokens >> header_.moveU >> header_.moveV; }
        else if (key == "zh") { ReadList(tokens, header_.centreHeight); }
        else if (key == "times") { ReadList(tokens, header_.times); }
        else if (key == "fields") { ReadList(tokens, header_.fields); }
    }
    if (header_.nx == 0U || header_.ny == 0U || header_.nz == 0U
        || header_.centreHeight.size() != header_.nz
        || header_.fields.empty())
    {
        SetError(error, "incomplete .orbitwx header: " + path.string());
        return false;
    }
    dataOffset_ = static_cast<std::uint64_t>(in.tellg());
    in.seekg(0, std::ios::end);
    const auto bytes = static_cast<std::uint64_t>(in.tellg()) - dataOffset_;
    const std::uint64_t frameBytes = header_.CellCount() * sizeof(float)
        * header_.fields.size();
    const std::uint64_t frames = bytes / frameBytes;
    if (frames < header_.times.size())
    {
        // A run that was interrupted: keep the complete frames.
        header_.times.resize(static_cast<std::size_t>(frames));
    }
    return true;
}

bool WxReader::ReadField(
    const std::size_t frame,
    const std::string& field,
    std::vector<float>& out,
    std::string* error)
{
    const int index = header_.FieldIndex(field);
    if (index < 0 || frame >= header_.times.size())
    {
        SetError(error, "no field '" + field + "' in frame "
            + std::to_string(frame));
        return false;
    }
    const std::size_t cells = header_.CellCount();
    const std::uint64_t offset = dataOffset_
        + (static_cast<std::uint64_t>(frame) * header_.fields.size()
            + static_cast<std::uint64_t>(index))
            * cells * sizeof(float);
    std::ifstream in(path_, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    out.resize(cells);
    in.read(
        reinterpret_cast<char*>(out.data()),
        static_cast<std::streamsize>(cells * sizeof(float)));
    if (!in)
    {
        SetError(error, "short read in " + path_.string());
        return false;
    }
    return true;
}
} // namespace orbit::weather_lab
