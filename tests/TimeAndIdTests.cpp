// These tests are written with assert(); keep it live in Release builds, where
// NDEBUG would otherwise compile every check away.
#undef NDEBUG

#include <orbit/core/StrongId.hpp>

#include <cassert>
#include <string>

namespace
{
struct TestIdTag;
using TestId = orbit::core::StrongId<TestIdTag>;
}

int main()
{
    const TestId known{
        .high = 0x0123456789abcdefULL,
        .low = 0xfedcba9876543210ULL
    };

    const std::string text =
        known.ToString();

    assert(
        text ==
        "01234567-89ab-cdef-fedc-ba9876543210");

    const auto parsed =
        TestId::Parse(text);

    assert(parsed.has_value());
    assert(*parsed == known);

    const auto compact =
        TestId::Parse(
            "0123456789abcdeffedcba9876543210");

    assert(compact.has_value());
    assert(*compact == known);

    assert(
        !TestId::Parse("not-an-id").has_value());

    const TestId random =
        TestId::Random();

    assert(random.IsValid());
    assert(
        TestId::Parse(random.ToString()) ==
        random);

    return 0;
}
