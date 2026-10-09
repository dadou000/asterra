#include <orbit/weather_lab/WeatherLabSession.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

namespace orbit::weather_lab
{
namespace
{
using Clock = std::chrono::steady_clock;

double Seconds(const Clock::time_point from, const Clock::time_point to)
{
    return std::chrono::duration<double>(to - from).count();
}

// Display fields of one frame, all nx*ny*nz.
struct DisplayFields
{
    bool valid = false;
    WxHeader header;
    double time = 0.0;
    std::map<std::string, std::vector<float>> fields;
};

const std::vector<std::string>& FieldNames()
{
    static const std::vector<std::string> names{
        "w", "qr", "qc", "qv", "thp", "zvort", "speed", "condensate"};
    return names;
}

// Turns raw model fields (th qv qc qr u v w zvort [+ ice]) into the derived
// display set. `thetaBase` is the per-level reference for thp.
void BuildDisplay(
    const WxHeader& header,
    const std::map<std::string, std::vector<float>>& raw,
    const std::vector<float>& thetaBase,
    DisplayFields& out)
{
    const std::size_t cells = header.CellCount();
    const std::size_t plane = static_cast<std::size_t>(header.nx) * header.ny;
    auto get = [&](const char* name) -> const std::vector<float>*
    {
        const auto it = raw.find(name);
        return it == raw.end() ? nullptr : &it->second;
    };
    out.header = header;
    out.fields.clear();
    for (const char* name : {"w", "qr", "qc", "qv", "zvort"})
    {
        const auto* src = get(name);
        out.fields[name] = src != nullptr ? *src : std::vector<float>(cells, 0.0F);
    }
    std::vector<float>& thp = out.fields["thp"];
    thp.assign(cells, 0.0F);
    if (const auto* th = get("th"))
    {
        for (std::size_t c = 0; c < cells; ++c)
        {
            thp[c] = (*th)[c] - thetaBase[c / plane];
        }
    }
    std::vector<float>& speed = out.fields["speed"];
    speed.assign(cells, 0.0F);
    const auto* u = get("u");
    const auto* v = get("v");
    if (u != nullptr && v != nullptr)
    {
        for (std::size_t c = 0; c < cells; ++c)
        {
            speed[c] = std::hypot(
                (*u)[c] - header.moveU, (*v)[c] - header.moveV);
        }
    }
    std::vector<float>& condensate = out.fields["condensate"];
    condensate.assign(cells, 0.0F);
    for (const char* name : {"qc", "qr", "qi", "qs", "qg"})
    {
        if (const auto* q = get(name))
        {
            for (std::size_t c = 0; c < cells; ++c)
            {
                condensate[c] += (*q)[c];
            }
        }
    }
    out.valid = true;
}

Slice ExtractSlice(const DisplayFields& d, const SliceRequest& r)
{
    Slice slice;
    slice.field = r.field;
    slice.unit = SliceFieldUnit(r.field);
    if (!d.valid)
    {
        slice.error = r.source == DisplaySource::Live
            ? "no live run yet (Start or Step first)"
            : "no playback loaded";
        return slice;
    }
    const auto it = d.fields.find(r.field);
    if (it == d.fields.end())
    {
        slice.error = "unknown field '" + r.field + "'";
        return slice;
    }
    const std::vector<float>& f = it->second;
    const WxHeader& h = d.header;
    const std::size_t plane = static_cast<std::size_t>(h.nx) * h.ny;
    slice.time = d.time;
    const float scale = r.field == "qr" || r.field == "qc" || r.field == "qv"
        || r.field == "condensate" ? 1000.0F : 1.0F;
    auto at = [&](const std::uint32_t i, const std::uint32_t j,
                  const std::uint32_t k)
    {
        return f[(static_cast<std::size_t>(k) * h.ny + j) * h.nx + i] * scale;
    };
    if (scale != 1.0F)
    {
        slice.unit = "g/kg";
    }
    switch (r.kind)
    {
    case SliceKind::Plan:
    case SliceKind::ColumnMax:
    {
        slice.width = h.nx;
        slice.height = h.ny;
        slice.cellWidthMeters = h.dx;
        slice.cellHeightMeters = h.dy;
        slice.values.assign(plane, 0.0F);
        std::uint32_t level = 0;
        if (r.kind == SliceKind::Plan)
        {
            float best = 1.0e30F;
            for (std::uint32_t k = 0; k < h.nz; ++k)
            {
                const float away = std::fabs(h.centreHeight[k] - r.height);
                if (away < best)
                {
                    best = away;
                    level = k;
                }
            }
            slice.title = r.field + " at "
                + std::to_string(static_cast<int>(h.centreHeight[level])) + " m";
        }
        else
        {
            slice.title = "column max " + r.field;
        }
        for (std::uint32_t j = 0; j < h.ny; ++j)
        {
            for (std::uint32_t i = 0; i < h.nx; ++i)
            {
                float value = 0.0F;
                if (r.kind == SliceKind::Plan)
                {
                    value = at(i, j, level);
                }
                else
                {
                    value = -1.0e30F;
                    for (std::uint32_t k = 0; k < h.nz; ++k)
                    {
                        value = std::max(value, at(i, j, k));
                    }
                }
                slice.values[static_cast<std::size_t>(j) * h.nx + i] = value;
            }
        }
        break;
    }
    case SliceKind::Section:
    {
        std::uint32_t row = 0;
        if (r.row >= 0)
        {
            row = std::min(static_cast<std::uint32_t>(r.row), h.ny - 1U);
        }
        else
        {
            const std::vector<float>& w = d.fields.at("w");
            float best = -1.0e30F;
            for (std::size_t c = 0; c < w.size(); ++c)
            {
                if (w[c] > best)
                {
                    best = w[c];
                    row = static_cast<std::uint32_t>((c % plane) / h.nx);
                }
            }
        }
        slice.width = h.nx;
        slice.height = h.nz;
        slice.cellWidthMeters = h.dx;
        slice.cellHeightMeters = h.nz > 1U
            ? h.centreHeight[1] - h.centreHeight[0] : 0.0F;
        slice.title = r.field + " section, y row " + std::to_string(row);
        slice.values.assign(static_cast<std::size_t>(h.nx) * h.nz, 0.0F);
        for (std::uint32_t k = 0; k < h.nz; ++k)
        {
            for (std::uint32_t i = 0; i < h.nx; ++i)
            {
                slice.values[static_cast<std::size_t>(k) * h.nx + i] = at(i, row, k);
            }
        }
        break;
    }
    }
    slice.minValue = slice.values.front();
    slice.maxValue = slice.values.front();
    for (std::size_t c = 0; c < slice.values.size(); ++c)
    {
        if (slice.values[c] < slice.minValue) { slice.minValue = slice.values[c]; }
        if (slice.values[c] > slice.maxValue)
        {
            slice.maxValue = slice.values[c];
            slice.peakColumn = static_cast<std::uint32_t>(c % slice.width);
            slice.peakRow = static_cast<std::uint32_t>(c / slice.width);
        }
    }
    slice.valid = true;
    return slice;
}
} // namespace

const char* SessionStateName(const SessionState state) noexcept
{
    switch (state)
    {
    case SessionState::Idle: return "idle";
    case SessionState::Paused: return "paused";
    case SessionState::Running: return "running";
    case SessionState::Finished: return "finished";
    case SessionState::Failed: return "failed";
    }
    return "idle";
}

const std::vector<std::string>& SliceFieldNames() { return FieldNames(); }

std::string SliceFieldUnit(const std::string& field)
{
    if (field == "w" || field == "speed") { return "m/s"; }
    if (field == "thp") { return "K"; }
    if (field == "zvort") { return "1/s"; }
    return "kg/kg";
}

// ------------------------------------------------------------------ session

struct WeatherLabSession::Impl
{
    mutable std::mutex mutex;
    std::condition_variable wake;
    WeatherLabSettings settings;

