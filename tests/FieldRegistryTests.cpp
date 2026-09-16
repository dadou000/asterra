#include <orbit/fields/FieldRegistry.hpp>

#include <cassert>

int main()
{
    orbit::fields::FieldRegistry fields;

    struct TestBodyTag;
    const orbit::universe::BodyId body{
        .high = 1,
        .low = 2
    };

    orbit::u64 revision = 7;

    const auto elevation =
        fields.Register({
            .descriptor = {
                .ownerBody = body,
                .name = "Elevation",
                .valueKind =
                    orbit::fields::
                        FieldValueKind::Scalar,
                .domain =
                    orbit::fields::
                        FieldDomain::Surface,
                .unit = "m",
                .residency =
                    orbit::fields::
                        FieldResidency::Cpu
            },
            .cpuEvaluator =
                [](const orbit::fields::
                       FieldLocation& location)
                    -> std::optional<
                        orbit::fields::FieldValue>
                {
                    const auto* surface =
                        std::get_if<
                            orbit::fields::
                                SurfaceFieldLocation>(
                                    &location);

                    if (surface == nullptr)
                    {
                        return std::nullopt;
                    }

                    return orbit::fields::
                        FieldValue(
                            surface->
                                unitDirection.y *
                            100.0);
                },
            .revision =
                [&revision]
                {
                    return revision;
                }
        });

    const auto sample =
        fields.TrySampleCpu(
            elevation,
            orbit::fields::SurfaceFieldLocation{
                .body = body,
                .unitDirection = {
                    0.0,
                    0.5,
                    0.0
                },
                .footprintMeters = 10.0
            });

    assert(sample.has_value());
    assert(
        std::get<orbit::f64>(*sample) ==
        50.0);
    assert(fields.Revision(elevation) == 7);

    ++revision;
    assert(fields.Revision(elevation) == 8);

    const auto gpuOnly =
        fields.Register({
            .descriptor = {
                .ownerBody = body,
                .name = "Velocity",
                .valueKind =
                    orbit::fields::
                        FieldValueKind::Vector,
                .domain =
                    orbit::fields::
                        FieldDomain::Volume,
                .unit = "m/s",
                .residency =
                    orbit::fields::
                        FieldResidency::Gpu,
                .resolution = {
                    .mode =
                        orbit::fields::
                            FieldResolutionMode::
                                AdaptiveLod
                }
            }
        });

    assert(
        !fields.TrySampleCpu(
            gpuOnly,
            orbit::fields::VolumeFieldLocation{
                .body = body
            }).has_value());

    assert(
        fields.FieldsForBody(body).size() ==
        2);

    return 0;
}
