#include "StudioApplication.hpp"

// HotReloadBootstrap calls this entry after process and reload-host setup.
int OrbitStudioMain(int argc, char** argv)
{
    orbit::editor_app::StudioApplication application;
    return application.Run(argc, argv);
}
