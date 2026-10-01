#include <orbit/profiler/Profiler.hpp>

#include <orbit/core/ThreadName.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

namespace orbit::profiler
{
namespace
{
struct Event
{
    const char* name{nullptr};
    u64 begin{0U};
    u64 end{0U};
    u16 beginCore{0U};
    u16 endCore{0U};
    u16 depth{0U};
    u16 reserved{0U};
};

static_assert(sizeof(Event) == 32U);

constexpr u32 kCapacity = 1U << 14U;
constexpr u32 kMaxDepth = 48U;
constexpr u32 kMaxStackFrames = 40U;

[[nodiscard]] u64 QpcFrequency() noexcept
{
#if defined(_WIN32)
    static const u64 frequency = []
    {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<u64>(value.QuadPart);
    }();
    return frequency;
#else
    return 1'000'000'000ULL;
#endif
}

[[nodiscard]] u16 CurrentCore() noexcept
{
#if defined(_WIN32)
    return static_cast<u16>(GetCurrentProcessorNumber());
#else
    return 0U;
#endif
}

[[nodiscard]] u32 CurrentThreadId() noexcept
{
#if defined(_WIN32)
    return static_cast<u32>(GetCurrentThreadId());
#else
    return 0U;
#endif
}

struct ThreadBuffer
{
    struct Open
    {
        const char* name{nullptr};
        u64 begin{0U};
        u16 core{0U};
    };

