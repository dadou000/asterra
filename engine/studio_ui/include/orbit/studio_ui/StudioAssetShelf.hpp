#pragma once

namespace orbit::content
{
class ContentService;
}

namespace orbit::studio_ui
{
// Binds the project content catalog to Orbit's canonical Properties surface.
// The shelf is intentionally registered as an inspector provider so it follows
// the unified Explorer -> Viewport -> Properties shell instead of introducing
// another standalone launcher/panel.
void InstallStudioAssetShelf(content::ContentService* content) noexcept;
} // namespace orbit::studio_ui