    // Live run. `solver` is touched by the compute thread while it runs and by
    // the API only while it is stopped (Reset joins before destroying).
    std::unique_ptr<FastStormSolver> solver;
    std::thread worker;
    bool exitRequested = false;
    SessionState state = SessionState::Idle;
    std::string error;
    double stepBudget = 0.0;
    double simTime = 0.0;
    std::uint64_t steps = 0;
    double wallSeconds = 0.0;
    FastStormDiagnostics diagnostics;
    std::size_t residentBytes = 0;
    std::vector<StormMetrics> liveMetrics;
    DisplayFields liveDisplay;
    std::vector<double> frameTimes;
    std::size_t nextFrame = 0;
    double baseThetaMean = 0.0;
    bool recording = false;
    bool initializing = false;
    WxWriter writer;
    double runStartWall = 0.0; // compute-seconds at the last resume
    double runStartSim = 0.0;
    Clock::time_point runStartClock{};

    // Playback.
    WxReader reader;
    bool hasPlayback = false;
    std::string playbackPath;
    std::size_t playbackFrame = 0;
    std::vector<StormMetrics> playbackMetrics;
    std::vector<float> playbackThetaBase;
    DisplayFields playbackDisplay;

    ~Impl() { StopWorker(); }

    void StopWorker()
    {
        {
            std::lock_guard lock(mutex);
            exitRequested = true;
        }
        wake.notify_all();
        if (worker.joinable())
        {
            worker.join();
        }
        exitRequested = false;
    }

