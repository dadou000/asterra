#include <orbit/studio_session/BezierHandleEditor.hpp>

#include <orbit/paths/PathNetwork.hpp>

#include <stdexcept>

namespace orbit::studio_session
{
BezierHandleEditor::BezierHandleEditor(
    editor_session::EditorWorldSession& world,
    WorldBoundPathNetwork& paths) noexcept
    : world_(&world),
      paths_(&paths)
{
}

BezierHandleEditor::~BezierHandleEditor()
{
    Cancel();
}

void BezierHandleEditor::Begin(
    const scene::ObjectId edge,
    const BezierHandle handle)
{
    if (world_ == nullptr ||
        paths_ == nullptr ||
        !world_->HasWorld())
    {
        throw std::logic_error(
            "Cannot edit Bezier handles without an open authoring world.");
    }

    if (Active())
    {
        throw std::logic_error(
            "A Bezier handle edit is already active.");
    }

    auto& commands = world_->Commands();

    if (commands.HasActiveTransaction())
    {
        throw std::logic_error(
            "Cannot start a Bezier gizmo edit inside another transaction.");
    }

    const auto record =
        paths_->Service().FindEdge(edge);

    if (!record.has_value())
    {
        throw std::invalid_argument(
            "Bezier gizmo edge was not found.");
    }

    if (record->mode != paths::EdgeMode::Bezier)
    {
        throw std::invalid_argument(
            "Bezier gizmo requires a Bezier path edge.");
    }

    commands.BeginTransaction(
        "Edit Bezier Handles");

    edge_ = edge;
    handle_ = handle;
    startHandle_ = record->startHandleMeters;
    endHandle_ = record->endHandleMeters;
    worldGeneration_ = world_->Generation();
    ownsTransaction_ = true;
}

void BezierHandleEditor::Update(
    const math::Double3 handleMeters)
{
    if (!Active() ||
        world_ == nullptr ||
        paths_ == nullptr)
    {
        throw std::logic_error(
            "No Bezier handle edit is active.");
    }

    if (world_->Generation() != worldGeneration_)
    {
        throw std::logic_error(
            "Bezier handle edit belongs to a stale authoring world.");
    }

    if (*handle_ == BezierHandle::Start)
    {
        startHandle_ = handleMeters;
    }
    else
    {
        endHandle_ = handleMeters;
    }

    paths_->Service().SetBezierHandles(
        *edge_,
        startHandle_,
        endHandle_);
}

void BezierHandleEditor::Commit()
{
    if (!Active() ||
        world_ == nullptr)
    {
        throw std::logic_error(
            "No Bezier handle edit is active.");
    }

    if (world_->Generation() != worldGeneration_)
    {
        throw std::logic_error(
            "Bezier handle edit belongs to a stale authoring world.");
    }

    if (!ownsTransaction_ ||
        !world_->Commands().HasActiveTransaction())
    {
        throw std::logic_error(
            "Bezier handle transaction is no longer active.");
    }

    world_->Commands().CommitTransaction();
    Clear();
}

void BezierHandleEditor::Cancel() noexcept
{
    if (ownsTransaction_ &&
        world_ != nullptr &&
        world_->HasWorld() &&
        world_->Generation() == worldGeneration_ &&
        world_->Commands().HasActiveTransaction())
    {
        try
        {
            world_->Commands().RollbackTransaction();
        }
        catch (...)
        {
            // Destruction/cancel must never throw. World lifecycle prevents
            // switching while this transaction is active, so this is only a
            // final defensive guard against an already-failed command path.
        }
    }

    Clear();
}

bool BezierHandleEditor::Active() const noexcept
{
    return edge_.has_value() &&
        handle_.has_value() &&
        ownsTransaction_;
}

std::optional<scene::ObjectId>
BezierHandleEditor::Edge() const noexcept
{
    return edge_;
}

std::optional<BezierHandle>
BezierHandleEditor::Handle() const noexcept
{
    return handle_;
}

math::Double3 BezierHandleEditor::StartHandle() const noexcept
{
    return startHandle_;
}

math::Double3 BezierHandleEditor::EndHandle() const noexcept
{
    return endHandle_;
}

void BezierHandleEditor::Clear() noexcept
{
    edge_.reset();
    handle_.reset();
    startHandle_ = {};
    endHandle_ = {};
    worldGeneration_ = 0;
    ownsTransaction_ = false;
}
} // namespace orbit::studio_session
