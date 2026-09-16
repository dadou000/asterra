#include <orbit/fields/FieldRegistry.hpp>

#include <stdexcept>
#include <utility>

namespace orbit::fields
{
namespace
{
[[nodiscard]] bool LocationMatchesDomain(
    const FieldDomain domain,
    const FieldLocation& location) noexcept
{
    switch (domain)
    {
        case FieldDomain::Surface:
            return std::holds_alternative<
                SurfaceFieldLocation>(
                    location);

        case FieldDomain::Volume:
        case FieldDomain::AtmosphericShell:
        case FieldDomain::LocalRegion:
        case FieldDomain::BodyInterior:
            return std::holds_alternative<
                VolumeFieldLocation>(
                    location);

        case FieldDomain::FreeSpace:
            return std::holds_alternative<
                FreeSpaceFieldLocation>(
                    location);
    }

    return false;
}

void ValidateRegistration(
    const FieldRegistration& registration)
{
    if (!registration.descriptor.id)
    {
        throw std::invalid_argument(
            "Field ID must be valid.");
    }

    if (registration.descriptor.name.empty())
    {
        throw std::invalid_argument(
            "Field name must not be empty.");
    }

    if (registration.descriptor.resolution.mode ==
            FieldResolutionMode::FixedSpacing &&
        registration.descriptor.resolution.
                nominalSpacingMeters <=
            0.0)
    {
        throw std::invalid_argument(
            "Fixed-spacing field requires positive nominal spacing.");
    }

    if (registration.descriptor.residency !=
            FieldResidency::Gpu &&
        !registration.cpuEvaluator)
    {
        throw std::invalid_argument(
            "CPU or hybrid field requires a CPU evaluator.");
    }
}
} // namespace

FieldId FieldRegistry::Register(
    FieldRegistration registration)
{
    FieldId id = FieldId::Random();

    while (fields_.contains(id))
    {
        id = FieldId::Random();
    }

    registration.descriptor.id = id;

    Add(std::move(registration));
    return id;
}

void FieldRegistry::Add(
    FieldRegistration registration)
{
    ValidateRegistration(registration);

    const FieldId id =
        registration.descriptor.id;

    if (fields_.contains(id))
    {
        throw std::invalid_argument(
            "Field ID already exists.");
    }

    fields_.emplace(
        id,
        FieldRecord{
            .descriptor =
                std::move(
                    registration.descriptor),
            .cpuEvaluator =
                std::move(
                    registration.cpuEvaluator),
            .revision =
                std::move(
                    registration.revision)
        });
}

const FieldDescriptor*
FieldRegistry::Find(
    const FieldId id) const noexcept
{
    const auto found = fields_.find(id);

    return found == fields_.end()
        ? nullptr
        : &found->second.descriptor;
}

std::vector<FieldId>
FieldRegistry::FieldsForBody(
    const universe::BodyId body) const
{
    std::vector<FieldId> result;

    for (const auto& [id, record] :
         fields_)
    {
        if (record.descriptor.
                ownerBody.has_value() &&
            *record.descriptor.ownerBody ==
                body)
        {
            result.push_back(id);
        }
    }

    return result;
}

u64 FieldRegistry::Revision(
    const FieldId id) const noexcept
{
    const auto found = fields_.find(id);

    if (found == fields_.end() ||
        !found->second.revision)
    {
        return 0;
    }

    return found->second.revision();
}

std::optional<FieldValue>
FieldRegistry::TrySampleCpu(
    const FieldId id,
    const FieldLocation& location) const
{
    const auto found = fields_.find(id);

    if (found == fields_.end())
    {
        return std::nullopt;
    }

    const FieldRecord& record =
        found->second;

    if (record.descriptor.residency ==
            FieldResidency::Gpu ||
        !record.cpuEvaluator ||
        !LocationMatchesDomain(
            record.descriptor.domain,
            location))
    {
        return std::nullopt;
    }

    if (record.descriptor.
            ownerBody.has_value())
    {
        const auto* surface =
            std::get_if<
                SurfaceFieldLocation>(
                    &location);

        const auto* volume =
            std::get_if<
                VolumeFieldLocation>(
                    &location);

        const universe::BodyId locationBody =
            surface != nullptr
                ? surface->body
                : (volume != nullptr
                    ? volume->body
                    : universe::BodyId{});

        if (!locationBody ||
            locationBody !=
                *record.descriptor.ownerBody)
        {
            return std::nullopt;
        }
    }

    return record.cpuEvaluator(location);
}
} // namespace orbit::fields
