#include <orbit/studio_session/StudioWorkspace.hpp>

#include <stdexcept>
#include <utility>

namespace orbit::studio_session
{
struct StudioWorkspace::State
{
    explicit State(documents::ProjectDocument document)
        : project(std::move(document)),
          session(project)
    {
    }

    documents::ProjectDocument project;
    StudioSession session;
};

StudioWorkspace::~StudioWorkspace()
{
    try
    {
        CheckpointCurrent();
    }
    catch (...)
    {
        // Destructors must not throw. Explicit CloseProject surfaces errors.
    }
}

StudioWorkspace::StudioWorkspace(
    StudioWorkspace&&) noexcept = default;
StudioWorkspace& StudioWorkspace::operator=(
    StudioWorkspace&&) noexcept = default;

StudioWorkspace::StudioWorkspace(
    std::unique_ptr<State> state) noexcept
    : state_(std::move(state))
{
}

void StudioWorkspace::CreateProject(
    const std::filesystem::path& rootDirectory,
    const std::string_view displayName)
{
    auto candidate =
        std::make_unique<State>(
            documents::ProjectDocument::Create(
                rootDirectory,
                displayName));
    CommitCandidate(std::move(candidate));
}

void StudioWorkspace::OpenProject(
    const std::filesystem::path& path)
{
    std::filesystem::path manifestPath = path;

    if (std::filesystem::is_directory(manifestPath))
    {
        manifestPath /= "Project.orbit.toml";
    }

    auto candidate =
        std::make_unique<State>(
            documents::ProjectDocument::Open(
                manifestPath));
    CommitCandidate(std::move(candidate));
}

void StudioWorkspace::CloseProject()
{
    if (state_ == nullptr)
    {
        return;
    }

    CheckpointCurrent();
    state_.reset();
    ++generation_;
}

bool StudioWorkspace::HasProject() const noexcept
{
    return state_ != nullptr;
}

u64 StudioWorkspace::Generation() const noexcept
{
    return generation_;
}

documents::ProjectDocument& StudioWorkspace::Project()
{
    return RequireState().project;
}

const documents::ProjectDocument&
StudioWorkspace::Project() const
{
    return RequireState().project;
}

StudioSession& StudioWorkspace::Session()
{
    return RequireState().session;
}

const StudioSession& StudioWorkspace::Session() const
{
    return RequireState().session;
}

void StudioWorkspace::CommitCandidate(
    std::unique_ptr<State> candidate)
{
    if (candidate == nullptr)
    {
        throw std::invalid_argument(
            "Studio workspace candidate must not be null.");
    }

    CheckpointCurrent();
    state_ = std::move(candidate);
    ++generation_;
}

void StudioWorkspace::CheckpointCurrent()
{
    if (state_ == nullptr)
    {
        return;
    }

    state_->project.Save();

    if (state_->session.World().HasWorld())
    {
        state_->session.World().Checkpoint();
    }
}

StudioWorkspace::State& StudioWorkspace::RequireState()
{
    if (state_ == nullptr)
    {
        throw std::logic_error(
            "Orbit Studio has no open project.");
    }

    return *state_;
}

const StudioWorkspace::State&
StudioWorkspace::RequireState() const
{
    if (state_ == nullptr)
    {
        throw std::logic_error(
            "Orbit Studio has no open project.");
    }

    return *state_;
}
} // namespace orbit::studio_session