    // Caller holds the lock. Builds the solver, the frame schedule and the
    // recorder, and samples frame 0.
    std::string EnsureSolver(std::unique_lock<std::mutex>& lock)
    {
        if (initializing)
        {
            return "the run is being created; retry in a moment";
        }
        if (solver)
        {
            return {};
        }
        if (const std::string problem = FastStormSolver::Validate(settings.solver);
            !problem.empty())
        {
            return problem;
        }
        if (!(settings.targetMinutes > 0.0) || !(settings.frameIntervalSeconds > 0.0))
        {
            return "targetMinutes and frameIntervalSeconds must be positive";
        }
        const double target = settings.targetMinutes * 60.0;
        frameTimes.clear();
        for (double t = 0.0; t < target - 1.0e-6; t += settings.frameIntervalSeconds)
        {
            frameTimes.push_back(t);
        }
        frameTimes.push_back(target);
        solver = std::make_unique<FastStormSolver>(settings.solver);
        simTime = 0.0;
        steps = 0;
        wallSeconds = 0.0;
        liveMetrics.clear();
        nextFrame = 0;
        recording = false;
        if (!settings.recordPath.empty())
        {
            std::vector<float> times(frameTimes.begin(), frameTimes.end());
            std::string openError;
            if (!writer.Open(settings.recordPath, solver->MakeHeader(times), &openError))
            {
                solver.reset();
                return openError;
            }
            recording = true;
        }
        residentBytes = solver->ResidentBytes();
        initializing = true;
        lock.unlock();
        SampleFrame(true);
        lock.lock();
        nextFrame = 1; // frame 0 was just sampled
        initializing = false;
        return {};
    }

    // Copies the fields of the live solver and, when `frame` is set, records
    // metrics and the frame. Worker-side (or API-side while stopped); takes
    // the lock only to publish.
    void SampleFrame(const bool frame)
    {
        FastStormSolver& s = *solver;
        std::map<std::string, std::vector<float>> raw;
        for (const char* name : {"th", "qv", "qc", "qr", "u", "v", "w", "zvort"})
        {
            s.CopyField(name, raw[name]);
        }
        const WxHeader header = s.MakeHeader({});
        const double now = s.Diagnostics().time;
        DisplayFields display;
        BuildDisplay(header, raw, s.Base().theta, display);
        display.time = now;

        StormMetrics metrics;
        const bool first = frame && liveMetricsEmptyUnlocked();
        if (first)
        {
            baseThetaMean = LowestLevelThetaMean(header, raw["th"]);
        }
        if (frame)
        {
            StormFrameFields fields;
            fields.th = &raw["th"];
            fields.w = &raw["w"];
            fields.qr = &raw["qr"];
            fields.qc = &raw["qc"];
            fields.zvort = &raw["zvort"];
            metrics = ComputeFrameMetrics(
                header, static_cast<float>(now), fields, baseThetaMean);
            if (recording)
            {
                (void)s.WriteFrame(writer);
            }
        }
        std::lock_guard lock(mutex);
        liveDisplay = std::move(display);
        diagnostics = s.Diagnostics();
        if (frame)
        {
            liveMetrics.push_back(metrics);
        }
    }

