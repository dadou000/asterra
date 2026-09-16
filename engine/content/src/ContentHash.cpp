#include <orbit/content/ContentHash.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace orbit::content
{
namespace
{
constexpr std::array<u32, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

[[nodiscard]] constexpr u32 RotateRight(
    const u32 value,
    const u32 bits) noexcept
{
    return std::rotr(
        value,
        static_cast<int>(
            bits));
}

[[nodiscard]] u32 ReadBigEndian(
    const std::byte* bytes) noexcept
{
    return
        (static_cast<u32>(
             std::to_integer<u8>(bytes[0]))
         << 24U) |
        (static_cast<u32>(
             std::to_integer<u8>(bytes[1]))
         << 16U) |
        (static_cast<u32>(
             std::to_integer<u8>(bytes[2]))
         << 8U) |
        static_cast<u32>(
            std::to_integer<u8>(bytes[3]));
}

void WriteBigEndian(
    std::byte* destination,
    const u32 value) noexcept
{
    destination[0] =
        static_cast<std::byte>(
            (value >> 24U) & 0xffU);
    destination[1] =
        static_cast<std::byte>(
            (value >> 16U) & 0xffU);
    destination[2] =
        static_cast<std::byte>(
            (value >> 8U) & 0xffU);
    destination[3] =
        static_cast<std::byte>(
            value & 0xffU);
}

class Sha256
{
public:
    void Update(
        std::span<const std::byte> input)
    {
        if (finalized_)
        {
            throw std::logic_error(
                "SHA-256 has already been finalized.");
        }

        if (input.size() >
            (std::numeric_limits<u64>::max() -
             totalBytes_))
        {
            throw std::overflow_error(
                "SHA-256 input length overflow.");
        }

        totalBytes_ +=
            static_cast<u64>(
                input.size());

        std::size_t offset = 0;

        if (bufferSize_ != 0)
        {
            const std::size_t copyCount =
                (std::min)(
                    input.size(),
                    buffer_.size() -
                        bufferSize_);

            std::copy_n(
                input.data(),
                copyCount,
                buffer_.data() +
                    bufferSize_);

            bufferSize_ +=
                copyCount;
            offset +=
                copyCount;

            if (bufferSize_ ==
                buffer_.size())
            {
                Transform(
                    buffer_.data());
                bufferSize_ = 0;
            }
        }

        while (input.size() - offset >=
               buffer_.size())
        {
            Transform(
                input.data() +
                offset);

            offset +=
                buffer_.size();
        }

        const std::size_t remaining =
            input.size() -
            offset;

        if (remaining != 0)
        {
            std::copy_n(
                input.data() +
                    offset,
                remaining,
                buffer_.data());

            bufferSize_ =
                remaining;
        }
    }

    [[nodiscard]] ContentHash Finalize()
    {
        if (finalized_)
        {
            throw std::logic_error(
                "SHA-256 has already been finalized.");
        }

        finalized_ = true;

        if (totalBytes_ >
            std::numeric_limits<u64>::max() /
                8ULL)
        {
            throw std::overflow_error(
                "SHA-256 bit length overflow.");
        }

        const u64 bitLength =
            totalBytes_ * 8ULL;

        buffer_[bufferSize_++] =
            std::byte{0x80};

        if (bufferSize_ > 56U)
        {
            std::fill(
                buffer_.begin() +
                    static_cast<std::ptrdiff_t>(
                        bufferSize_),
                buffer_.end(),
                std::byte{0});

            Transform(
                buffer_.data());

            bufferSize_ = 0;
        }

        std::fill(
            buffer_.begin() +
                static_cast<std::ptrdiff_t>(
                    bufferSize_),
            buffer_.begin() + 56,
            std::byte{0});

        for (std::size_t index = 0;
             index < 8U;
             ++index)
        {
            buffer_[56U + index] =
                static_cast<std::byte>(
                    (bitLength >>
                     static_cast<u32>(
                         56U -
                         index * 8U)) &
                    0xffULL);
        }

        Transform(
            buffer_.data());

        std::array<std::byte, ContentHash::Size>
            digest{};

        for (std::size_t index = 0;
             index < state_.size();
             ++index)
        {
            WriteBigEndian(
                digest.data() +
                    index * 4U,
                state_[index]);
        }

        return ContentHash(
            digest);
    }

private:
    void Transform(
        const std::byte* block)
    {
        std::array<u32, 64>
            schedule{};

        for (std::size_t index = 0;
             index < 16U;
             ++index)
        {
            schedule[index] =
                ReadBigEndian(
                    block +
                    index * 4U);
        }

        for (std::size_t index = 16U;
             index < schedule.size();
             ++index)
        {
            const u32 previous15 =
                schedule[index - 15U];
            const u32 previous2 =
                schedule[index - 2U];

            const u32 sigma0 =
                RotateRight(
                    previous15,
                    7U) ^
                RotateRight(
                    previous15,
                    18U) ^
                (previous15 >> 3U);

            const u32 sigma1 =
                RotateRight(
                    previous2,
                    17U) ^
                RotateRight(
                    previous2,
                    19U) ^
                (previous2 >> 10U);

            schedule[index] =
                schedule[index - 16U] +
                sigma0 +
                schedule[index - 7U] +
                sigma1;
        }

        u32 a = state_[0];
        u32 b = state_[1];
        u32 c = state_[2];
        u32 d = state_[3];
        u32 e = state_[4];
        u32 f = state_[5];
        u32 g = state_[6];
        u32 h = state_[7];

        for (std::size_t index = 0;
             index < 64U;
             ++index)
        {
            const u32 sum1 =
                RotateRight(e, 6U) ^
                RotateRight(e, 11U) ^
                RotateRight(e, 25U);

            const u32 choose =
                (e & f) ^
                ((~e) & g);

            const u32 temporary1 =
                h +
                sum1 +
                choose +
                kRoundConstants[index] +
                schedule[index];

            const u32 sum0 =
                RotateRight(a, 2U) ^
                RotateRight(a, 13U) ^
                RotateRight(a, 22U);

            const u32 majority =
                (a & b) ^
                (a & c) ^
                (b & c);

            const u32 temporary2 =
                sum0 +
                majority;

            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 +
                temporary2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<u32, 8> state_{
        0x6a09e667U,
        0xbb67ae85U,
        0x3c6ef372U,
        0xa54ff53aU,
        0x510e527fU,
        0x9b05688cU,
        0x1f83d9abU,
        0x5be0cd19U
    };

    std::array<std::byte, 64>
        buffer_{};
    std::size_t bufferSize_{0};
    u64 totalBytes_{0};
    bool finalized_{false};
};
} // namespace

ContentHash::ContentHash(
    std::array<std::byte, Size> bytes) noexcept
    : bytes_(
          std::move(bytes))
{
}

const std::array<std::byte, ContentHash::Size>&
ContentHash::Bytes() const noexcept
{
    return bytes_;
}

std::string ContentHash::ToHex() const
{
    std::ostringstream stream;
    stream << std::hex
           << std::setfill('0');

    for (const std::byte value :
         bytes_)
    {
        stream
            << std::setw(2)
            << static_cast<unsigned int>(
                   std::to_integer<u8>(
                       value));
    }

    return stream.str();
}

ContentHash HashBytes(
    const std::span<const std::byte> bytes)
{
    Sha256 hash;
    hash.Update(
        bytes);
    return hash.Finalize();
}

ContentHash HashString(
    const std::string_view text)
{
    return HashBytes(
        std::as_bytes(
            std::span(
                text.data(),
                text.size())));
}

ContentHash HashFile(
    const std::filesystem::path& path)
{
    std::ifstream input(
        path,
        std::ios::binary);

    if (!input)
    {
        throw std::runtime_error(
            "Unable to open content source for hashing: " +
            path.string());
    }

    constexpr std::size_t
        kChunkBytes =
            1024U * 1024U;

    std::array<std::byte, kChunkBytes>
        buffer{};

    Sha256 hash;

    while (input)
    {
        input.read(
            reinterpret_cast<char*>(
                buffer.data()),
            static_cast<std::streamsize>(
                buffer.size()));

        const std::streamsize count =
            input.gcount();

        if (count > 0)
        {
            hash.Update(
                std::span(
                    buffer.data(),
                    static_cast<std::size_t>(
                        count)));
        }
    }

    if (!input.eof())
    {
        throw std::runtime_error(
            "Unable to read content source for hashing: " +
            path.string());
    }

    return hash.Finalize();
}
} // namespace orbit::content