    std::unique_ptr<Event[]> events{std::make_unique<Event[]>(kCapacity)};
    std::atomic<u64> head{0U};
    u32 threadId{0U};
    bool synthetic{false};
    std::string laneName;
    // Set by NoteThreadName; survives the thread exiting.
    std::mutex nameMutex;
    std::string threadName;
    // Only the owning thread touches these.
    std::array<Open, kMaxDepth> open{};
    u32 depth{0U};
};

// ---- registry --------------------------------------------------------------
std::mutex gRegistryMutex;
std::vector<std::shared_ptr<ThreadBuffer>> gBuffers;
std::vector<std::pair<const char*, std::shared_ptr<ThreadBuffer>>> gLanes;
std::mutex gInternMutex;
std::unordered_set<std::string> gInterned;

std::atomic<bool> gEnabled{true};
std::atomic<f64> gHitchMs{100.0};
std::atomic<f64> gStallMs{250.0};
std::atomic<f64> gWindowMs{4000.0};
std::atomic<u32> gMaxHitchFiles{24U};
std::mutex gConfigMutex;
Config gConfig;

[[nodiscard]] u64 ProcessBaseTicks() noexcept
{
    static const u64 base = NowTicks();
    return base;
}

thread_local std::string gPendingThreadName;

[[nodiscard]] ThreadBuffer* RegisterThisThread() noexcept
{
    try
    {
        auto buffer = std::make_shared<ThreadBuffer>();
        buffer->threadId = CurrentThreadId();
        buffer->threadName = gPendingThreadName;
        std::scoped_lock lock(gRegistryMutex);
        gBuffers.push_back(buffer);
        return buffer.get();
    }
    catch (...)
    {
        return nullptr;
    }
}

[[nodiscard]] ThreadBuffer* ThisThreadBuffer() noexcept
{
    thread_local ThreadBuffer* buffer = nullptr;
    if (buffer == nullptr)
    {
        static_cast<void>(ProcessBaseTicks());
        buffer = RegisterThisThread();
    }
    return buffer;
}

void WriteEvent(ThreadBuffer& buffer, const Event& event) noexcept
{
    const u64 head = buffer.head.load(std::memory_order_relaxed);
    buffer.events[head & (kCapacity - 1U)] = event;
    buffer.head.store(head + 1U, std::memory_order_release);
}

[[nodiscard]] ThreadBuffer* LaneBuffer(const char* lane) noexcept
{
    thread_local const char* cachedLane = nullptr;
    thread_local ThreadBuffer* cachedBuffer = nullptr;
    if (cachedLane == lane && cachedBuffer != nullptr)
    {
        return cachedBuffer;
    }

    try
    {
        std::scoped_lock lock(gRegistryMutex);
        for (const auto& [name, buffer] : gLanes)
        {
            if (std::string_view(name) == std::string_view(lane))
            {
                cachedLane = lane;
                cachedBuffer = buffer.get();
                return cachedBuffer;
            }
        }
        auto buffer = std::make_shared<ThreadBuffer>();
        buffer->synthetic = true;
        buffer->laneName = lane;
        buffer->threadId = 0x7F000000U + static_cast<u32>(gLanes.size());
        gLanes.emplace_back(lane, buffer);
        gBuffers.push_back(buffer);
        cachedLane = lane;
        cachedBuffer = buffer.get();
        return cachedBuffer;
    }
    catch (...)
    {
        return nullptr;
    }
}

// ---- frames ------------------------------------------------------------------
constexpr u32 kFrameHistory = 512U;
std::atomic<u64> gFrameIndex{0U};
std::atomic<u64> gFrameStartTicks{0U};
std::atomic<bool> gInFrame{false};
std::atomic<u64> gFramesCompleted{0U};
std::atomic<u64> gHitchCount{0U};
std::array<std::atomic<f32>, kFrameHistory> gFrameMs{};

struct HitchRequest
{
    u64 frame{0U};
    u64 beginTicks{0U};
    u64 endTicks{0U};
    f64 frameMs{0.0};
};

std::mutex gHitchMutex;
std::condition_variable gWake;
std::vector<HitchRequest> gPendingHitches;
std::vector<HitchInfo> gHitches;

// ---- watchdog ------------------------------------------------------------------
struct StackSample
{
    u64 ticks{0U};
    u64 frame{0U};
    std::array<void*, kMaxStackFrames> frames{};
    u32 count{0U};
};

std::mutex gSampleMutex;
std::vector<StackSample> gSamples;
std::thread gWatchdog;
std::atomic<bool> gStopWatchdog{false};
std::atomic<bool> gWatchdogRunning{false};
#if defined(_WIN32)
HANDLE gWatchedThread{nullptr};
#endif
u32 gWatchedThreadId{0U};

#if defined(_WIN64)
// No C++ objects with destructors in here: it uses SEH.
[[nodiscard]] u32 UnwindContext(
    CONTEXT& context,
    void** frames,
    const u32 capacity) noexcept
{
    u32 count = 0U;
    __try
    {
        while (count < capacity)
        {
            const DWORD64 pc = context.Rip;
            if (pc == 0U)
            {
                break;
            }
            frames[count++] = reinterpret_cast<void*>(pc);

            DWORD64 imageBase = 0U;
            PRUNTIME_FUNCTION function =
                RtlLookupFunctionEntry(pc, &imageBase, nullptr);
            if (function == nullptr)
            {
                // Leaf frame: the return address is at the top of the stack.
                const DWORD64 returnAddress =
                    *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rip = returnAddress;
                context.Rsp += sizeof(DWORD64);
            }
            else
            {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0U;
                RtlVirtualUnwind(
                    UNW_FLAG_NHANDLER,
                    imageBase,
                    pc,
                    function,
                    &context,
                    &handlerData,
                    &establisher,
                    nullptr);
            }

            if (context.Rip == 0U || context.Rsp == 0U)
            {
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return count;
}
#endif

[[nodiscard]] bool SampleWatchedThread(StackSample& sample) noexcept
{
#if defined(_WIN64)
    if (gWatchedThread == nullptr)
    {
        return false;
    }

    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(gWatchedThread) == static_cast<DWORD>(-1))
    {
        return false;
    }
    const BOOL captured = GetThreadContext(gWatchedThread, &context);
    // Resume before doing anything that could take a lock the thread holds.
    ResumeThread(gWatchedThread);
    if (captured == FALSE)
    {
        return false;
    }

    sample.count = UnwindContext(context, sample.frames.data(), kMaxStackFrames);
    return sample.count > 0U;
#else
    static_cast<void>(sample);
    return false;
#endif
}

// ---- symbolication -----------------------------------------------------------------
[[nodiscard]] std::string ModuleAndOffset(const void* address)
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address),
            &module) != FALSE &&
        module != nullptr)
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(module, path, MAX_PATH);
        std::string name(path);
        if (const auto slash = name.find_last_of("\\/");
            slash != std::string::npos)
        {
            name = name.substr(slash + 1U);
        }
        const auto offset = reinterpret_cast<std::uintptr_t>(address) -
            reinterpret_cast<std::uintptr_t>(module);
        return std::format("{}+0x{:x}", name, offset);
    }
#endif
    return std::format("0x{:x}", reinterpret_cast<std::uintptr_t>(address));
}

// dbghelp is not thread safe and both the writer thread and Capture() use it.
std::mutex gSymbolMutex;

std::unordered_map<std::uintptr_t, std::string> gSymbolCache;

#if defined(_WIN32)
// State of the private dbghelp session; guarded by gSymbolMutex.
bool gSymbolsInitialised = false;
bool gSymbolsAvailable = false;
std::vector<std::uintptr_t> gLoadedSymbolModules;

[[nodiscard]] HANDLE SymbolSession() noexcept
{
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x4F524249U));
}
#endif

// dbghelp keeps every PDB it has opened locked until the session ends, and a
// locked OrbitStudio.pdb makes the next hot build fail to link (LNK1201). So
// the session only lives while a batch of addresses is being resolved; results
// stay in gSymbolCache, so a repeat costs nothing and never reopens the PDB.
void ReleaseSymbols() noexcept
{
#if defined(_WIN32)
    std::scoped_lock symbolLock(gSymbolMutex);
    if (gSymbolsInitialised && gSymbolsAvailable)
    {
        SymCleanup(SymbolSession());
    }
    gSymbolsInitialised = false;
    gSymbolsAvailable = false;
    gLoadedSymbolModules.clear();
#endif
}

[[nodiscard]] std::string SymbolizeUncached(const void* address);