    bool liveMetricsEmptyUnlocked()
    {
        std::lock_guard lock(mutex);
        return liveMetrics.empty();
    }

    void Work()
    {
        std::unique_lock lock(mutex);
        Clock::time_point lastPublish = Clock::now();
        for (;;)
        {
            wake.wait(lock, [this]
            {
                return exitRequested
                    || state == SessionState::Running || stepBudget > 0.0;
            });
            if (exitRequested)
            {
                return;
            }
            const double target = frameTimes.back();
            const double nextTime = nextFrame < frameTimes.size()
                ? frameTimes[nextFrame] : target;
            const bool running = state == SessionState::Running;
            const double budget = stepBudget;
            lock.unlock();

            bool failed = false;
            std::string failure;
            double advanced = 0.0;
            const auto t0 = Clock::now();
            try
            {
                const double time = solver->Diagnostics().time;
                const double toFrame = nextTime - time;
                double limit = std::min<double>(
                    settings.solver.maxTimeStep, std::max(toFrame, 1.0e-3));
                if (!running)
                {
                    limit = std::min(limit, std::max(budget, 1.0e-3));
                }
                const auto request = static_cast<float>(limit);
                advanced = static_cast<double>(solver->Step(request));
            }
            catch (const std::exception& e)
            {
                failed = true;
                failure = e.what();
            }
            const auto t1 = Clock::now();

            const double time = solver->Diagnostics().time;
            bool atFrame = false;
            bool atEnd = false;
            if (!failed && nextFrame < frameTimes.size()
                && time >= frameTimes[nextFrame] - 1.0e-3)
            {
                atFrame = true;
            }
            atEnd = !failed && time >= target - 1.0e-3;
            const bool publish = atFrame || atEnd
                || Seconds(lastPublish, t1) > 0.25;
            if (atFrame)
            {
                SampleFrame(true);
                ++nextFrame;
            }
            else if (publish)
            {
                SampleFrame(false);
            }
            if (publish)
            {
                lastPublish = Clock::now();
            }

            lock.lock();
            simTime = time;
            steps = solver->Diagnostics().steps;
            wallSeconds += Seconds(t0, t1);
            diagnostics = solver->Diagnostics();
            if (failed)
            {
                state = SessionState::Failed;
                error = failure;
                stepBudget = 0.0;
            }
            else if (atEnd)
            {
                state = SessionState::Finished;
                stepBudget = 0.0;
                if (recording)
                {
                    writer.Close();
                    recording = false;
                }
            }
            else if (!running)
            {
                stepBudget -= advanced;
                if (stepBudget < 1.0e-3)
                {
                    stepBudget = 0.0;
                }
            }
            // Speed limit: sleep until the wall clock has caught up.
            if (!failed && state == SessionState::Running
                && settings.speedLimit > 0.0)
            {
                const double allowedSim = runStartSim
                    + Seconds(runStartClock, Clock::now()) * settings.speedLimit;
                if (time > allowedSim)
                {
                    wake.wait_for(lock,
                        std::chrono::duration<double>(
                            (time - allowedSim) / settings.speedLimit),
                        [this]
                        {
                            return exitRequested
                                || state != SessionState::Running;
                        });
                }
            }
        }
    }

