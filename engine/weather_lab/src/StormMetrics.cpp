#include <orbit/weather_lab/StormMetrics.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace orbit::weather_lab
{
namespace
{
bool Load(WxReader& reader, const std::size_t frame, const char* name,
    std::vector<float>& out, const bool required, std::string* error)
{
    if (reader.Header().FieldIndex(name) < 0)
    {
        out.clear();
        if (required && error != nullptr)
        {
            *error = std::string("missing required field '") + name + "'";
        }
        return !required;
    }
    return reader.ReadField(frame, name, out, error);
}
} // namespace

double LowestLevelThetaMean(
    const WxHeader& header, const std::vector<float>& th)
{
    const std::size_t plane = static_cast<std::size_t>(header.nx) * header.ny;
    double sum = 0.0;
    for (std::size_t c = 0; c < plane; ++c)
    {
        sum += th[c];
    }
    return sum / static_cast<double>(plane);
}

StormMetrics ComputeFrameMetrics(
    const WxHeader& h,
    const float time,
    const StormFrameFields& fields,
    const double baseThetaMean)
{
    const std::size_t plane = static_cast<std::size_t>(h.nx) * h.ny;
    const std::vector<float>& th = *fields.th;
    const std::vector<float>& w = *fields.w;
    const std::vector<float>& qr = *fields.qr;
    std::vector<float> derivedVorticity;
    const std::vector<float>* zvortPtr = fields.zvort;
    if (zvortPtr == nullptr)
    {
        derivedVorticity.assign(h.CellCount(), 0.0F);
        if (fields.u != nullptr && fields.v != nullptr)
        {
            const std::vector<float>& u = *fields.u;
            const std::vector<float>& v = *fields.v;
            for (std::uint32_t k = 0; k < h.nz; ++k)
            {
                for (std::uint32_t j = 1; j + 1U < h.ny; ++j)
                {
                    for (std::uint32_t i = 1; i + 1U < h.nx; ++i)
                    {
                        const std::size_t c = (static_cast<std::size_t>(k) * h.ny + j) * h.nx + i;
                        derivedVorticity[c] = (v[c + 1U] - v[c - 1U]) / (2.0F * h.dx)
                            - (u[c + h.nx] - u[c - h.nx]) / (2.0F * h.dy);
                    }
                }
            }
        }
        zvortPtr = &derivedVorticity;
    }
    const std::vector<float>& zvort = *zvortPtr;

    StormMetrics m;
    m.time = time;
    m.maxUpdraft = -1.0e9F;
    m.maxDowndraft = 1.0e9F;
    for (std::uint32_t k = 0; k < h.nz; ++k)
    {
        const float z = h.centreHeight[k];
        for (std::size_t c = 0; c < plane; ++c)
        {
            const std::size_t idx = static_cast<std::size_t>(k) * plane + c;
            if (w[idx] > m.maxUpdraft)
            {
                m.maxUpdraft = w[idx];
                m.maxUpdraftHeight = z;
            }
            m.maxDowndraft = std::min(m.maxDowndraft, w[idx]);
            m.maxRainMixing = std::max(m.maxRainMixing, qr[idx] * 1000.0F);
            float condensate = qr[idx];
            for (const auto* species : {fields.qc, fields.qi, fields.qs, fields.qg})
            {
                if (species != nullptr && !species->empty())
                {
                    condensate += (*species)[idx];
                }
            }
            if (condensate > 1.0e-4F)
            {
                m.cloudTop = std::max(m.cloudTop, z);
            }
            if (z <= 1000.0F)
            {
                m.maxLowVorticity = std::max(m.maxLowVorticity, zvort[idx]);
            }
        }
    }
    const float layer = h.nz > 1U ? h.centreHeight[1] - h.centreHeight[0] : 0.0F;
    for (std::size_t c = 0; c < plane; ++c)
    {
        float uh = 0.0F;
        for (std::uint32_t k = 0; k < h.nz; ++k)
        {
            const float z = h.centreHeight[k];
            if (z >= 2000.0F && z <= 5000.0F)
            {
                const std::size_t idx = static_cast<std::size_t>(k) * plane + c;
                uh += w[idx] * zvort[idx] * layer;
            }
        }
        m.maxUpdraftHelicity = std::max(m.maxUpdraftHelicity, uh);
        m.coldPoolDeficit = std::max(
            m.coldPoolDeficit, static_cast<float>(baseThetaMean) - th[c]);
        if (qr[c] > 1.0e-5F)
        {
            m.rainArea += h.dx * h.dy * 1.0e-6F;
        }
    }
    return m;
}

bool ComputeStormMetrics(
    WxReader& reader, std::vector<StormMetrics>& out, std::string* error)
{
    const WxHeader& h = reader.Header();
    out.clear();
    std::vector<float> th, w, qr, u, v, zvort, qc, qi, qs, qg;
    double baseMean = 0.0;
    for (std::size_t f = 0; f < reader.FrameCount(); ++f)
    {
        if (!Load(reader, f, "th", th, true, error)
            || !Load(reader, f, "w", w, true, error)
            || !Load(reader, f, "qr", qr, true, error)
            || !Load(reader, f, "qc", qc, false, error)
            || !Load(reader, f, "qi", qi, false, error)
            || !Load(reader, f, "qs", qs, false, error)
            || !Load(reader, f, "qg", qg, false, error)
            || !Load(reader, f, "zvort", zvort, false, error))
        {
            return false;
        }
        if (zvort.empty()
            && (!Load(reader, f, "u", u, true, error)
                || !Load(reader, f, "v", v, true, error)))
        {
            return false;
        }
        if (f == 0U)
        {
            baseMean = LowestLevelThetaMean(h, th);
        }
        StormFrameFields fields;
        fields.th = &th;
        fields.w = &w;
        fields.qr = &qr;
        fields.qc = qc.empty() ? nullptr : &qc;
        fields.qi = qi.empty() ? nullptr : &qi;
        fields.qs = qs.empty() ? nullptr : &qs;
        fields.qg = qg.empty() ? nullptr : &qg;
        fields.zvort = zvort.empty() ? nullptr : &zvort;
        fields.u = u.empty() ? nullptr : &u;
        fields.v = v.empty() ? nullptr : &v;
        out.push_back(ComputeFrameMetrics(h, h.times[f], fields, baseMean));
    }
    return true;
}

std::string FormatMetricsTable(const std::vector<StormMetrics>& metrics)
{
    std::string text =
        "  t[min]  wmax[m/s]  z(wmax)[km]  wmin[m/s]  qr[g/kg]  top[km]"
        "  zeta<1km[1/s]  UH2-5[m2/s2]  coldpool[K]  rain[km2]\n";
    char line[256];
    for (const StormMetrics& m : metrics)
    {
        std::snprintf(line, sizeof line,
            "%8.1f %10.1f %12.2f %10.1f %9.2f %8.1f %14.4f %13.0f %12.2f %10.0f\n",
            static_cast<double>(m.time) / 60.0,
            static_cast<double>(m.maxUpdraft),
            static_cast<double>(m.maxUpdraftHeight) / 1000.0,
            static_cast<double>(m.maxDowndraft),
            static_cast<double>(m.maxRainMixing),
            static_cast<double>(m.cloudTop) / 1000.0,
            static_cast<double>(m.maxLowVorticity),
            static_cast<double>(m.maxUpdraftHelicity),
            static_cast<double>(m.coldPoolDeficit),
            static_cast<double>(m.rainArea));
        text += line;
    }
    return text;
}
} // namespace orbit::weather_lab
