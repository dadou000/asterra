#include <orbit/volume_fields/VolumeFieldStorage.hpp>

#include <orbit/world_model/VolumeSchemas.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "Volume-field test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::volume_fields;

    const u64 scalarOnly =
        static_cast<u64>(
            world_model::
                VolumeField::Density);

    const u64 scalarVector =
        scalarOnly |
        static_cast<u64>(
            world_model::
                VolumeField::Velocity);

    const auto scalarFields =
        FieldsFromMask(
            scalarOnly);
    const auto mixedFields =
        FieldsFromMask(
            scalarVector);

    Check(scalarFields.size() == 1U);
    Check(mixedFields.size() == 2U);
    Check(FieldKind(
              world_model::
                  VolumeField::Density) ==
          FieldValueKind::Scalar);
    Check(FieldKind(
              world_model::
                  VolumeField::Velocity) ==
          FieldValueKind::Vector3);
    Check(FieldBytesPerCell(
              world_model::
                  VolumeField::Density) ==
          4U);
    Check(FieldBytesPerCell(
              world_model::
                  VolumeField::Velocity) ==
          16U);

    // The movement/reuse algorithm is exercised by its deterministic
    // coordinate contract in the GPU-backed class. Keep the arithmetic
    // invariant documented here: one 8^3 scalar tile is 2048 bytes and
    // one vector tile is 8192 bytes.
    Check(
        8U * 8U * 8U *
            FieldBytesPerCell(
                world_model::
                    VolumeField::Density) ==
        2048U);

    Check(
        8U * 8U * 8U *
            FieldBytesPerCell(
                world_model::
                    VolumeField::Velocity) ==
        8192U);

    return 0;
}