    void EnsureWorker()
    {
        if (!worker.joinable())
        {
            worker = std::thread([this] { Work(); });
        }
    }
};

WeatherLabSession::WeatherLabSession()
    : impl_(std::make_unique<Impl>())
{
}

WeatherLabSession::~WeatherLabSession() = default;

std::string WeatherLabSession::Configure(const WeatherLabSettings& settings)
{
    std::lock_guard lock(impl_->mutex);
    if (impl_->solver)
    {
        return "a live run exists; Reset before changing settings";
    }
    if (const std::string problem = FastStormSolver::Validate(settings.solver);
        !problem.empty())
    {
        return problem;
    }
    if (!(settings.targetMinutes > 0.0) || !(settings.frameIntervalSeconds > 0.0)
        || settings.speedLimit < 0.0)
    {
        return "targetMinutes and frameIntervalSeconds must be positive, speedLimit >= 0";
    }
    impl_->settings = settings;
    return {};
}

WeatherLabSettings WeatherLabSession::Settings() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->settings;
}

std::string WeatherLabSession::Start()
{
    std::unique_lock lock(impl_->mutex);
    if (impl_->state == SessionState::Finished)
    {
        return "the run reached its target time; Reset to start again";
    }
    if (impl_->state == SessionState::Failed)
    {
        return "the run failed (" + impl_->error + "); Reset first";
    }
    if (impl_->state == SessionState::Running)
    {
        return {};
    }
    if (const std::string problem = impl_->EnsureSolver(lock); !problem.empty())
    {
        return problem;
    }
    impl_->runStartSim = impl_->simTime;
    impl_->runStartClock = Clock::now();
    impl_->state = SessionState::Running;
    impl_->EnsureWorker();
    lock.unlock();
    impl_->wake.notify_all();
    return {};
}

void WeatherLabSession::Pause()
{
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->state == SessionState::Running)
        {
            impl_->state = SessionState::Paused;
        }
    }
    impl_->wake.notify_all();
}

void WeatherLabSession::Reset()
{
    impl_->StopWorker();
    std::lock_guard lock(impl_->mutex);
    impl_->writer.Close();
    impl_->recording = false;
    impl_->solver.reset();
    impl_->state = SessionState::Idle;
    impl_->error.clear();
    impl_->stepBudget = 0.0;
    impl_->simTime = 0.0;
    impl_->steps = 0;
    impl_->wallSeconds = 0.0;
    impl_->diagnostics = {};
    impl_->residentBytes = 0;
    impl_->liveMetrics.clear();
    impl_->liveDisplay = {};
    impl_->nextFrame = 0;
}

std::string WeatherLabSession::Step(const double seconds)
{
    if (!(seconds > 0.0))
    {
        return "seconds must be positive";
    }
    std::unique_lock lock(impl_->mutex);
    if (impl_->state == SessionState::Finished || impl_->state == SessionState::Failed)
    {
        return "the run is " + std::string(SessionStateName(impl_->state)) + "; Reset first";
    }
    if (impl_->state == SessionState::Running)
    {
        return "pause before stepping";
    }
    if (const std::string problem = impl_->EnsureSolver(lock); !problem.empty())
    {
        return problem;
    }
    impl_->state = SessionState::Paused;
    impl_->stepBudget += seconds;
    impl_->EnsureWorker();
    lock.unlock();
    impl_->wake.notify_all();
    return {};
}

void WeatherLabSession::SetSpeedLimit(const double limit)
{
    {
        std::lock_guard lock(impl_->mutex);
        impl_->settings.speedLimit = std::max(limit, 0.0);
        impl_->runStartSim = impl_->simTime;
        impl_->runStartClock = Clock::now();
    }
    impl_->wake.notify_all();
}

WeatherLabStatus WeatherLabSession::Status() const
{
    std::lock_guard lock(impl_->mutex);
    WeatherLabStatus s;
    s.state = impl_->state;
    s.error = impl_->error;
    s.simTime = impl_->simTime;
    s.targetSeconds = impl_->settings.targetMinutes * 60.0;
    s.steps = impl_->steps;
    s.wallSeconds = impl_->wallSeconds;
    s.realTimeRatio = impl_->wallSeconds > 0.0
        ? impl_->simTime / impl_->wallSeconds : 0.0;
    s.diagnostics = impl_->diagnostics;
    s.residentBytes = impl_->residentBytes;
    s.liveFrames = impl_->liveMetrics.size();
    s.recording = impl_->recording;
    s.hasPlayback = impl_->hasPlayback;
    s.playbackPath = impl_->playbackPath;
    if (impl_->hasPlayback)
    {
        s.playbackSource = impl_->reader.Header().source;
        s.playbackFrames = impl_->reader.FrameCount();
        s.playbackFrame = impl_->playbackFrame;
        s.playbackTime = impl_->playbackDisplay.time;
    }
    return s;
}

