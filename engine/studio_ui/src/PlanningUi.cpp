#include <orbit/studio_ui/PlanningUi.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <stdexcept>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;

[[nodiscard]] const Value& ObjectParams(const Value& params)
{
    if (!params.IsObject()) throw rpc::Error(-32602, "params must be an object");
    return params;
}

[[nodiscard]] u64 RequiredId(const Value& params)
{
    const Value* id = params.Find("id");
    if (id == nullptr || !id->IsInteger() || id->AsInteger() <= 0)
        throw rpc::Error(-32602, "id must be a positive integer");
    return static_cast<u64>(id->AsInteger());
}

[[nodiscard]] std::string RequiredString(const Value& params, const char* key)
{
    const Value* value = params.Find(key);
    if (value == nullptr || !value->IsString())
        throw rpc::Error(-32602, std::string(key) + " must be a string");
    return value->AsString();
}

[[nodiscard]] Value BubbleValue(const PlanBubble& bubble)
{
    Value::Object result{
        {"id", static_cast<i64>(bubble.id)},
        {"title", bubble.title},
        {"description", bubble.description},
        {"status", std::string(PlanStatusName(bubble.status))},
        {"x", bubble.x},
        {"y", bubble.y}};
    result.emplace("after", bubble.after.has_value()
        ? Value(static_cast<i64>(*bubble.after))
        : Value(nullptr));
    return result;
}

constexpr std::array<std::string_view, 4> kStatusLabels{
    "Idea", "Ready", "In progress", "Done"};

[[nodiscard]] math::Float4 BubbleColor(const PlanStatus status)
{
    switch (status)
    {
    case PlanStatus::Idea: return {0.28F, 0.40F, 0.60F, 1.0F};
    case PlanStatus::Ready: return {0.22F, 0.54F, 0.68F, 1.0F};
    case PlanStatus::InProgress: return {0.78F, 0.48F, 0.18F, 1.0F};
    case PlanStatus::Done: return {0.22F, 0.58F, 0.40F, 1.0F};
    }
    return {0.28F, 0.40F, 0.60F, 1.0F};
}
} // namespace

PlanningUi::PlanningUi(ImplementationPlan& plan) noexcept : plan_(&plan) {}

void PlanningUi::Register(editor_ui::EditorUi& ui)
{
    ui_ = &ui;
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Planning",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = -20,
        .minSize = {620.0F, 400.0F},
        .defaultSize = {1050.0F, 700.0F},
        .draw = [this](editor_ui::PanelContext& context) { Draw(context); }
    });
}

void PlanningUi::RegisterRpc(rpc::Dispatcher& dispatcher)
{
    dispatcher.Register({
        .name = "planning.list",
        .description = "List the implementation-plan bubbles and their schedule links.",
        .mutating = false
    }, [this](const Value&)
    {
        Value::Array bubbles;
        for (const PlanBubble& bubble : plan_->Bubbles()) bubbles.push_back(BubbleValue(bubble));
        return Value::Object{{"bubbles", std::move(bubbles)}, {"revision", static_cast<i64>(plan_->Revision())}};
    });

    dispatcher.Register({
        .name = "planning.create",
        .description = "Add a floating idea bubble to the implementation plan.",
        .mutating = true
    }, [this](const Value& input)
    {
        const Value& params = ObjectParams(input);
        std::string description;
        if (const Value* value = params.Find("description"); value != nullptr)
        {
            if (!value->IsString()) throw rpc::Error(-32602, "description must be a string");
            description = value->AsString();
        }
        const u64 id = plan_->Create(RequiredString(params, "title"), std::move(description));
        return BubbleValue(*plan_->Find(id));
    });

    dispatcher.Register({
        .name = "planning.update",
        .description = "Edit a planning bubble, move it on the canvas, or set/clear the bubble it follows.",
        .mutating = true
    }, [this](const Value& input)
    {
        const Value& params = ObjectParams(input);
        PlanBubblePatch patch;
        if (const Value* value = params.Find("title"); value != nullptr)
        {
            if (!value->IsString()) throw rpc::Error(-32602, "title must be a string");
            patch.title = value->AsString();
        }
        if (const Value* value = params.Find("description"); value != nullptr)
        {
            if (!value->IsString()) throw rpc::Error(-32602, "description must be a string");
            patch.description = value->AsString();
        }
        if (const Value* value = params.Find("status"); value != nullptr)
        {
            if (!value->IsString()) throw rpc::Error(-32602, "status must be a string");
            patch.status = ParsePlanStatus(value->AsString());
            if (!patch.status.has_value()) throw rpc::Error(-32602, "status must be idea, ready, in_progress or done");
        }
        if (const Value* value = params.Find("after"); value != nullptr)
        {
            if (value->IsNull()) patch.after = std::optional<u64>{};
            else if (value->IsInteger() && value->AsInteger() > 0) patch.after = static_cast<u64>(value->AsInteger());
            else throw rpc::Error(-32602, "after must be a positive bubble id or null");
        }
        for (const auto [key, target] : {std::pair{"x", &patch.x}, std::pair{"y", &patch.y}})
        {
            if (const Value* value = params.Find(key); value != nullptr)
            {
                if (!value->IsNumber()) throw rpc::Error(-32602, std::string(key) + " must be a number");
                *target = value->AsNumber();
            }
        }
        const u64 id = RequiredId(params);
        plan_->Update(id, patch);
        return BubbleValue(*plan_->Find(id));
    });

    dispatcher.Register({
        .name = "planning.delete",
        .description = "Delete a planning bubble and release its dependents as floating ideas.",
        .mutating = true
    }, [this](const Value& input)
    {
        const u64 id = RequiredId(ObjectParams(input));
        if (!plan_->Remove(id)) throw rpc::Error(-32004, "Plan bubble not found");
        return Value::Object{{"deleted", static_cast<i64>(id)}};
    });
}