[[nodiscard]] std::string Symbolize(const void* address)
{
    {
        std::scoped_lock symbolLock(gSymbolMutex);
        if (const auto found =
                gSymbolCache.find(reinterpret_cast<std::uintptr_t>(address));
            found != gSymbolCache.end())
        {
            return found->second;
        }
    }
    std::string text = SymbolizeUncached(address);
    std::scoped_lock symbolLock(gSymbolMutex);
    if (gSymbolCache.size() > 20000U)
    {
        gSymbolCache.clear();
    }
    gSymbolCache.emplace(reinterpret_cast<std::uintptr_t>(address), text);
    return text;
}

[[nodiscard]] std::string SymbolizeUncached(const void* address)
{
    std::scoped_lock symbolLock(gSymbolMutex);
#if defined(_WIN32)
    // A private dbghelp session (a made-up process handle, no invade): anything
    // else in the process that already called SymInitialize for the real process
    // handle makes a second SymInitialize fail, which used to silently turn
    // symbolisation off. Modules are loaded explicitly, on first use.
    const HANDLE session = SymbolSession();
    bool& available = gSymbolsAvailable;
    std::vector<std::uintptr_t>& loadedModules = gLoadedSymbolModules;
    if (!gSymbolsInitialised)
    {
        gSymbolsInitialised = true;
        SymSetOptions(
            SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        available = SymInitialize(session, nullptr, FALSE) != FALSE;
    }

    if (available)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCSTR>(address),
                &module) != FALSE &&
            module != nullptr)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(module);
            if (std::find(loadedModules.begin(), loadedModules.end(), base) ==
                loadedModules.end())
            {
                loadedModules.push_back(base);
                char path[MAX_PATH]{};
                GetModuleFileNameA(module, path, MAX_PATH);
                const auto* dos =
                    reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                    reinterpret_cast<const char*>(module) + dos->e_lfanew);
                SymLoadModuleEx(
                    session,
                    nullptr,
                    path,
                    nullptr,
                    static_cast<DWORD64>(base),
                    nt->OptionalHeader.SizeOfImage,
                    nullptr,
                    0U);
            }
        }
    }

    if (available)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256U]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255U;
        DWORD64 displacement = 0U;
        if (SymFromAddr(
                session,
                reinterpret_cast<DWORD64>(address),
                &displacement,
                symbol) != FALSE)
        {
            std::string text = std::format(
                "{}+0x{:x}", symbol->Name, displacement);
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0U;
            if (SymGetLineFromAddr64(
                    session,
                    reinterpret_cast<DWORD64>(address),
                    &lineDisplacement,
                    &line) != FALSE &&
                line.FileName != nullptr)
            {
                std::string file(line.FileName);
                if (const auto slash = file.find_last_of("\\/");
                    slash != std::string::npos)
                {
                    file = file.substr(slash + 1U);
                }
                text += std::format(" ({}:{})", file, line.LineNumber);
            }
            return text;
        }
    }
#endif
    return ModuleAndOffset(address);
}

[[nodiscard]] bool IsSystemFrame(const std::string& symbol)
{
    std::string lower = symbol;
    std::transform(
        lower.begin(),
        lower.end(),
        lower.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // Unsymbolised system frames are "module.dll+0xOFFSET".
    for (const char* prefix :
         {"ntdll", "kernelbase", "kernel32", "win32u", "user32", "gdi32",
          "vulkan-1", "nvoglv", "nvwgf", "dxgi", "ucrtbase", "msvcp",
          "vcruntime", "nvcuda"})
    {
        if (lower.rfind(prefix, 0U) == 0U)
        {
            return true;
        }
    }
    return false;
}

// ---- output ----------------------------------------------------------------------------
[[nodiscard]] std::string JsonEscape(const std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2U);
    for (const char c : text)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U)
            {
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            }
            else
            {
                out += c;
            }
        }
    }
    return out;
}

[[nodiscard]] std::string ThreadDisplayName(const ThreadBuffer& buffer)
{
    if (buffer.synthetic)
    {
        return buffer.laneName;
    }
    {
        std::scoped_lock lock(const_cast<ThreadBuffer&>(buffer).nameMutex);
        if (!buffer.threadName.empty())
        {
            return buffer.threadName;
        }
    }
#if defined(_WIN32)
    if (HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, buffer.threadId);
        thread != nullptr)
    {
        PWSTR description = nullptr;
        std::string result;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) &&
            description != nullptr)
        {
            for (const wchar_t* c = description; *c != L'\0'; ++c)
            {
                result.push_back(
                    *c < 0x80 ? static_cast<char>(*c) : '?');
            }
            LocalFree(description);
        }
        CloseHandle(thread);
        if (!result.empty())
        {
            return result;
        }
    }
    else
    {
        return std::format("Thread {} (exited)", buffer.threadId);
    }
#endif
    return std::format("Thread {}", buffer.threadId);
}

struct LaneSnapshot
{
    std::string name;
    u32 threadId{0U};
    bool synthetic{false};
    std::vector<Event> events;
};

