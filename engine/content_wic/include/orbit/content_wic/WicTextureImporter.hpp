#pragma once

namespace orbit::content
{
class ImporterRegistry;
}

namespace orbit::content_wic
{
// Registers Windows Imaging Component source decoders that produce Orbit's
// platform-neutral runtime texture container.
void RegisterTextureImporters(
    content::ImporterRegistry& registry);
} // namespace orbit::content_wic