void PlanningUi::LoadSelection()
{
    const PlanBubble* bubble = plan_->Find(selected_);
    editRevision_ = plan_->Revision();
    if (bubble == nullptr)
    {
        selected_ = 0;
        title_.clear();
        description_.clear();
        return;
    }
    title_ = bubble->title;
    description_ = bubble->description;
}

void PlanningUi::Draw(editor_ui::PanelContext& context)
{
    if (plan_ == nullptr) return;
    context.Heading("Implementation plan");
    context.MutedText("Capture ideas as floating bubbles, then connect each step to the work it follows.");
    context.SameLine();
    if (context.Button("+ New idea##plan-new"))
    {
        selected_ = plan_->Create("Untitled idea");
        LoadSelection();
    }
    if (!plan_->LastSaveError().empty()) context.ErrorText("Plan save failed: " + plan_->LastSaveError());
    DrawCanvas(context);

    if (selected_ == 0U) return;
    const PlanBubble* bubble = plan_->Find(selected_);
    if (bubble == nullptr) { LoadSelection(); return; }
    if (editRevision_ != plan_->Revision()) LoadSelection();
    context.Separator();
    context.Heading(std::format("Edit bubble #{}", selected_));
    static_cast<void>(context.InputText("Title", title_));
    static_cast<void>(context.InputTextMultiline("Description", description_, {0.0F, 90.0F}));
    i32 status = static_cast<i32>(bubble->status);
    if (context.Combo("Status", kStatusLabels, status))
    {
        PlanBubblePatch patch;
        patch.status = static_cast<PlanStatus>(std::clamp(status, 0, 3));
        try { plan_->Update(selected_, patch); editRevision_ = plan_->Revision(); }
        catch (const std::exception& error) { status_ = error.what(); statusError_ = true; }
    }
    std::vector<std::string> labels{"Floating idea"};
    std::vector<std::string_view> options;
    labels.reserve(plan_->Bubbles().size() + 1U);
    for (const PlanBubble& item : plan_->Bubbles())
        if (item.id != selected_) labels.push_back(std::format("After: {}##{}", item.title, item.id));
    for (const std::string& label : labels) options.push_back(label);
    i32 after = 0;
    if (bubble->after.has_value())
    {
        std::size_t option = 1U;
        for (const PlanBubble& item : plan_->Bubbles())
        {
            if (item.id == selected_) continue;
            if (item.id == *bubble->after) { after = static_cast<i32>(option); break; }
            ++option;
        }
    }
    if (context.Combo("Schedule after", options, after))
    {
        PlanBubblePatch patch;
        if (after == 0) patch.after = std::optional<u64>{};
        else
        {
            std::size_t option = 1U;
            for (const PlanBubble& item : plan_->Bubbles())
            {
                if (item.id == selected_) continue;
                if (option++ == static_cast<std::size_t>(after)) { patch.after = item.id; break; }
            }
        }
        try { plan_->Update(selected_, patch); editRevision_ = plan_->Revision(); }
        catch (const std::exception& error) { status_ = error.what(); statusError_ = true; }
    }
    if (context.Button("Save details##plan-save"))
    {
        try
        {
            PlanBubblePatch patch;
            patch.title = title_;
            patch.description = description_;
            plan_->Update(selected_, patch);
            editRevision_ = plan_->Revision();
            status_ = "Plan updated.";
            statusError_ = false;
        }
        catch (const std::exception& error) { status_ = error.what(); statusError_ = true; }
    }
    context.SameLine();
    if (context.Button("Delete bubble##plan-delete"))
    {
        static_cast<void>(plan_->Remove(selected_));
        selected_ = 0;
        LoadSelection();
    }
    if (!status_.empty())
    {
        context.SameLine();
        if (statusError_) context.ErrorText(status_); else context.MutedText(status_);
    }
}

