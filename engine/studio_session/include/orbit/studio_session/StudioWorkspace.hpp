#pragma once

#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/studio_session/StudioSession.hpp>

#include <filesystem>
#include <memory>
#include <string_view>

namespace orbit::studio_session
{
// Top-level Orbit Studio ownership boundary. Project switches are transactional:
// a complete candidate ProjectDocument + StudioSession is constructed before
// the previous project is checkpointed and replaced.
class StudioWorkspace
{
public:
    StudioWorkspace() = default;
    ~StudioWorkspace();

    StudioWorkspace(const StudioWorkspace&) = delete;
    StudioWorkspace& operator=(const StudioWorkspace&) = delete;

    StudioWorkspace(StudioWorkspace&&) noexcept;
    StudioWorkspace& operator=(StudioWorkspace&&) noexcept;

    void CreateProject(
        const std::filesystem::path& rootDirectory,
        std::string_view displayName);

    // Accepts either a project directory or Project.orbit.toml path.
    void OpenProject(
        const std::filesystem::path& path);

    void CloseProject();

    [[nodiscard]] bool HasProject() const noexcept;
    [[nodiscard]] u64 Generation() const noexcept;

    [[nodiscard]] documents::ProjectDocument& Project();
    [[nodiscard]] const documents::ProjectDocument& Project() const;

    [[nodiscard]] StudioSession& Session();
    [[nodiscard]] const StudioSession& Session() const;

private:
    struct State;

    explicit StudioWorkspace(
        std::unique_ptr<State> state) noexcept;

    void CommitCandidate(std::unique_ptr<State> candidate);
    void CheckpointCurrent();

    [[nodiscard]] State& RequireState();
    [[nodiscard]] const State& RequireState() const;

    std::unique_ptr<State> state_;
    u64 generation_{0};
};
} // namespace orbit::studio_session
