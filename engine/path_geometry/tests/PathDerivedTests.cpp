#include <orbit/path_geometry/PathDerived.hpp>

#include <cmath>
#include <iostream>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr \
                << "PathGeometry test failed: " \
                << #expression \
                << " at line " \
                << __LINE__ \
                << '\n'; \
            return 1; \
        } \
    } while (false)

namespace
{
[[nodiscard]] bool Near(
    const orbit::f64 a,
    const orbit::f64 b,
    const orbit::f64 epsilon =
        1.0e-9)
{
    return std::abs(a - b) <=
        epsilon;
}
}

int main()
{
    const orbit::scene::ObjectId edge =
        orbit::scene::ObjectId::Random();
    const orbit::frames::FrameId frame =
        orbit::frames::FrameId::Random();

    orbit::path_geometry::PathCenterline
        source{
            .edge = edge,
            .frame = frame,
            .sourceRevision = 17,
            .samples = {
                {
                    .position = {
                        0.0,
                        0.0,
                        0.0
                    },
                    .up = {
                        0.0,
                        1.0,
                        0.0
                    }
                },
                {
                    .position = {
                        20.0,
                        0.0,
                        0.0
                    },
                    .up = {
                        0.0,
                        1.0,
                        0.0
                    }
                }
            }
        };

    orbit::paths::PathProfile profile{
        .name = "Four Lane Road",
        .kind =
            orbit::paths::
                PathProfileKind::Road,
        .widthMeters = 12.0,
        .lanes = 4
    };

    const orbit::path_geometry::
        PathBuildOptions options{
            .sampleSpacingMeters = 5.0
        };

    const auto product =
        orbit::path_geometry::
            BuildPathDerived(
                source,
                profile,
                options);

    ORBIT_TEST_CHECK(
        product.edge == edge);
    ORBIT_TEST_CHECK(
        product.frame == frame);
    ORBIT_TEST_CHECK(
        product.sourceRevision == 17);
    ORBIT_TEST_CHECK(
        product.widthMeters == 12.0);
    ORBIT_TEST_CHECK(
        product.laneCount == 4);
    ORBIT_TEST_CHECK(
        product.stations.size() == 5);
    ORBIT_TEST_CHECK(
        product.visualMesh.
            vertices.size() == 10);
    ORBIT_TEST_CHECK(
        product.visualMesh.
            indices.size() == 24);
    ORBIT_TEST_CHECK(
        product.collision.size() == 8);
    ORBIT_TEST_CHECK(
        product.navigation.size() ==
        product.stations.size());
    ORBIT_TEST_CHECK(
        product.lanes.size() == 4);

    for (std::size_t triangle = 0;
         triangle <
             product.collision.size();
         ++triangle)
    {
        const std::size_t base =
            triangle * 3U;

        const auto& collision =
            product.collision[triangle];

        const auto indexA =
            product.visualMesh.
                indices[base];
        const auto indexB =
            product.visualMesh.
                indices[base + 1U];
        const auto indexC =
            product.visualMesh.
                indices[base + 2U];

        ORBIT_TEST_CHECK(
            collision.a ==
            product.visualMesh.
                vertices[indexA].
                position);
        ORBIT_TEST_CHECK(
            collision.b ==
            product.visualMesh.
                vertices[indexB].
                position);
        ORBIT_TEST_CHECK(
            collision.c ==
            product.visualMesh.
                vertices[indexC].
                position);
    }

    const orbit::f64
        expectedOffsets[4]{
            -4.5,
            -1.5,
            1.5,
            4.5
        };

    for (orbit::u32 lane = 0;
         lane < 4;
         ++lane)
    {
        const auto& reference =
            product.lanes[lane];

        ORBIT_TEST_CHECK(
            reference.laneIndex == lane);
        ORBIT_TEST_CHECK(
            Near(
                reference.
                    lateralOffsetMeters,
                expectedOffsets[lane]));
        ORBIT_TEST_CHECK(
            reference.points.size() ==
            product.stations.size());

        for (std::size_t index = 0;
             index <
                 reference.points.size();
             ++index)
        {
            const orbit::f64 z =
                reference.points[index].
                    position.z;

            ORBIT_TEST_CHECK(
                Near(
                    z,
                    expectedOffsets[lane]));
            ORBIT_TEST_CHECK(
                Near(
                    reference.
                        points[index].
                        stationMeters,
                    product.
                        stations[index].
                        stationMeters));
        }
    }

    ORBIT_TEST_CHECK(
        product.referenceGraph.
            nodes.size() ==
        product.stations.size() * 4U);

    ORBIT_TEST_CHECK(
        product.referenceGraph.
            edges.size() ==
        (product.stations.size() - 1U) *
            4U);

    for (const auto& edgeRef :
         product.referenceGraph.edges)
    {
        ORBIT_TEST_CHECK(
            edgeRef.bidirectional);
        ORBIT_TEST_CHECK(
            edgeRef.lengthMeters > 0.0);
    }

    for (const auto& nav :
         product.navigation)
    {
        ORBIT_TEST_CHECK(
            Near(
                nav.halfWidthMeters,
                6.0));
        ORBIT_TEST_CHECK(
            nav.lanes == 4);
    }

    const auto regenerated =
        orbit::path_geometry::
            BuildPathDerived(
                source,
                profile,
                options);

    // This is the cache-deletion acceptance invariant: no cache state is
    // consumed by the builder, so rebuilding from authoritative inputs is
    // exactly equivalent.
    ORBIT_TEST_CHECK(
        regenerated == product);
    ORBIT_TEST_CHECK(
        regenerated.buildSignature ==
        product.buildSignature);

    auto changedProfile = profile;
    changedProfile.widthMeters = 14.0;

    const auto changed =
        orbit::path_geometry::
            BuildPathDerived(
                source,
                changedProfile,
                options);

    ORBIT_TEST_CHECK(
        changed.buildSignature !=
        product.buildSignature);
    ORBIT_TEST_CHECK(
        changed.visualMesh !=
        product.visualMesh);

    orbit::path_geometry::PathCenterline
        curved = source;

    curved.samples = {
        {
            .position = {
                0.0,
                0.0,
                0.0
            },
            .up = {
                0.0,
                1.0,
                0.0
            }
        },
        {
            .position = {
                10.0,
                0.0,
                5.0
            },
            .up = {
                0.0,
                1.0,
                0.0
            }
        },
        {
            .position = {
                20.0,
                0.0,
                15.0
            },
            .up = {
                0.0,
                1.0,
                0.0
            }
        }
    };

    const auto curvedProduct =
        orbit::path_geometry::
            BuildPathDerived(
                curved,
                profile,
                {
                    .sampleSpacingMeters =
                        2.5
                });

    ORBIT_TEST_CHECK(
        curvedProduct.stations.size() >
        curved.samples.size());

    for (const auto& station :
         curvedProduct.stations)
    {
        ORBIT_TEST_CHECK(
            orbit::math::Length(
                station.tangent) >
            0.99);
        ORBIT_TEST_CHECK(
            std::abs(
                orbit::math::Dot(
                    station.tangent,
                    station.lateral)) <
            1.0e-8);
    }

    return 0;
}