void PlanningUi::DrawCanvas(editor_ui::PanelContext& context)
{
    constexpr f32 kNodeRadius = 33.0F;
    const editor_ui::UiSize available = context.ContentAvailable();
    const editor_ui::UiSize canvasSize{
        std::max(available.width, 100.0F),
        std::max(available.height * 0.70F, 260.0F)};
    const auto pointer = context.Canvas("implementation-plan-canvas", canvasSize);
    for (const PlanBubble& bubble : plan_->Bubbles())
    {
        if (!bubble.after.has_value()) continue;
        const PlanBubble* predecessor = plan_->Find(*bubble.after);
        if (predecessor != nullptr)
        {
            const math::Float2 from = draggingBubble_ == predecessor->id && dragPositionDraft_
                ? math::Float2{static_cast<f32>(dragX_), static_cast<f32>(dragY_)}
                : math::Float2{static_cast<f32>(predecessor->x), static_cast<f32>(predecessor->y)};
            const math::Float2 to = draggingBubble_ == bubble.id && dragPositionDraft_
                ? math::Float2{static_cast<f32>(dragX_), static_cast<f32>(dragY_)}
                : math::Float2{static_cast<f32>(bubble.x), static_cast<f32>(bubble.y)};
            context.CanvasLine(
                from,
                to,
                {0.55F, 0.70F, 0.90F, 0.85F}, 2.0F);
        }
    }
    for (const PlanBubble& bubble : plan_->Bubbles())
    {
        const math::Float2 center = draggingBubble_ == bubble.id && dragPositionDraft_
            ? math::Float2{static_cast<f32>(dragX_), static_cast<f32>(dragY_)}
            : math::Float2{static_cast<f32>(bubble.x), static_cast<f32>(bubble.y)};
        const bool selected = bubble.id == selected_;
        context.CanvasCircle(center, selected ? kNodeRadius + 4.0F : kNodeRadius + 1.0F,
            selected ? math::Float4{0.88F, 0.91F, 1.0F, 1.0F} : math::Float4{0.28F, 0.32F, 0.39F, 1.0F}, false, selected ? 2.0F : 1.0F);
        context.CanvasCircle(center, kNodeRadius, BubbleColor(bubble.status), true);
        context.CanvasText({center.x - 0.024F, center.y - 0.01F}, {1.0F, 1.0F, 1.0F, 1.0F}, std::format("{}", bubble.id));
        context.CanvasText({center.x + 0.025F, center.y - 0.01F}, {0.93F, 0.95F, 0.98F, 1.0F}, bubble.title.substr(0U, 24U));
    }
    if (pointer.clicked)
    {
        u64 hit = 0;
        f64 nearest = 1.0;
        for (const PlanBubble& bubble : plan_->Bubbles())
        {
            const f64 dx = (pointer.u - bubble.x) * canvasSize.width;
            const f64 dy = (pointer.v - bubble.y) * canvasSize.height;
            const f64 distance = dx * dx + dy * dy;
            if (distance < static_cast<f64>(kNodeRadius * kNodeRadius) && distance < nearest * nearest)
            {
                hit = bubble.id;
                nearest = std::sqrt(distance);
            }
        }
        if (hit != 0U)
        {
            selected_ = hit;
            draggingBubble_ = hit;
            if (const PlanBubble* bubble = plan_->Find(hit); bubble != nullptr)
            {
                dragX_ = bubble->x;
                dragY_ = bubble->y;
                dragPositionDraft_ = true;
            }
            LoadSelection();
        }
        else
        {
            draggingBubble_ = 0U;
            dragPositionDraft_ = false;
        }
    }
    if (pointer.dragging && draggingBubble_ != 0U)
    {
        const PlanBubble* bubble = plan_->Find(draggingBubble_);
        if (bubble != nullptr)
        {
            static_cast<void>(bubble);
            dragX_ = std::clamp(static_cast<f64>(pointer.u), 0.03, 0.97);
            dragY_ = std::clamp(static_cast<f64>(pointer.v), 0.06, 0.94);
        }
    }
    if (pointer.leftReleased)
    {
        if (draggingBubble_ != 0U && dragPositionDraft_)
        {
            try
            {
                PlanBubblePatch patch;
                patch.x = dragX_;
                patch.y = dragY_;
                plan_->Update(draggingBubble_, patch);
                editRevision_ = plan_->Revision();
            }
            catch (const std::exception& error) { status_ = error.what(); statusError_ = true; }
        }
        draggingBubble_ = 0U;
        dragPositionDraft_ = false;
    }
}
} // namespace orbit::studio_ui
