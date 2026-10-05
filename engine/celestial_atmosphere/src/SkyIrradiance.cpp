#include <orbit/celestial_atmosphere/SkyIrradiance.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbit::celestial_atmosphere
{
namespace
{
constexpr f64 kPi = std::numbers::pi_v<f64>;

// Real spherical harmonics, bands 0..2, in the usual (Ramamoorthi-Hanrahan)
// ordering: 00, 1-1, 10, 11, 2-2, 2-1, 20, 21, 22.
[[nodiscard]] std::array<f64, 9> ShBasis(const math::Double3& d) noexcept
{
    return {
        0.282095,
        0.488603 * d.y,
        0.488603 * d.z,
        0.488603 * d.x,
        1.092548 * d.x * d.y,
        1.092548 * d.y * d.z,
        0.315392 * (3.0 * d.z * d.z - 1.0),
        1.092548 * d.x * d.z,
        0.546274 * (d.x * d.x - d.y * d.y)};
}
} // namespace

math::Double3 SkyFrameSunDirection(
    const math::Double3& observerUpBody,
    const math::Double3& sunDirectionBody,
    const f64 cosineQuantum)
{
    const auto up = math::Normalize(observerUpBody);
    const auto sun = math::Normalize(sunDirectionBody);

    f64 cosZenith = std::clamp(math::Dot(up, sun), -1.0, 1.0);
    if (cosineQuantum > 0.0)
        cosZenith = std::round(cosZenith / cosineQuantum) * cosineQuantum;
    cosZenith = std::clamp(cosZenith, -1.0, 1.0);

    const f64 sinZenith = std::sqrt(std::max(0.0, 1.0 - cosZenith * cosZenith));
    return {sinZenith, 0.0, cosZenith};
}

math::Double3 SkyFrameBasis::ToBody(
    const math::Double3& v) const noexcept
{
    return x * v.x + y * v.y + z * v.z;
}

math::Double3 SkyFrameBasis::FromBody(
    const math::Double3& v) const noexcept
{
    return {math::Dot(v, x), math::Dot(v, y), math::Dot(v, z)};
}

SkyFrameBasis MakeSkyFrameBasis(
    const math::Double3& observerUpBody,
    const math::Double3& sunDirectionBody)
{
    SkyFrameBasis basis;
    basis.z = math::Normalize(observerUpBody);

    const auto sun = math::Normalize(sunDirectionBody);
    auto horizontal = sun - basis.z * math::Dot(sun, basis.z);

    if (math::LengthSquared(horizontal) <= 1.0e-12)
    {
        // Sun at the zenith or nadir: any horizontal axis is equivalent.
        const math::Double3 reference =
            std::abs(basis.z.y) < 0.9
                ? math::Double3{0.0, 1.0, 0.0}
                : math::Double3{1.0, 0.0, 0.0};
        horizontal = reference - basis.z * math::Dot(reference, basis.z);
    }

    basis.x = math::Normalize(horizontal);
    basis.y = math::Cross(basis.z, basis.x);
    return basis;
}

SkySphericalHarmonics ProjectSkyViewToSphericalHarmonics(
    const AtmosphereLut2D& skyView)
{
    SkySphericalHarmonics result;

    if (skyView.width == 0U ||
        skyView.height == 0U ||
        skyView.texels.size() <
            static_cast<std::size_t>(skyView.width) * skyView.height)
    {
        return result;
    }

    // Texel (x, y) covers theta in [pi*y/H, pi*(y+1)/H] from the zenith and
    // phi in [2*pi*x/W, 2*pi*(x+1)/W]; its solid angle is the exact band area
    // so the rows near the poles are not over-weighted.
    const f64 deltaPhi = 2.0 * kPi / static_cast<f64>(skyView.width);
    std::array<math::Double3, 9> sum{};

    for (u32 y = 0U; y < skyView.height; ++y)
    {
        const f64 theta0 =
            kPi * static_cast<f64>(y) / static_cast<f64>(skyView.height);
        const f64 theta1 =
            kPi * static_cast<f64>(y + 1U) / static_cast<f64>(skyView.height);
        const f64 thetaMid = 0.5 * (theta0 + theta1);
        const f64 solidAngle =
            deltaPhi * (std::cos(theta0) - std::cos(theta1));

        const f64 sinTheta = std::sin(thetaMid);
        const f64 cosTheta = std::cos(thetaMid);

        for (u32 x = 0U; x < skyView.width; ++x)
        {
            const f64 phi = deltaPhi * (static_cast<f64>(x) + 0.5);
            const math::Double3 direction{
                sinTheta * std::cos(phi),
                sinTheta * std::sin(phi),
                cosTheta};

            const auto& texel = skyView.At(x, y);
            const math::Double3 radiance{
                std::max(static_cast<f64>(texel.x), 0.0),
                std::max(static_cast<f64>(texel.y), 0.0),
                std::max(static_cast<f64>(texel.z), 0.0)};

            const auto basis = ShBasis(direction);
            for (std::size_t i = 0U; i < 9U; ++i)
                sum[i] = sum[i] + radiance * (basis[i] * solidAngle);
        }
    }

    for (std::size_t i = 0U; i < 9U; ++i)
    {
        result.coefficients[i] = {
            static_cast<f32>(sum[i].x),
            static_cast<f32>(sum[i].y),
            static_cast<f32>(sum[i].z)};
    }
    return result;
}

math::Float3 EvaluateSkyIrradiance(
    const SkySphericalHarmonics& sh,
    const math::Double3& normalSkyFrame) noexcept
{
    const auto n = math::Normalize(normalSkyFrame);
    const auto basis = ShBasis(n);

    // Cosine-lobe convolution factors per band: pi, 2*pi/3, pi/4.
    constexpr std::array<f64, 9> kBand{
        kPi,
        2.0 * kPi / 3.0, 2.0 * kPi / 3.0, 2.0 * kPi / 3.0,
        kPi / 4.0, kPi / 4.0, kPi / 4.0, kPi / 4.0, kPi / 4.0};

    f64 r = 0.0;
    f64 g = 0.0;
    f64 b = 0.0;
    for (std::size_t i = 0U; i < 9U; ++i)
    {
        const f64 w = kBand[i] * basis[i];
        r += w * static_cast<f64>(sh.coefficients[i].x);
        g += w * static_cast<f64>(sh.coefficients[i].y);
        b += w * static_cast<f64>(sh.coefficients[i].z);
    }

    return {
        static_cast<f32>(std::max(r, 0.0)),
        static_cast<f32>(std::max(g, 0.0)),
        static_cast<f32>(std::max(b, 0.0))};
}
} // namespace orbit::celestial_atmosphere
