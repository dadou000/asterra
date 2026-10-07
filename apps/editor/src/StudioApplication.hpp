#pragma once

namespace orbit::editor_app
{
// Unified Studio lifecycle. HotReloadBootstrap owns the immutable process entry.
class StudioApplication
{
public:
    int Run(int argc, char** argv);
};
} // namespace orbit::editor_app