[[nodiscard]] std::vector<LaneSnapshot> CollectLanes(const u64 cutoffTicks)
{
    std::vector<std::shared_ptr<ThreadBuffer>> buffers;
    {
        std::scoped_lock lock(gRegistryMutex);
        buffers = gBuffers;
    }

    std::vector<LaneSnapshot> lanes;
    lanes.reserve(buffers.size());
    for (const auto& buffer : buffers)
    {
        LaneSnapshot lane;
        lane.name = ThreadDisplayName(*buffer);
        lane.threadId = buffer->threadId;
        lane.synthetic = buffer->synthetic;

        const u64 head = buffer->head.load(std::memory_order_acquire);
        const u64 count = std::min<u64>(head, kCapacity);
        lane.events.reserve(static_cast<std::size_t>(count));
        for (u64 i = head - count; i < head; ++i)
        {
            const Event event = buffer->events[i & (kCapacity - 1U)];
            // A slot being overwritten while copied fails these checks.
            if (event.name == nullptr || event.end < event.begin ||
                event.end < cutoffTicks)
            {
                continue;
            }
            lane.events.push_back(event);
        }
        std::sort(
            lane.events.begin(),
            lane.events.end(),
            [](const Event& a, const Event& b) { return a.begin < b.begin; });
        if (!lane.events.empty())
        {
            lanes.push_back(std::move(lane));
        }
    }
    return lanes;
}

[[nodiscard]] f64 ToMicroseconds(const u64 ticks) noexcept
{
    const u64 base = ProcessBaseTicks();
    const f64 relative = ticks >= base
        ? static_cast<f64>(ticks - base)
        : -static_cast<f64>(base - ticks);
    return relative * 1.0e6 / static_cast<f64>(QpcFrequency());
}

[[nodiscard]] std::filesystem::path DefaultOutputDirectory()
{
#if defined(_WIN32)
    const auto fromEnvironment = [](const char* name) -> std::string
    {
        char* value = nullptr;
        std::size_t length = 0U;
        std::string result;
        if (_dupenv_s(&value, &length, name) == 0 && value != nullptr)
        {
            result = value;
            std::free(value);
        }
        return result;
    };

    if (const auto override = fromEnvironment("ORBIT_PROFILER_DIR");
        !override.empty())
    {
        return std::filesystem::path(override);
    }
    if (const auto local = fromEnvironment("LOCALAPPDATA"); !local.empty())
    {
        return std::filesystem::path(local) / "Orbit" / "profiler";
    }
#endif
    return std::filesystem::temp_directory_path() / "orbit_profiler";
}

struct TraceResult
{
    u64 events{0U};
    u32 threads{0U};
};

// Writes every lane's events newer than `cutoffTicks` as a Chrome trace: one
// process with a lane per thread and a second with a lane per CPU core.
[[nodiscard]] TraceResult WriteTrace(
    const std::filesystem::path& path,
    const u64 cutoffTicks,
    const std::vector<StackSample>& samples,
    const std::string& headline)
{
    const auto lanes = CollectLanes(cutoffTicks);

    std::error_code error;
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        throw std::runtime_error(
            "Orbit profiler could not open " + path.string() + ".");
    }

    TraceResult result;
    std::string text;
    text.reserve(1U << 20U);
    text += "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n";
    bool first = true;
    const auto separator = [&]
    {
        if (!first)
        {
            text += ",\n";
        }
        first = false;
    };
    const auto flush = [&]
    {
        if (text.size() > (1U << 19U))
        {
            out << text;
            text.clear();
        }
    };

    separator();
    text += "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"args\":{\"name\":\"Orbit threads"
        + (headline.empty() ? std::string() : " - " + JsonEscape(headline)) + "\"}}";
    separator();
    text += "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":2,\"args\":{\"name\":\"CPU cores\"}}";

    std::map<u16, bool> coreLanes;
    u32 sortIndex = 0U;
    u32 mainThreadId = gWatchedThreadId;
    for (const auto& lane : lanes)
    {
        ++result.threads;
        separator();
        text += std::format(
            "{{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":{},\"args\":{{\"name\":\"{}\"}}}}",
            lane.threadId,
            JsonEscape(lane.name));
        separator();
        text += std::format(
            "{{\"name\":\"thread_sort_index\",\"ph\":\"M\",\"pid\":1,\"tid\":{},\"args\":{{\"sort_index\":{}}}}}",
            lane.threadId,
            lane.synthetic ? 0U : ++sortIndex);

        for (const auto& event : lane.events)
        {
            const f64 begin = ToMicroseconds(event.begin);
            const f64 duration =
                std::max(ToMicroseconds(event.end) - begin, 0.001);
            separator();
            text += std::format(
                "{{\"name\":\"{}\",\"ph\":\"X\",\"ts\":{:.3f},\"dur\":{:.3f},\"pid\":1,\"tid\":{},\"args\":{{\"core\":{},\"depth\":{}}}}}",
                JsonEscape(event.name),
                begin,
                duration,
                lane.threadId,
                event.beginCore,
                event.depth);
            ++result.events;

            if (!lane.synthetic && duration >= 30.0)
            {
                coreLanes[event.beginCore] = true;
                separator();
                text += std::format(
                    "{{\"name\":\"{}\",\"ph\":\"X\",\"ts\":{:.3f},\"dur\":{:.3f},\"pid\":2,\"tid\":{},\"args\":{{\"thread\":\"{}\"}}}}",
                    JsonEscape(event.name),
                    begin,
                    duration,
                    event.beginCore,
                    JsonEscape(lane.name));
            }
            flush();
        }
    }

    for (const auto& [core, used] : coreLanes)
    {
        static_cast<void>(used);
        separator();
        text += std::format(
            "{{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":2,\"tid\":{},\"args\":{{\"name\":\"Core {}\"}}}}",
            core,
            core);
    }

    // Stack samples taken while the watched thread was stalled.
    for (const auto& sample : samples)
    {
        std::string stack = "[";
        std::string label;
        for (u32 i = 0U; i < sample.count; ++i)
        {
            const std::string symbol = Symbolize(sample.frames[i]);
            if (i != 0U)
            {
                stack += ",";
            }
            stack += "\"" + JsonEscape(symbol) + "\"";
            if (label.empty() && !IsSystemFrame(symbol))
            {
                label = symbol;
            }
        }
        stack += "]";
        if (label.empty() && sample.count > 0U)
        {
            label = Symbolize(sample.frames[0]);
        }
        separator();
        text += std::format(
            "{{\"name\":\"STALLED in {}\",\"ph\":\"i\",\"s\":\"t\",\"ts\":{:.3f},\"pid\":1,\"tid\":{},\"args\":{{\"stack\":{}}}}}",
            JsonEscape(label),
            ToMicroseconds(sample.ticks),
            mainThreadId,
            stack);
        ++result.events;
        flush();
    }

    text += "\n]}\n";
    out << text;
    ReleaseSymbols();
    return result;
}

