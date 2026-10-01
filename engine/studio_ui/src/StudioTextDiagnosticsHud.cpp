#include <orbit/studio_ui/StudioTextDiagnosticsHud.hpp>

#include <orbit/studio_ui/StudioRenderViewSet.hpp>

#include <exception>
#include <optional>
#include <utility>

namespace orbit::studio_ui
{
void StudioTextDiagnosticsHud::Draw(
    editor_ui::PanelContext& context,
    StudioRenderViewSet& views,
    const std::string_view id,
    const editor_ui::ImageInteraction& interaction)
{
    const auto* view = views.Find(id);
    if (view == nullptr || !views.TextDiagnosticsHud(id))
    {
        return;
    }

    const auto& camera = view->Camera();
    const bool hovered = interaction.hovered;
    const std::array<f64, 11> key{
        camera.localPositionMeters.x,
        camera.localPositionMeters.y,
        camera.localPositionMeters.z,
        static_cast<f64>(camera.forward.x),
        static_cast<f64>(camera.forward.y),
        static_cast<f64>(camera.forward.z),
        hovered ? static_cast<f64>(interaction.u) : -1.0,
        hovered ? static_cast<f64>(interaction.v) : -1.0,
        static_cast<f64>(view->Width()),
        static_cast<f64>(view->Height()),
        static_cast<f64>(camera.verticalFovRadians)};

    auto& cache = caches_[std::string(id)];
    const auto now = std::chrono::steady_clock::now();
    if (!cache.valid ||
        cache.key != key ||
        now - cache.builtAt > std::chrono::milliseconds(500))
    {
        try
        {
            cache.text = FormatStudioViewportTextReport(
                views.TextDiagnostics(
                    id,
                    hovered
                        ? std::optional(std::pair<f32, f32>{
                              interaction.u,
                              interaction.v})
                        : std::nullopt));
        }
        catch (const std::exception& exception)
        {
            cache.text =
                std::string("Diagnostics unavailable: ") + exception.what();
        }
        cache.key = key;
        cache.builtAt = now;
        cache.valid = true;
    }

    context.OverlayTextOnLastItem(cache.text);
}
} // namespace orbit::studio_ui
