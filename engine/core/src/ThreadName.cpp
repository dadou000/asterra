#include <orbit/core/ThreadName.hpp>

#include <orbit/profiler/Profiler.hpp>

#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace orbit::core
{
void SetCurrentThreadName(const std::string_view name) noexcept
{
    profiler::NoteThreadName(name);

#if defined(_WIN32)
    try
    {
        // SetThreadDescription takes UTF-16. Thread names are ASCII in
        // practice, so a widening copy is enough.
        std::wstring wide;
        wide.reserve(name.size());
        for (const char character : name)
        {
            wide.push_back(static_cast<wchar_t>(
                static_cast<unsigned char>(character)));
        }
        SetThreadDescription(GetCurrentThread(), wide.c_str());
    }
    catch (...)
    {
        // Naming is diagnostic only.
    }
#else
    static_cast<void>(name);
#endif
}
} // namespace orbit::core