void PruneHitchFiles(const std::filesystem::path& directory, const u32 keep)
{
    std::error_code error;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        const auto name = entry.path().filename().string();
        if (name.rfind("hitch-", 0U) == 0U && entry.path().extension() == ".json")
        {
            files.emplace_back(entry.last_write_time(error), entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    while (files.size() > keep)
    {
        std::filesystem::remove(files.front().second, error);
        files.erase(files.begin());
    }
}

void WriteHitch(const HitchRequest& request)
{
    std::vector<StackSample> samples;
    {
        std::scoped_lock lock(gSampleMutex);
        for (const auto& sample : gSamples)
        {
            if (sample.frame == request.frame)
            {
                samples.push_back(sample);
            }
        }
    }

    const f64 windowMs = gWindowMs.load(std::memory_order_relaxed);
    const u64 windowTicks = static_cast<u64>(
        windowMs * static_cast<f64>(QpcFrequency()) / 1000.0);
    // The window is measured back from when the long frame *began*, not from when
    // it ended: a 12 s stall would otherwise keep only its own last 4 s and lose
    // the frames that led into it.
    const u64 anchor = request.beginTicks != 0U ? request.beginTicks : request.endTicks;
    const u64 cutoff = anchor > windowTicks ? anchor - windowTicks : 0U;

    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#endif
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

    const std::filesystem::path directory = OutputDirectory();
    const std::filesystem::path path = directory /
        std::format(
            "hitch-{}-f{}-{:.0f}ms.json", stamp, request.frame, request.frameMs);

    HitchInfo info;
    info.path = path;
    info.frame = request.frame;
    info.frameMs = request.frameMs;
    info.time = stamp;
    info.stackSamples = static_cast<u32>(samples.size());

    try
    {
        static_cast<void>(WriteTrace(
            path,
            cutoff,
            samples,
            std::format("hitch frame {} ({:.0f} ms)", request.frame, request.frameMs)));

        // The frames most often on top of the sampled stacks.
        std::map<std::string, u32> counts;
        for (const auto& sample : samples)
        {
            std::string chosen;
            for (u32 i = 0U; i < sample.count; ++i)
            {
                const std::string symbol = Symbolize(sample.frames[i]);
                if (!IsSystemFrame(symbol))
                {
                    chosen = symbol;
                    break;
                }
            }
            if (chosen.empty() && sample.count > 0U)
            {
                chosen = Symbolize(sample.frames[0]);
            }
            if (!chosen.empty())
            {
                ++counts[chosen];
            }
        }
        std::vector<std::pair<u32, std::string>> ranked;
        for (const auto& [symbol, count] : counts)
        {
            ranked.emplace_back(count, symbol);
        }
        std::sort(ranked.begin(), ranked.end(), std::greater<>());
        for (std::size_t i = 0U; i < ranked.size() && i < 5U; ++i)
        {
            info.topFrames.push_back(
                std::format("{}x {}", ranked[i].first, ranked[i].second));
        }

        PruneHitchFiles(directory, gMaxHitchFiles.load(std::memory_order_relaxed));
        ReleaseSymbols();
    }
    catch (...)
    {
        ReleaseSymbols();
        return;
    }

    std::scoped_lock lock(gHitchMutex);
    gHitches.push_back(std::move(info));
    if (gHitches.size() > 64U)
    {
        gHitches.erase(gHitches.begin());
    }
}

void WatchdogMain()
{
    core::SetCurrentThreadName("Orbit.Profiler");
    u64 lastSample = 0U;

    while (!gStopWatchdog.load(std::memory_order_acquire))
    {
        {
            std::unique_lock lock(gHitchMutex);
            gWake.wait_for(lock, std::chrono::milliseconds(10));
        }

        // Stall sampling: the watched thread is still inside a frame.
        if (gInFrame.load(std::memory_order_acquire))
        {
            const u64 now = NowTicks();
            const f64 inFrameMs = TicksToMilliseconds(
                now - gFrameStartTicks.load(std::memory_order_acquire));
            const f64 sinceLast = TicksToMilliseconds(now - lastSample);
            if (inFrameMs > gStallMs.load(std::memory_order_relaxed) &&
                sinceLast >= 50.0)
            {
                lastSample = now;
                StackSample sample;
                sample.ticks = now;
                sample.frame = gFrameIndex.load(std::memory_order_acquire);
                if (SampleWatchedThread(sample))
                {
                    std::scoped_lock lock(gSampleMutex);
                    gSamples.push_back(sample);
                    if (gSamples.size() > 512U)
                    {
                        gSamples.erase(gSamples.begin(), gSamples.begin() + 128);
                    }
                }
            }
        }

        std::vector<HitchRequest> pending;
        {
            std::scoped_lock lock(gHitchMutex);
            pending.swap(gPendingHitches);
        }
        for (const auto& request : pending)
        {
            WriteHitch(request);
        }
    }
}
} // namespace

// ---- public API ---------------------------------------------------------------------------
u64 NowTicks() noexcept
{
#if defined(_WIN32)
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<u64>(value.QuadPart);
#else
    return static_cast<u64>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

f64 TicksToMilliseconds(const u64 ticks) noexcept
{
    return static_cast<f64>(ticks) * 1000.0 / static_cast<f64>(QpcFrequency());
}

f64 TicksPerMillisecond() noexcept
{
    return static_cast<f64>(QpcFrequency()) / 1000.0;
}

bool Enabled() noexcept
{
    return gEnabled.load(std::memory_order_relaxed);
}

void BeginScope(const char* name) noexcept
{
    ThreadBuffer* buffer = ThisThreadBuffer();
    if (buffer == nullptr)
    {
        return;
    }
    if (buffer->depth < kMaxDepth)
    {
        buffer->open[buffer->depth] = {name, NowTicks(), CurrentCore()};
    }
    ++buffer->depth;
}

void EndScope() noexcept
{
    ThreadBuffer* buffer = ThisThreadBuffer();
    if (buffer == nullptr || buffer->depth == 0U)
    {
        return;
    }
    --buffer->depth;
    if (buffer->depth >= kMaxDepth)
    {
        return;
    }
    const auto open = buffer->open[buffer->depth];
    WriteEvent(
        *buffer,
        Event{
            .name = open.name,
            .begin = open.begin,
            .end = NowTicks(),
            .beginCore = open.core,
            .endCore = CurrentCore(),
            .depth = static_cast<u16>(buffer->depth),
            .reserved = 0U
        });
}

void RecordSpan(
    const char* name,
    const u64 beginTicks,
    const u64 endTicks) noexcept
{
    if (!Enabled())
    {
        return;
    }
    ThreadBuffer* buffer = ThisThreadBuffer();
    if (buffer == nullptr)
    {
        return;
    }
    WriteEvent(
        *buffer,
        Event{
            .name = name,
            .begin = beginTicks,
            .end = endTicks,
            .beginCore = CurrentCore(),
            .endCore = CurrentCore(),
            .depth = static_cast<u16>(std::min(buffer->depth, kMaxDepth)),
            .reserved = 0U
        });
}

void RecordLaneSpan(
    const char* lane,
    const char* name,
    const u64 beginTicks,
    const u64 endTicks) noexcept
{
    if (!Enabled())
    {
        return;
    }
    ThreadBuffer* buffer = LaneBuffer(lane);
    if (buffer == nullptr)
    {
        return;
    }
    WriteEvent(
        *buffer,
        Event{
            .name = name,
            .begin = beginTicks,
            .end = endTicks,
            .beginCore = 0U,
            .endCore = 0U,
            .depth = 0U,
            .reserved = 0U
        });
}

void NoteThreadName(const std::string_view name)
{
    try
    {
        gPendingThreadName.assign(name);
        // Already registered? Update its record too.
        std::scoped_lock lock(gRegistryMutex);
        const u32 id = CurrentThreadId();
        for (const auto& buffer : gBuffers)
        {
            if (!buffer->synthetic && buffer->threadId == id)
            {
                std::scoped_lock nameLock(buffer->nameMutex);
                buffer->threadName.assign(name);
            }
        }
    }
    catch (...)
    {
    }
}

const char* Intern(const std::string_view name)
{
    std::scoped_lock lock(gInternMutex);
    return gInterned.emplace(name).first->c_str();
}

void BeginFrame() noexcept
{
    gFrameIndex.fetch_add(1U, std::memory_order_acq_rel);
    gFrameStartTicks.store(NowTicks(), std::memory_order_release);
    gInFrame.store(true, std::memory_order_release);
}

void CancelFrame() noexcept
{
    gInFrame.store(false, std::memory_order_release);
}

void EndFrame() noexcept
{
    const u64 end = NowTicks();
    const u64 begin = gFrameStartTicks.load(std::memory_order_acquire);
    gInFrame.store(false, std::memory_order_release);

    const f64 milliseconds = TicksToMilliseconds(end - begin);
    const u64 index = gFramesCompleted.fetch_add(1U, std::memory_order_acq_rel);
    gFrameMs[index % kFrameHistory].store(
        static_cast<f32>(milliseconds), std::memory_order_relaxed);

    RecordLaneSpan("Frames", "frame", begin, end);

    if (Enabled() &&
        milliseconds > gHitchMs.load(std::memory_order_relaxed) &&
        gWatchdogRunning.load(std::memory_order_acquire))
    {
        gHitchCount.fetch_add(1U, std::memory_order_relaxed);
        try
        {
            {
                std::scoped_lock lock(gHitchMutex);
                gPendingHitches.push_back({
                    .frame = gFrameIndex.load(std::memory_order_acquire),
                    .beginTicks = begin,
                    .endTicks = end,
                    .frameMs = milliseconds
                });
            }
            gWake.notify_one();
        }
        catch (...)
        {
        }
    }
}

FrameSummary Frames() noexcept
{
    FrameSummary summary;
    summary.frames = gFramesCompleted.load(std::memory_order_acquire);
    summary.hitches = gHitchCount.load(std::memory_order_relaxed);
    const u64 count = std::min<u64>(summary.frames, 240U);
    f64 sum = 0.0;
    for (u64 i = 0U; i < count; ++i)
    {
        const u64 slot = (summary.frames - 1U - i) % kFrameHistory;
        const f64 value = gFrameMs[slot].load(std::memory_order_relaxed);
        if (i == 0U)
        {
            summary.lastMs = value;
        }
        sum += value;
        summary.worstMs = std::max(summary.worstMs, value);
    }
    summary.averageMs = count == 0U ? 0.0 : sum / static_cast<f64>(count);
    return summary;
}

std::vector<f32> RecentFrameMilliseconds(const u32 count)
{
    const u64 frames = gFramesCompleted.load(std::memory_order_acquire);
    const u32 available = static_cast<u32>(
        std::min<u64>({frames, static_cast<u64>(count), kFrameHistory}));
    std::vector<f32> result;
    result.reserve(available);
    for (u32 i = 0U; i < available; ++i)
    {
        const u64 slot = (frames - available + i) % kFrameHistory;
        result.push_back(gFrameMs[slot].load(std::memory_order_relaxed));
    }
    return result;
}

void Configure(const Config& config)
{
    {
        std::scoped_lock lock(gConfigMutex);
        gConfig = config;
    }
    gEnabled.store(config.enabled, std::memory_order_relaxed);
    gHitchMs.store(std::max(config.hitchThresholdMs, 1.0), std::memory_order_relaxed);
    gStallMs.store(std::max(config.stallThresholdMs, 10.0), std::memory_order_relaxed);
    gWindowMs.store(
        std::clamp(config.captureWindowMs, 250.0, 15000.0),
        std::memory_order_relaxed);
    gMaxHitchFiles.store(std::max(config.maxHitchFiles, 1U), std::memory_order_relaxed);
}

Config CurrentConfig()
{
    std::scoped_lock lock(gConfigMutex);
    Config copy = gConfig;
    copy.enabled = gEnabled.load(std::memory_order_relaxed);
    copy.hitchThresholdMs = gHitchMs.load(std::memory_order_relaxed);
    copy.stallThresholdMs = gStallMs.load(std::memory_order_relaxed);
    copy.captureWindowMs = gWindowMs.load(std::memory_order_relaxed);
    copy.maxHitchFiles = gMaxHitchFiles.load(std::memory_order_relaxed);
    return copy;
}

std::filesystem::path OutputDirectory()
{
    {
        std::scoped_lock lock(gConfigMutex);
        if (!gConfig.outputDirectory.empty())
        {
            return gConfig.outputDirectory;
        }
    }
    return DefaultOutputDirectory();
}

void StartWatchdog()
{
    if (gWatchdogRunning.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    static_cast<void>(ProcessBaseTicks());
    gWatchedThreadId = CurrentThreadId();
#if defined(_WIN32)
    HANDLE duplicate = nullptr;
    if (DuplicateHandle(
            GetCurrentProcess(),
            GetCurrentThread(),
            GetCurrentProcess(),
            &duplicate,
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE,
            0) != FALSE)
    {
        gWatchedThread = duplicate;
    }
#endif
    gStopWatchdog.store(false, std::memory_order_release);
    gWatchdog = std::thread(WatchdogMain);
}

void StopWatchdog()
{
    if (!gWatchdogRunning.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    gStopWatchdog.store(true, std::memory_order_release);
    gWake.notify_all();
    if (gWatchdog.joinable())
    {
        gWatchdog.join();
    }
#if defined(_WIN32)
    if (gWatchedThread != nullptr)
    {
        CloseHandle(gWatchedThread);
        gWatchedThread = nullptr;
    }
#endif
}

CaptureInfo Capture(const std::filesystem::path& path, const f64 windowMs)
{
    const f64 clamped = std::clamp(windowMs, 100.0, 15000.0);
    const u64 now = NowTicks();
    const u64 windowTicks = static_cast<u64>(
        clamped * static_cast<f64>(QpcFrequency()) / 1000.0);
    const u64 cutoff = now > windowTicks ? now - windowTicks : 0U;

    std::vector<StackSample> samples;
    {
        std::scoped_lock lock(gSampleMutex);
        for (const auto& sample : gSamples)
        {
            if (sample.ticks >= cutoff)
            {
                samples.push_back(sample);
            }
        }
    }

    const TraceResult result = WriteTrace(
        path,
        cutoff,
        samples,
        std::format("capture of the last {:.0f} ms", clamped));

    CaptureInfo info;
    info.path = path;
    info.events = result.events;
    info.threads = result.threads;
    info.windowMs = clamped;
    return info;
}

f64 NowMilliseconds() noexcept
{
    return ToMicroseconds(NowTicks()) / 1000.0;
}

Snapshot TakeSnapshot(const f64 windowMs, const bool includeStalls)
{
    const f64 clamped = std::clamp(windowMs, 10.0, 60000.0);
    const u64 now = NowTicks();
    const u64 windowTicks = static_cast<u64>(
        clamped * static_cast<f64>(QpcFrequency()) / 1000.0);
    const u64 cutoff = now > windowTicks ? now - windowTicks : 0U;

    Snapshot snapshot;
    snapshot.source = "live";
    snapshot.endMs = ToMicroseconds(now) / 1000.0;
    snapshot.beginMs = snapshot.endMs - clamped;

    auto lanes = CollectLanes(cutoff);
    snapshot.lanes.reserve(lanes.size());
    for (auto& source : lanes)
    {
        SnapshotLane lane;
        lane.name = std::move(source.name);
        lane.threadId = source.threadId;
        lane.synthetic = source.synthetic;
        lane.slices.reserve(source.events.size());
        for (const Event& event : source.events)
        {
            const f64 begin = ToMicroseconds(event.begin) / 1000.0;
            const f64 end = ToMicroseconds(event.end) / 1000.0;
            lane.slices.push_back({
                .startMs = begin,
                .durationMs = std::max(end - begin, 0.0),
                .name = event.name,
                .depth = event.depth,
                .core = event.beginCore});
        }
        snapshot.sliceCount += lane.slices.size();
        snapshot.lanes.push_back(std::move(lane));
    }
    if (includeStalls)
    {
        std::vector<StackSample> samples;
        {
            std::scoped_lock lock(gSampleMutex);
            for (const auto& sample : gSamples)
            {
                if (sample.ticks >= cutoff)
                {
                    samples.push_back(sample);
                }
            }
        }
        for (const auto& sample : samples)
        {
            SnapshotStall stall;
            stall.timeMs = ToMicroseconds(sample.ticks) / 1000.0;
            stall.frame = sample.frame;
            for (u32 i = 0U; i < sample.count; ++i)
            {
                std::string symbol = Symbolize(sample.frames[i]);
                if (stall.label.empty() && !IsSystemFrame(symbol))
                {
                    stall.label = symbol;
                }
                stall.stack.push_back(std::move(symbol));
            }
            if (stall.label.empty() && !stall.stack.empty())
            {
                stall.label = stall.stack.front();
            }
            snapshot.stalls.push_back(std::move(stall));
        }
        ReleaseSymbols();
    }
    return snapshot;
}

std::filesystem::path DefaultCapturePath()
{
    const auto stamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    return OutputDirectory() / ("capture-" + std::to_string(stamp) + ".json");
}

std::vector<HitchInfo> RecentHitches()
{
    std::scoped_lock lock(gHitchMutex);
    return gHitches;
}

namespace
{
// Joins the watchdog at static destruction so exiting with it running cannot
// reach std::terminate. Declared last so it is destroyed before the state the
// watchdog thread uses.
struct WatchdogShutdownGuard
{
    ~WatchdogShutdownGuard()
    {
        StopWatchdog();
    }
};

const WatchdogShutdownGuard gWatchdogShutdownGuard;
} // namespace
} // namespace orbit::profiler
