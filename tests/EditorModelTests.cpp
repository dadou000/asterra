#include <orbit/core/Log.hpp>
#include <orbit/editor_model/OutputLog.hpp>

#include <string_view>

int main()
{
    orbit::editor_model::OutputLog output(8);

    orbit::log::Info(
        "studio-output-sentinel");

    const auto entries =
        output.Snapshot();

    bool found = false;

    for (const auto& entry : entries)
    {
        if (entry.message ==
            "studio-output-sentinel")
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        return 1;
    }

    output.Clear();

    if (!output.Snapshot().empty())
    {
        return 2;
    }

    return 0;
}
