#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_ui/ImplementationPlan.hpp>

#include <string>

namespace orbit::studio_ui
{
class PlanningUi
{
public:
    explicit PlanningUi(ImplementationPlan& plan) noexcept;
    void Register(editor_ui::EditorUi& ui);
    void RegisterRpc(rpc::Dispatcher& dispatcher);

    inline static constexpr editor_ui::PanelId kPanelId{
        .high = 0x4f52424954504c4eULL,
        .low = 0x494d504c504c414eULL
    };

private:
    void Draw(editor_ui::PanelContext& context);
    void DrawCanvas(editor_ui::PanelContext& context);
    void LoadSelection();

    ImplementationPlan* plan_{nullptr};
    editor_ui::EditorUi* ui_{nullptr};
    u64 selected_{0};
    u64 draggingBubble_{0};
    f64 dragX_{0.5};
    f64 dragY_{0.5};
    bool dragPositionDraft_{false};
    u64 editRevision_{0};
    std::string title_;
    std::string description_;
    std::string newTitle_;
    std::string status_;
    bool statusError_{false};
    bool dirty_{false};
};
} // namespace orbit::studio_ui
