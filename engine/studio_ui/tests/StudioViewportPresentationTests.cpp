#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <cstdlib>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Studio viewport presentation test failed.\n";
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main()
{
    using orbit::studio_session::ViewportMode;
    using orbit::studio_ui::SelectStudioViewportPresentation;
    using orbit::studio_ui::StudioViewportPresentation;

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::Perspective,
            true,
            false,
            false) ==
        StudioViewportPresentation::BodyPreview);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::BodyMap,
            false,
            false,
            false) ==
        StudioViewportPresentation::Blank);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::Debug,
            true,
            true,
            true) ==
        StudioViewportPresentation::TerrainDebug);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::Debug,
            true,
            false,
            false) ==
        StudioViewportPresentation::TerrainDebugUnavailable);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::Debug,
            true,
            true,
            false) ==
        StudioViewportPresentation::TerrainDebugUnavailable);

    Check(
        SelectStudioViewportPresentation(
            ViewportMode::Debug,
            false,
            true,
            true) ==
        StudioViewportPresentation::TerrainDebug);

    return EXIT_SUCCESS;
}
