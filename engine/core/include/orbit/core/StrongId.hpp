#pragma once

#include <orbit/core/Types.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace orbit::core
{
template <typename Tag>
struct StrongId
{
    u64 high{0};
    u64 low{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return high != 0 || low != 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return IsValid();
    }

    [[nodiscard]] constexpr bool operator==(
        const StrongId&) const noexcept = default;

    [[nodiscard]] constexpr auto operator<=>(
        const StrongId&) const noexcept = default;

    [[nodiscard]] std::string ToString() const
    {
        constexpr char digits[] =
            "0123456789abcdef";

        std::array<char, 36> result{};
        const std::array<u8, 16> bytes{
            static_cast<u8>(high >> 56),
            static_cast<u8>(high >> 48),
            static_cast<u8>(high >> 40),
            static_cast<u8>(high >> 32),
            static_cast<u8>(high >> 24),
            static_cast<u8>(high >> 16),
            static_cast<u8>(high >> 8),
            static_cast<u8>(high),
            static_cast<u8>(low >> 56),
            static_cast<u8>(low >> 48),
            static_cast<u8>(low >> 40),
            static_cast<u8>(low >> 32),
            static_cast<u8>(low >> 24),
            static_cast<u8>(low >> 16),
            static_cast<u8>(low >> 8),
            static_cast<u8>(low)
        };

        std::size_t out = 0;
        for (std::size_t index = 0;
             index < bytes.size();
             ++index)
        {
            if (index == 4 ||
                index == 6 ||
                index == 8 ||
                index == 10)
            {
                result[out++] = '-';
            }

            result[out++] =
                digits[(bytes[index] >> 4) & 0x0F];
            result[out++] =
                digits[bytes[index] & 0x0F];
        }

        return std::string(
            result.data(),
            result.size());
    }

    [[nodiscard]] static std::optional<StrongId>
    Parse(std::string_view text) noexcept
    {
        std::array<u8, 16> bytes{};
        std::size_t nibbleIndex = 0;

        auto hexValue =
            [](const char value) noexcept -> int
            {
                if (value >= '0' && value <= '9')
                {
                    return value - '0';
                }

                if (value >= 'a' && value <= 'f')
                {
                    return value - 'a' + 10;
                }

                if (value >= 'A' && value <= 'F')
                {
                    return value - 'A' + 10;
                }

                return -1;
            };

        for (const char value : text)
        {
            if (value == '-')
            {
                continue;
            }

            const int digit = hexValue(value);
            if (digit < 0 || nibbleIndex >= 32)
            {
                return std::nullopt;
            }

            const std::size_t byteIndex =
                nibbleIndex / 2;

            if ((nibbleIndex & 1U) == 0)
            {
                bytes[byteIndex] =
                    static_cast<u8>(digit << 4);
            }
            else
            {
                bytes[byteIndex] |=
                    static_cast<u8>(digit);
            }

            ++nibbleIndex;
        }

        if (nibbleIndex != 32)
        {
            return std::nullopt;
        }

        StrongId id{};

        for (std::size_t index = 0; index < 8; ++index)
        {
            id.high =
                (id.high << 8) |
                bytes[index];
            id.low =
                (id.low << 8) |
                bytes[index + 8];
        }

        return id;
    }

    [[nodiscard]] static StrongId Random()
    {
        thread_local std::mt19937_64 generator{
            []
            {
                std::random_device source;
                std::seed_seq seed{
                    source(), source(),
                    source(), source(),
                    source(), source(),
                    source(), source()
                };
                return std::mt19937_64(seed);
            }()
        };

        StrongId id{
            .high = generator(),
            .low = generator()
        };

        // RFC 4122-compatible version/variant bit placement makes the
        // textual representation recognizable to external tools while
        // Orbit continues to treat the value as an opaque strong ID.
        id.high &=
            0xFFFFFFFFFFFF0FFFULL;
        id.high |=
            0x0000000000004000ULL;
        id.low &=
            0x3FFFFFFFFFFFFFFFULL;
        id.low |=
            0x8000000000000000ULL;

        return id;
    }
};
} // namespace orbit::core

namespace std
{
template <typename Tag>
struct hash<orbit::core::StrongId<Tag>>
{
    [[nodiscard]] size_t operator()(
        const orbit::core::StrongId<Tag>& id) const noexcept
    {
        const size_t highHash =
            std::hash<orbit::u64>{}(id.high);
        const size_t lowHash =
            std::hash<orbit::u64>{}(id.low);

        return highHash ^
            (lowHash +
             static_cast<size_t>(
                 0x9e3779b97f4a7c15ULL) +
             (highHash << 6) +
             (highHash >> 2));
    }
};
} // namespace std