std::vector<StormMetrics> WeatherLabSession::LiveMetrics() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->liveMetrics;
}

std::string WeatherLabSession::LoadPlayback(const std::filesystem::path& path)
{
    WxReader reader;
    std::string error;
    if (!reader.Open(path, &error))
    {
        return error;
    }
    std::vector<StormMetrics> metrics;
    if (!ComputeStormMetrics(reader, metrics, &error))
    {
        return error;
    }
    if (reader.FrameCount() == 0U)
    {
        return "the file holds no complete frames";
    }
    // Per-level reference theta from the first frame.
    const WxHeader& h = reader.Header();
    std::vector<float> th;
    if (!reader.ReadField(0, "th", th, &error))
    {
        return error;
    }
    std::vector<float> base(h.nz, 0.0F);
    const std::size_t plane = static_cast<std::size_t>(h.nx) * h.ny;
    for (std::uint32_t k = 0; k < h.nz; ++k)
    {
        double sum = 0.0;
        for (std::size_t c = 0; c < plane; ++c)
        {
            sum += th[static_cast<std::size_t>(k) * plane + c];
        }
        base[k] = static_cast<float>(sum / static_cast<double>(plane));
    }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->reader = std::move(reader);
        impl_->playbackMetrics = std::move(metrics);
        impl_->playbackThetaBase = std::move(base);
        impl_->playbackPath = path.string();
        impl_->hasPlayback = true;
        impl_->playbackFrame = 0;
    }
    return SelectPlaybackFrame(0);
}

void WeatherLabSession::ClearPlayback()
{
    std::lock_guard lock(impl_->mutex);
    impl_->hasPlayback = false;
    impl_->playbackMetrics.clear();
    impl_->playbackDisplay = {};
    impl_->playbackPath.clear();
}

std::string WeatherLabSession::SelectPlaybackFrame(const std::size_t frame)
{
    std::lock_guard lock(impl_->mutex);
    if (!impl_->hasPlayback)
    {
        return "no playback loaded";
    }
    if (frame >= impl_->reader.FrameCount())
    {
        return "frame out of range (0.." + std::to_string(impl_->reader.FrameCount() - 1U) + ")";
    }
    std::map<std::string, std::vector<float>> raw;
    std::string error;
    for (const char* name : {"th", "w", "qr", "qc", "qv", "qi", "qs", "qg", "u", "v", "zvort"})
    {
        if (impl_->reader.Header().FieldIndex(name) < 0)
        {
            continue;
        }
        if (!impl_->reader.ReadField(frame, name, raw[name], &error))
        {
            return error;
        }
    }
    BuildDisplay(impl_->reader.Header(), raw, impl_->playbackThetaBase,
        impl_->playbackDisplay);
    impl_->playbackDisplay.time = impl_->reader.Header().times[frame];
    impl_->playbackFrame = frame;
    return {};
}

std::vector<StormMetrics> WeatherLabSession::PlaybackMetrics() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->playbackMetrics;
}

std::vector<ComparisonRow> WeatherLabSession::Compare() const
{
    std::lock_guard lock(impl_->mutex);
    std::vector<ComparisonRow> rows;
    for (const StormMetrics& reference : impl_->playbackMetrics)
    {
        for (const StormMetrics& live : impl_->liveMetrics)
        {
            if (std::fabs(live.time - reference.time) < 1.0F)
            {
                rows.push_back({reference.time, reference, live});
                break;
            }
        }
    }
    return rows;
}

Slice WeatherLabSession::GetSlice(const SliceRequest& request) const
{
    std::lock_guard lock(impl_->mutex);
    return ExtractSlice(
        request.source == DisplaySource::Live
            ? impl_->liveDisplay : impl_->playbackDisplay,
        request);
}
} // namespace orbit::weather_lab
