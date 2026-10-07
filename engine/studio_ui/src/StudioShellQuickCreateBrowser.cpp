#include <orbit/studio_ui/StudioExpansionShell.hpp>

#include <orbit/content/ContentService.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/editor_ui/FocusState.hpp>
#include <orbit/editor_ui/PanelExtensions.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/studio_ui/CommandPaletteModel.hpp>
#include <orbit/studio_ui/SelectionBreadcrumbs.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/terrain_debug/TerrainDebugSeam.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "StudioShellInternals.hpp"

namespace orbit::studio_ui
{
using namespace shell_detail;

void StudioExpansionShell::DrawQuickCreateBrowser(editor_ui::PanelContext& context, bool openQuickCreateBrowser,
        const std::vector<commands::CommandCatalogEntry>& commandCatalog,
        commands::CommandRegistry& registry,
        const std::vector<CommandPaletteEntry>& palette)
{
    auto& world = owner_->session_->World();
    const f32 quickCreateBrowserWidth =
        520.0F * editor_ui::CurrentUiScale();
    if (context.BeginPopup(
            "studio-quick-create-browser-popup",
            openQuickCreateBrowser,
            {.width = quickCreateBrowserWidth, .height = 0.0F}))
    {
        context.Text("Add · Browse All");
        if (openQuickCreateBrowser)
        {
            context.FocusNextItem();
        }
        if (context.InputText(
                "##studio-quick-create-browser-query",
                quickCreateBrowseQuery_))
        {
            quickCreateBrowseSelection_ = 0;
        }
        context.Separator();

        const auto clearArgumentForm = [this]
        {
            quickCreateArgumentCommand_ = {};
            quickCreateArguments_.clear();
            quickCreateArgumentEnabled_.clear();
            quickCreateIdText_.clear();
            quickCreatePickerQuery_.clear();
            quickCreatePickerSelection_.clear();
            quickCreateArgumentError_.clear();
        };

        const auto beginArgumentForm =
            [this, &registry, &clearArgumentForm](
                const commands::CommandId command)
            {
                clearArgumentForm();
                const auto* descriptor = registry.Find(command);
                if (descriptor == nullptr)
                {
                    quickCreateArgumentError_ =
                        "Command metadata is no longer available.";
                    return;
                }

                quickCreateArgumentCommand_ = command;
                for (const auto& parameter : descriptor->parameters)
                {
                    quickCreateArgumentEnabled_[parameter.name] =
                        parameter.required;

                    if (parameter.defaultValue.has_value())
                    {
                        bool compatible = false;
                        switch (parameter.kind)
                        {
                        case commands::CommandValueKind::Boolean:
                            compatible = std::holds_alternative<bool>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::Integer:
                            compatible = std::holds_alternative<i64>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::Float:
                            compatible = std::holds_alternative<f64>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::String:
                            compatible = std::holds_alternative<std::string>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::Vector3:
                            compatible = std::holds_alternative<math::Double3>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::ObjectId:
                            compatible = std::holds_alternative<scene::ObjectId>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::PropertyId:
                            compatible = std::holds_alternative<schema::PropertyId>(*parameter.defaultValue);
                            break;
                        }

                        if (compatible)
                        {
                            quickCreateArguments_[parameter.name] =
                                *parameter.defaultValue;
                            if (const auto* object =
                                    std::get_if<scene::ObjectId>(&*parameter.defaultValue))
                            {
                                quickCreateIdText_[parameter.name] =
                                    object->IsValid() ? object->ToString() : std::string{};
                            }
                            else if (const auto* property =
                                         std::get_if<schema::PropertyId>(&*parameter.defaultValue))
                            {
                                quickCreateIdText_[parameter.name] =
                                    property->IsValid() ? property->ToString() : std::string{};
                            }
                            continue;
                        }
                    }

                    if (!parameter.choices.empty())
                    {
                        quickCreateArguments_[parameter.name] =
                            parameter.choices.front().value;
                        continue;
                    }

                    switch (parameter.kind)
                    {
                    case commands::CommandValueKind::Boolean:
                        quickCreateArguments_[parameter.name] = false;
                        break;
                    case commands::CommandValueKind::Integer:
                        quickCreateArguments_[parameter.name] = i64{0};
                        break;
                    case commands::CommandValueKind::Float:
                        quickCreateArguments_[parameter.name] = f64{0.0};
                        break;
                    case commands::CommandValueKind::String:
                        quickCreateArguments_[parameter.name] = std::string{};
                        break;
                    case commands::CommandValueKind::Vector3:
                        quickCreateArguments_[parameter.name] = math::Double3{};
                        break;
                    case commands::CommandValueKind::ObjectId:
                        quickCreateArguments_[parameter.name] = scene::ObjectId{};
                        quickCreateIdText_[parameter.name] = {};
                        break;
                    case commands::CommandValueKind::PropertyId:
                        quickCreateArguments_[parameter.name] = schema::PropertyId{};
                        quickCreateIdText_[parameter.name] = {};
                        break;
                    }
                }
            };

        if (quickCreateArgumentCommand_.IsValid())
        {
            const auto* descriptor =
                registry.Find(quickCreateArgumentCommand_);
            if (descriptor == nullptr)
            {
                context.ErrorText(
                    "Command argument form failed: command metadata disappeared.");
                if (context.Button("Back##quick-create-argument-back-missing"))
                {
                    clearArgumentForm();
                }
            }
            else
            {
                if (context.Button("Back##quick-create-argument-back"))
                {
                    clearArgumentForm();
                }
                else
                {
                    context.SameLine();
                    context.Heading(descriptor->name);
                    if (!descriptor->description.empty())
                    {
                        context.MutedText(descriptor->description);
                    }

                    std::vector<const commands::CommandParameter*> formParameters;
                    formParameters.reserve(descriptor->parameters.size());
                    for (const auto& parameter : descriptor->parameters)
                    {
                        if (parameter.required)
                        {
                            formParameters.push_back(&parameter);
                        }
                    }
                    for (const auto& parameter : descriptor->parameters)
                    {
                        if (!parameter.required)
                        {
                            formParameters.push_back(&parameter);
                        }
                    }

                    const bool hasRequiredParameters = std::ranges::any_of(
                        descriptor->parameters,
                        [](const commands::CommandParameter& parameter)
                        {
                            return parameter.required;
                        });
                    if (hasRequiredParameters)
                    {
                        context.MutedText("Required inputs");
                    }

                    bool optionalSectionDrawn = false;
                    bool optionalSectionOpen = false;
                    for (const auto* parameterPointer : formParameters)
                    {
                        const auto& parameter = *parameterPointer;
                        const std::string parameterDisplayName =
                            parameter.displayName.empty()
                                ? parameter.name
                                : parameter.displayName;

                        if (!parameter.required && !optionalSectionDrawn)
                        {
                            if (hasRequiredParameters)
                            {
                                context.Separator();
                            }
                            optionalSectionOpen = context.Section(
                                "Optional inputs##quick-create-optional-inputs",
                                false);
                            optionalSectionDrawn = true;
                        }
                        if (!parameter.required && !optionalSectionOpen)
                        {
                            continue;
                        }

                        bool enabled = parameter.required ||
                            quickCreateArgumentEnabled_[parameter.name];
                        if (!parameter.required)
                        {
                            std::string optionalLabel =
                                "Set " + parameterDisplayName +
                                "##quick-create-optional-" + parameter.name;
                            if (context.Checkbox(optionalLabel, enabled))
                            {
                                quickCreateArgumentEnabled_[parameter.name] =
                                    enabled;
                            }
                        }

                        if (!enabled)
                        {
                            continue;
                        }

                        std::string visibleLabel = parameterDisplayName;
                        if (!parameter.unit.empty())
                        {
                            visibleLabel += " (" + parameter.unit + ")";
                        }
                        std::string label = visibleLabel;
                        if (parameter.required)
                        {
                            label += " *";
                        }
                        label += "##quick-create-argument-";
                        label += parameter.name;

                        auto found = quickCreateArguments_.find(parameter.name);
                        if (found == quickCreateArguments_.end())
                        {
                            continue;
                        }

                        if (!parameter.choices.empty())
                        {
                            i32 choiceIndex = 0;
                            bool currentChoiceFound = false;
                            for (std::size_t choice = 0U;
                                 choice < parameter.choices.size();
                                 ++choice)
                            {
                                if (parameter.choices[choice].value == found->second)
                                {
                                    choiceIndex = static_cast<i32>(choice);
                                    currentChoiceFound = true;
                                    break;
                                }
                            }

                            if (!currentChoiceFound)
                            {
                                choiceIndex = 0;
                                found->second = parameter.choices.front().value;
                            }

                            constexpr std::size_t kSearchableChoiceThreshold = 8U;
                            if (parameter.choices.size() <= kSearchableChoiceThreshold)
                            {
                                std::vector<std::string_view> choiceLabels;
                                choiceLabels.reserve(parameter.choices.size());
                                for (const auto& commandChoice : parameter.choices)
                                {
                                    choiceLabels.push_back(commandChoice.label);
                                }

                                if (context.Combo(label, choiceLabels, choiceIndex))
                                {
                                    found->second =
                                        parameter.choices[
                                            static_cast<std::size_t>(choiceIndex)].value;
                                }
                            }
                            else
                            {
                                std::string choiceDisplayLabel = visibleLabel;
                                if (parameter.required)
                                {
                                    choiceDisplayLabel += " *";
                                }
                                context.KeyValue(
                                    choiceDisplayLabel,
                                    parameter.choices[
                                        static_cast<std::size_t>(choiceIndex)].label);

                                const std::string pickerKey =
                                    "choice:" + parameter.name;
                                std::string chooseLabel =
                                    "Choose…##quick-create-choice-picker-" +
                                    parameter.name;
                                const bool openPicker = context.Button(chooseLabel);
                                if (openPicker)
                                {
                                    quickCreatePickerQuery_[pickerKey].clear();
                                    quickCreatePickerSelection_[pickerKey] = 0;
                                }

                                const std::string popupId =
                                    "quick-create-choice-picker-popup-" +
                                    parameter.name;
                                if (context.BeginPopup(
                                        popupId,
                                        openPicker,
                                        {.width = 420.0F * editor_ui::CurrentUiScale(),
                                         .height = 0.0F}))
                                {
                                    context.Text("Choose " + parameterDisplayName);
                                    if (openPicker)
                                    {
                                        context.FocusNextItem();
                                    }

                                    auto& query = quickCreatePickerQuery_[pickerKey];
                                    auto& resultSelection =
                                        quickCreatePickerSelection_[pickerKey];
                                    if (context.InputText(
                                            "Search##quick-create-choice-search",
                                            query))
                                    {
                                        resultSelection = 0;
                                    }
                                    if (context.KeyPressed(editor_ui::UiKey::Escape))
                                    {
                                        if (!query.empty())
                                        {
                                            query.clear();
                                            resultSelection = 0;
                                        }
                                        else
                                        {
                                            context.CloseCurrentPopup();
                                        }
                                    }
                                    context.Separator();

                                    struct RankedChoice
                                    {
                                        std::size_t index{0U};
                                        i32 score{0};
                                    };
                                    std::vector<RankedChoice> matches;
                                    for (std::size_t choice = 0U;
                                         choice < parameter.choices.size();
                                         ++choice)
                                    {
                                        const i32 score = PaletteMatchScore(
                                            parameter.choices[choice].label,
                                            query);
                                        if (!query.empty() &&
                                            score == std::numeric_limits<i32>::min())
                                        {
                                            continue;
                                        }
                                        matches.push_back({choice, score});
                                    }
                                    std::ranges::stable_sort(
                                        matches,
                                        [](const RankedChoice& left,
                                           const RankedChoice& right)
                                        {
                                            if (left.score != right.score)
                                            {
                                                return left.score > right.score;
                                            }
                                            return left.index < right.index;
                                        });

                                    if (matches.empty())
                                    {
                                        resultSelection = 0;
                                        context.MutedText("No matching choices.");
                                    }
                                    else
                                    {
                                        const i32 resultCount =
                                            static_cast<i32>(matches.size());
                                        resultSelection = std::clamp(
                                            resultSelection, 0, resultCount - 1);
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Down, true))
                                        {
                                            resultSelection =
                                                (resultSelection + 1) % resultCount;
                                        }
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Up, true))
                                        {
                                            resultSelection =
                                                (resultSelection + resultCount - 1) %
                                                resultCount;
                                        }

                                        const auto chooseChoice =
                                            [&](const std::size_t resultIndex)
                                            {
                                                found->second =
                                                    parameter.choices[resultIndex].value;
                                                query.clear();
                                                context.CloseCurrentPopup();
                                            };

                                        bool chosenByMouse = false;
                                        for (i32 index = 0;
                                             index < resultCount;
                                             ++index)
                                        {
                                            const std::size_t resultIndex =
                                                matches[static_cast<std::size_t>(index)].index;
                                            std::string option =
                                                parameter.choices[resultIndex].label +
                                                "##quick-create-choice-result-" +
                                                std::to_string(resultIndex);
                                            if (context.Selectable(
                                                    option,
                                                    parameter.choices[resultIndex].value ==
                                                        found->second))
                                            {
                                                chooseChoice(resultIndex);
                                                chosenByMouse = true;
                                                break;
                                            }
                                        }

                                        if (!chosenByMouse &&
                                            context.KeyPressed(editor_ui::UiKey::Enter))
                                        {
                                            chooseChoice(
                                                matches[static_cast<std::size_t>(
                                                    resultSelection)].index);
                                        }
                                    }
                                    context.EndPopup();
                                }
                            }
                        }
                        else
                        {
                        switch (parameter.kind)
                        {
                        case commands::CommandValueKind::Boolean:
                            if (auto* value = std::get_if<bool>(&found->second))
                            {
                                static_cast<void>(context.Checkbox(label, *value));
                            }
                            break;
                        case commands::CommandValueKind::Integer:
                            if (auto* value = std::get_if<i64>(&found->second))
                            {
                                static_cast<void>(context.InputInteger(label, *value));
                                if (parameter.minimum.has_value())
                                {
                                    const f64 bounded = std::clamp(
                                        std::ceil(*parameter.minimum),
                                        static_cast<f64>(std::numeric_limits<i64>::min()),
                                        static_cast<f64>(std::numeric_limits<i64>::max()));
                                    *value = std::max(*value, static_cast<i64>(bounded));
                                }
                                if (parameter.maximum.has_value())
                                {
                                    const f64 bounded = std::clamp(
                                        std::floor(*parameter.maximum),
                                        static_cast<f64>(std::numeric_limits<i64>::min()),
                                        static_cast<f64>(std::numeric_limits<i64>::max()));
                                    *value = std::min(*value, static_cast<i64>(bounded));
                                }
                            }
                            break;
                        case commands::CommandValueKind::Float:
                            if (auto* value = std::get_if<f64>(&found->second))
                            {
                                const bool boundedSlider =
                                    parameter.minimum.has_value() &&
                                    parameter.maximum.has_value() &&
                                    std::isfinite(*parameter.minimum) &&
                                    std::isfinite(*parameter.maximum) &&
                                    *parameter.minimum < *parameter.maximum;
                                if (boundedSlider)
                                {
                                    static_cast<void>(context.SliderDouble(
                                        label,
                                        *value,
                                        *parameter.minimum,
                                        *parameter.maximum));
                                }
                                else
                                {
                                    static_cast<void>(context.InputDouble(label, *value));
                                    if (parameter.minimum.has_value())
                                    {
                                        *value = std::max(*value, *parameter.minimum);
                                    }
                                    if (parameter.maximum.has_value())
                                    {
                                        *value = std::min(*value, *parameter.maximum);
                                    }
                                }
                            }
                            break;
                        case commands::CommandValueKind::String:
                            if (auto* value = std::get_if<std::string>(&found->second))
                            {
                                if (!parameter.assetKinds.empty() &&
                                    owner_->content_ != nullptr)
                                {
                                    auto assetMatchesParameter =
                                        [&parameter](const content::AssetRecord& asset)
                                        {
                                            const std::string_view kind =
                                                content::AssetKindName(asset.kind);
                                            return std::ranges::any_of(
                                                parameter.assetKinds,
                                                [kind](const std::string& accepted)
                                                {
                                                    return accepted == kind;
                                                });
                                        };

                                    std::string selected = value->empty()
                                        ? "None"
                                        : *value;
                                    if (!value->empty())
                                    {
                                        if (const auto* asset =
                                                owner_->content_->FindByPath(*value);
                                            asset != nullptr &&
                                            assetMatchesParameter(*asset))
                                        {
                                            selected = asset->name;
                                            selected += " · ";
                                            selected += content::AssetKindName(asset->kind);
                                        }
                                    }
                                    context.KeyValue(parameterDisplayName, selected);

                                    const std::string pickerKey =
                                        "asset:" + parameter.name;
                                    std::string chooseLabel =
                                        "Choose Asset…##quick-create-asset-picker-" +
                                        parameter.name;
                                    const bool openPicker = context.Button(chooseLabel);
                                    if (openPicker)
                                    {
                                        quickCreatePickerQuery_[pickerKey].clear();
                                        quickCreatePickerSelection_[pickerKey] = 0;
                                    }

                                    const std::string popupId =
                                        "quick-create-asset-picker-popup-" +
                                        parameter.name;
                                    if (context.BeginPopup(
                                            popupId,
                                            openPicker,
                                            {.width = 500.0F * editor_ui::CurrentUiScale(),
                                             .height = 0.0F}))
                                    {
                                        context.Text("Choose " + parameterDisplayName);
                                        if (openPicker)
                                        {
                                            context.FocusNextItem();
                                        }

                                        auto& query = quickCreatePickerQuery_[pickerKey];
                                        auto& resultSelection =
                                            quickCreatePickerSelection_[pickerKey];
                                        if (context.InputText(
                                                "Search##quick-create-asset-search",
                                                query))
                                        {
                                            resultSelection = 0;
                                        }
                                        if (context.KeyPressed(editor_ui::UiKey::Escape))
                                        {
                                            if (!query.empty())
                                            {
                                                query.clear();
                                                resultSelection = 0;
                                            }
                                            else
                                            {
                                                context.CloseCurrentPopup();
                                            }
                                        }
                                        context.Separator();

                                        std::vector<content::AssetRecord> candidates;
                                        for (auto asset : owner_->content_->Search(query))
                                        {
                                            if (assetMatchesParameter(asset))
                                            {
                                                candidates.push_back(std::move(asset));
                                            }
                                            if (candidates.size() >= 24U)
                                            {
                                                break;
                                            }
                                        }

                                        if (candidates.empty())
                                        {
                                            resultSelection = 0;
                                            context.MutedText(
                                                "No matching project assets.");
                                        }
                                        else
                                        {
                                            const i32 resultCount =
                                                static_cast<i32>(candidates.size());
                                            resultSelection = std::clamp(
                                                resultSelection, 0, resultCount - 1);
                                            if (context.KeyPressed(
                                                    editor_ui::UiKey::Down, true))
                                            {
                                                resultSelection =
                                                    (resultSelection + 1) % resultCount;
                                            }
                                            if (context.KeyPressed(
                                                    editor_ui::UiKey::Up, true))
                                            {
                                                resultSelection =
                                                    (resultSelection + resultCount - 1) %
                                                    resultCount;
                                            }

                                            const auto chooseAsset =
                                                [&](const content::AssetRecord& asset)
                                                {
                                                    *value =
                                                        asset.sourcePath.generic_string();
                                                    query.clear();
                                                    context.CloseCurrentPopup();
                                                };

                                            bool chosenByMouse = false;
                                            for (i32 index = 0;
                                                 index < resultCount;
                                                 ++index)
                                            {
                                                const auto& asset =
                                                    candidates[static_cast<std::size_t>(index)];
                                                std::string option = asset.name;
                                                option += "\n";
                                                option += content::AssetKindName(asset.kind);
                                                option += " · ";
                                                option += asset.sourcePath.generic_string();
                                                option += "##quick-create-asset-result-";
                                                option += asset.id.ToString();
                                                if (context.Selectable(
                                                        option,
                                                        *value ==
                                                            asset.sourcePath.generic_string()))
                                                {
                                                    chooseAsset(asset);
                                                    chosenByMouse = true;
                                                    break;
                                                }
                                            }

                                            if (!chosenByMouse &&
                                                context.KeyPressed(editor_ui::UiKey::Enter))
                                            {
                                                chooseAsset(
                                                    candidates[static_cast<std::size_t>(
                                                        resultSelection)]);
                                            }
                                        }
                                        context.EndPopup();
                                    }

                                    std::string advancedLabel =
                                        "Advanced asset path##quick-create-asset-path-" +
                                        parameter.name;
                                    if (context.Section(advancedLabel, false))
                                    {
                                        static_cast<void>(
                                            context.InputText(label, *value));
                                    }
                                }
                                else
                                {
                                    static_cast<void>(context.InputText(label, *value));
                                }
                            }
                            break;
                        case commands::CommandValueKind::Vector3:
                            if (auto* value = std::get_if<math::Double3>(&found->second))
                            {
                                static_cast<void>(context.InputDouble3(label, *value));
                                if (parameter.minimum.has_value())
                                {
                                    value->x = std::max(value->x, *parameter.minimum);
                                    value->y = std::max(value->y, *parameter.minimum);
                                    value->z = std::max(value->z, *parameter.minimum);
                                }
                                if (parameter.maximum.has_value())
                                {
                                    value->x = std::min(value->x, *parameter.maximum);
                                    value->y = std::min(value->y, *parameter.maximum);
                                    value->z = std::min(value->z, *parameter.maximum);
                                }
                            }
                            break;
                        case commands::CommandValueKind::ObjectId:
                        {
                            auto& idText = quickCreateIdText_[parameter.name];
                            std::string selected = "None";
                            if (const auto parsed = scene::ObjectId::Parse(idText);
                                parsed.has_value())
                            {
                                if (const auto record = world.Objects().Find(*parsed);
                                    record.has_value())
                                {
                                    selected = record->name;
                                    if (const auto* type =
                                            world.Schemas().FindType(record->type);
                                        type != nullptr)
                                    {
                                        selected += " · ";
                                        selected += type->displayName;
                                    }
                                }
                                else
                                {
                                    selected = parsed->ToString();
                                }
                            }
                            context.KeyValue(parameterDisplayName, selected);

                            std::string chooseLabel =
                                "Choose…##quick-create-object-picker-" +
                                parameter.name;
                            const bool openPicker = context.Button(chooseLabel);
                            const std::string pickerSelectionKey =
                                "object:" + parameter.name;
                            if (openPicker)
                            {
                                quickCreatePickerQuery_[parameter.name].clear();
                                quickCreatePickerSelection_[pickerSelectionKey] = 0;
                            }
                            std::string popupId =
                                "quick-create-object-picker-popup-" +
                                parameter.name;
                            if (context.BeginPopup(
                                    popupId,
                                    openPicker,
                                    {.width = 440.0F * editor_ui::CurrentUiScale(),
                                     .height = 0.0F}))
                            {
                                context.Text("Choose Object");
                                if (openPicker)
                                {
                                    context.FocusNextItem();
                                }
                                auto& query =
                                    quickCreatePickerQuery_[parameter.name];
                                auto& resultSelection =
                                    quickCreatePickerSelection_[pickerSelectionKey];
                                if (context.InputText(
                                        "Search##quick-create-object-search",
                                        query))
                                {
                                    resultSelection = 0;
                                }
                                if (context.KeyPressed(editor_ui::UiKey::Escape))
                                {
                                    if (!query.empty())
                                    {
                                        query.clear();
                                        resultSelection = 0;
                                    }
                                    else
                                    {
                                        context.CloseCurrentPopup();
                                    }
                                }
                                context.Separator();

                                const auto selectedObjectId =
                                    scene::ObjectId::Parse(idText);
                                const auto chooseObject =
                                    [&](const scene::ObjectRecord& candidate)
                                    {
                                        found->second = candidate.id;
                                        idText = candidate.id.ToString();
                                        query.clear();
                                        context.CloseCurrentPopup();
                                    };

                                std::vector<scene::ObjectId> selectedObjectPath;
                                if (selectedObjectId.has_value())
                                {
                                    auto current =
                                        world.Objects().Find(*selectedObjectId);
                                    for (u32 depth = 0U;
                                         current.has_value() && depth < 64U;
                                         ++depth)
                                    {
                                        selectedObjectPath.push_back(current->id);
                                        if (!current->parent.has_value())
                                        {
                                            break;
                                        }
                                        current =
                                            world.Objects().Find(*current->parent);
                                    }
                                }

                                if (!query.empty())
                                {
                                    const auto candidates =
                                        world.Explorer().Search(query, 20U);
                                    if (candidates.empty())
                                    {
                                        resultSelection = 0;
                                        context.MutedText("No matching objects.");
                                    }
                                    else
                                    {
                                        const i32 resultCount =
                                            static_cast<i32>(candidates.size());
                                        resultSelection = std::clamp(
                                            resultSelection,
                                            0,
                                            resultCount - 1);
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Down,
                                                true))
                                        {
                                            resultSelection =
                                                (resultSelection + 1) % resultCount;
                                        }
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Up,
                                                true))
                                        {
                                            resultSelection =
                                                (resultSelection + resultCount - 1) %
                                                resultCount;
                                        }

                                        bool chosenByMouse = false;
                                        for (i32 index = 0;
                                             index < resultCount;
                                             ++index)
                                        {
                                            const auto& candidate =
                                                candidates[static_cast<std::size_t>(index)];
                                            std::string option = candidate.name;
                                            if (const auto* type =
                                                    world.Schemas().FindType(candidate.type);
                                                type != nullptr)
                                            {
                                                option += "\n";
                                                option += type->displayName;
                                            }
                                            option += " · ";
                                            option += candidate.id.ToString();
                                            option += "##object-picker-search-result-";
                                            option += candidate.id.ToString();

                                            if (context.Selectable(
                                                    option,
                                                    index == resultSelection))
                                            {
                                                chooseObject(candidate);
                                                chosenByMouse = true;
                                                break;
                                            }
                                        }
                                        if (!chosenByMouse &&
                                            context.KeyPressed(
                                                editor_ui::UiKey::Enter) &&
                                            resultSelection >= 0 &&
                                            resultSelection < resultCount)
                                        {
                                            chooseObject(
                                                candidates[static_cast<std::size_t>(
                                                    resultSelection)]);
                                        }
                                    }
                                }
                                else
                                {
                                    const auto selectedObjects =
                                        world.Selection().Ordered();
                                    if (!selectedObjects.empty())
                                    {
                                        context.MutedText("Selection");
                                        for (const auto selectedId : selectedObjects)
                                        {
                                            const auto candidate =
                                                world.Objects().Find(selectedId);
                                            if (!candidate.has_value())
                                            {
                                                continue;
                                            }

                                            std::string option = candidate->name;
                                            if (const auto* type =
                                                    world.Schemas().FindType(candidate->type);
                                                type != nullptr)
                                            {
                                                option += "\n";
                                                option += type->displayName;
                                            }
                                            option += "##object-picker-selected-";
                                            option += candidate->id.ToString();
                                            if (context.Selectable(
                                                    option,
                                                    selectedObjectId.has_value() &&
                                                        candidate->id == *selectedObjectId))
                                            {
                                                chooseObject(*candidate);
                                            }
                                        }
                                        context.Separator();
                                    }

                                    const auto roots = world.Explorer().Roots();
                                    if (roots.empty())
                                    {
                                        context.MutedText("No objects are available in this world.");
                                    }
                                    else
                                    {
                                        context.MutedText("Hierarchy");
                                        std::function<void(
                                            const scene::ObjectRecord&,
                                            u32)> drawObjectNode;
                                        drawObjectNode =
                                            [&](const scene::ObjectRecord& candidate,
                                                const u32 depth)
                                            {
                                                constexpr u32 kMaximumPickerDepth = 64U;
                                                const auto children =
                                                    depth < kMaximumPickerDepth
                                                        ? world.Explorer().Children(candidate.id)
                                                        : std::vector<scene::ObjectRecord>{};
                                                const bool isChosen =
                                                    selectedObjectId.has_value() &&
                                                    candidate.id == *selectedObjectId;
                                                const bool onSelectedPath =
                                                    std::ranges::find(
                                                        selectedObjectPath,
                                                        candidate.id) !=
                                                    selectedObjectPath.end();

                                                std::string typeName;
                                                if (const auto* type =
                                                        world.Schemas().FindType(candidate.type);
                                                    type != nullptr)
                                                {
                                                    typeName = type->displayName;
                                                }

                                                if (children.empty())
                                                {
                                                    std::string option = candidate.name;
                                                    if (!typeName.empty())
                                                    {
                                                        option += " · ";
                                                        option += typeName;
                                                    }
                                                    option += "##object-picker-leaf-";
                                                    option += candidate.id.ToString();
                                                    if (context.Selectable(option, isChosen))
                                                    {
                                                        chooseObject(candidate);
                                                    }
                                                    return;
                                                }

                                                std::string nodeLabel = candidate.name;
                                                if (!typeName.empty())
                                                {
                                                    nodeLabel += " · ";
                                                    nodeLabel += typeName;
                                                }
                                                nodeLabel += "##object-picker-node-";
                                                nodeLabel += candidate.id.ToString();
                                                context.SetNextTreeItemOpen(
                                                    onSelectedPath);
                                                const auto interaction =
                                                    context.TreeItem(nodeLabel, isChosen);
                                                context.SameLine();
                                                std::string chooseLabel =
                                                    "Choose##object-picker-choose-" +
                                                    candidate.id.ToString();
                                                if (context.Button(chooseLabel))
                                                {
                                                    chooseObject(candidate);
                                                }
                                                if (interaction.open)
                                                {
                                                    for (const auto& child : children)
                                                    {
                                                        drawObjectNode(child, depth + 1U);
                                                    }
                                                    context.TreePop();
                                                }
                                            };

                                        for (const auto& root : roots)
                                        {
                                            drawObjectNode(root, 0U);
                                        }
                                    }
                                }
                                context.EndPopup();
                            }

                            std::string advancedLabel =
                                "Advanced UUID##quick-create-object-uuid-" +
                                parameter.name;
                            if (context.Section(advancedLabel, false))
                            {
                                static_cast<void>(context.InputText(label, idText));
                            }
                            break;
                        }
                        case commands::CommandValueKind::PropertyId:
                        {
                            auto& idText = quickCreateIdText_[parameter.name];
                            std::string selected = "None";
                            if (const auto parsed = schema::PropertyId::Parse(idText);
                                parsed.has_value())
                            {
                                const auto catalog = world.Schemas().Catalog();
                                for (const auto& type : catalog)
                                {
                                    const auto property = std::ranges::find_if(
                                        type.properties,
                                        [&parsed](const schema::PropertySchema& item)
                                        {
                                            return item.id == *parsed;
                                        });
                                    if (property != type.properties.end())
                                    {
                                        selected = property->name +
                                            " · " + type.displayName;
                                        break;
                                    }
                                }
                                if (selected == "None")
                                {
                                    selected = parsed->ToString();
                                }
                            }
                            context.KeyValue(parameterDisplayName, selected);

                            std::string chooseLabel =
                                "Choose…##quick-create-property-picker-" +
                                parameter.name;
                            const bool openPicker = context.Button(chooseLabel);
                            const std::string pickerSelectionKey =
                                "property:" + parameter.name;
                            if (openPicker)
                            {
                                quickCreatePickerQuery_[parameter.name].clear();
                                quickCreatePickerSelection_[pickerSelectionKey] = 0;
                            }
                            std::string popupId =
                                "quick-create-property-picker-popup-" +
                                parameter.name;
                            if (context.BeginPopup(
                                    popupId,
                                    openPicker,
                                    {.width = 460.0F * editor_ui::CurrentUiScale(),
                                     .height = 0.0F}))
                            {
                                context.Text("Choose Property");
                                if (openPicker)
                                {
                                    context.FocusNextItem();
                                }
                                auto& query =
                                    quickCreatePickerQuery_[parameter.name];
                                auto& resultSelection =
                                    quickCreatePickerSelection_[pickerSelectionKey];
                                if (context.InputText(
                                        "Search##quick-create-property-search",
                                        query))
                                {
                                    resultSelection = 0;
                                }
                                if (context.KeyPressed(editor_ui::UiKey::Escape))
                                {
                                    if (!query.empty())
                                    {
                                        query.clear();
                                        resultSelection = 0;
                                    }
                                    else
                                    {
                                        context.CloseCurrentPopup();
                                    }
                                }
                                context.Separator();

                                std::optional<schema::TypeId> preferredType;
                                for (const auto& commandParameter :
                                     descriptor->parameters)
                                {
                                    if (commandParameter.kind !=
                                        commands::CommandValueKind::ObjectId)
                                    {
                                        continue;
                                    }
                                    const auto textIt =
                                        quickCreateIdText_.find(commandParameter.name);
                                    if (textIt == quickCreateIdText_.end())
                                    {
                                        continue;
                                    }
                                    const auto objectId =
                                        scene::ObjectId::Parse(textIt->second);
                                    if (!objectId.has_value())
                                    {
                                        continue;
                                    }
                                    if (const auto object =
                                            world.Objects().Find(*objectId);
                                        object.has_value())
                                    {
                                        preferredType = object->type;
                                        break;
                                    }
                                }
                                if (!preferredType.has_value() &&
                                    world.Selection().Ordered().size() == 1U)
                                {
                                    if (const auto selectedObject =
                                            world.Objects().Find(
                                                world.Selection().Ordered().front());
                                        selectedObject.has_value())
                                    {
                                        preferredType = selectedObject->type;
                                    }
                                }

                                const auto catalog = world.Schemas().Catalog();
                                const auto selectedPropertyId =
                                    schema::PropertyId::Parse(idText);
                                const auto chooseProperty =
                                    [&](const schema::PropertySchema& property)
                                    {
                                        found->second = property.id;
                                        idText = property.id.ToString();
                                        query.clear();
                                        context.CloseCurrentPopup();
                                    };

                                std::optional<schema::TypeId> selectedPropertyType;
                                if (selectedPropertyId.has_value())
                                {
                                    for (const auto& type : catalog)
                                    {
                                        const auto property = std::ranges::find_if(
                                            type.properties,
                                            [&selectedPropertyId](
                                                const schema::PropertySchema& item)
                                            {
                                                return item.id ==
                                                    *selectedPropertyId;
                                            });
                                        if (property != type.properties.end())
                                        {
                                            selectedPropertyType = type.id;
                                            break;
                                        }
                                    }
                                }

                                if (!query.empty())
                                {
                                    struct PropertyCandidate
                                    {
                                        const schema::TypeSchema* type{nullptr};
                                        const schema::PropertySchema* property{nullptr};
                                        bool preferred{false};
                                    };

                                    std::vector<PropertyCandidate> candidates;
                                    const std::string lowerQuery = PaletteLower(query);
                                    for (const auto& type : catalog)
                                    {
                                        for (const auto& property : type.properties)
                                        {
                                            std::string searchable =
                                                property.name + " " +
                                                type.displayName + " " +
                                                type.category;
                                            if (PaletteLower(searchable).find(lowerQuery) ==
                                                std::string::npos)
                                            {
                                                continue;
                                            }

                                            candidates.push_back({
                                                .type = &type,
                                                .property = &property,
                                                .preferred =
                                                    preferredType.has_value() &&
                                                    type.id == *preferredType
                                            });
                                        }
                                    }

                                    std::ranges::stable_sort(
                                        candidates,
                                        [](const PropertyCandidate& left,
                                           const PropertyCandidate& right)
                                        {
                                            if (left.preferred != right.preferred)
                                            {
                                                return left.preferred;
                                            }
                                            if (left.type->displayName !=
                                                right.type->displayName)
                                            {
                                                return left.type->displayName <
                                                    right.type->displayName;
                                            }
                                            return left.property->name <
                                                right.property->name;
                                        });
                                    if (candidates.size() > 24U)
                                    {
                                        candidates.resize(24U);
                                    }

                                    if (candidates.empty())
                                    {
                                        resultSelection = 0;
                                        context.MutedText(
                                            "No matching schema properties.");
                                    }
                                    else
                                    {
                                        const i32 resultCount =
                                            static_cast<i32>(candidates.size());
                                        resultSelection = std::clamp(
                                            resultSelection,
                                            0,
                                            resultCount - 1);
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Down,
                                                true))
                                        {
                                            resultSelection =
                                                (resultSelection + 1) % resultCount;
                                        }
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Up,
                                                true))
                                        {
                                            resultSelection =
                                                (resultSelection + resultCount - 1) %
                                                resultCount;
                                        }

                                        bool chosenByMouse = false;
                                        for (i32 index = 0;
                                             index < resultCount;
                                             ++index)
                                        {
                                            const auto& candidate =
                                                candidates[static_cast<std::size_t>(index)];
                                            std::string option =
                                                candidate.property->name;
                                            option += "\n";
                                            option += candidate.type->displayName;
                                            if (!candidate.property->unit.empty())
                                            {
                                                option += " · ";
                                                option += candidate.property->unit;
                                            }
                                            option += " · ";
                                            option += candidate.property->id.ToString();
                                            option += "##property-picker-search-result-";
                                            option += candidate.property->id.ToString();

                                            if (context.Selectable(
                                                    option,
                                                    index == resultSelection))
                                            {
                                                chooseProperty(*candidate.property);
                                                chosenByMouse = true;
                                                break;
                                            }
                                        }
                                        if (!chosenByMouse &&
                                            context.KeyPressed(
                                                editor_ui::UiKey::Enter) &&
                                            resultSelection >= 0 &&
                                            resultSelection < resultCount)
                                        {
                                            chooseProperty(
                                                *candidates[static_cast<std::size_t>(
                                                    resultSelection)].property);
                                        }
                                    }
                                }
                                else
                                {
                                    struct PropertyCategoryGroup
                                    {
                                        std::string name;
                                        std::vector<const schema::TypeSchema*> types;
                                        bool preferred{false};
                                    };

                                    std::vector<PropertyCategoryGroup> groups;
                                    for (const auto& type : catalog)
                                    {
                                        if (type.properties.empty())
                                        {
                                            continue;
                                        }

                                        const std::string category =
                                            type.category.empty()
                                                ? "Uncategorized"
                                                : type.category;
                                        auto group = std::ranges::find_if(
                                            groups,
                                            [&category](
                                                const PropertyCategoryGroup& item)
                                            {
                                                return item.name == category;
                                            });
                                        if (group == groups.end())
                                        {
                                            groups.push_back({
                                                .name = category
                                            });
                                            group = groups.end() - 1;
                                        }

                                        group->types.push_back(&type);
                                        if (preferredType.has_value() &&
                                            type.id == *preferredType)
                                        {
                                            group->preferred = true;
                                        }
                                    }

                                    std::ranges::stable_sort(
                                        groups,
                                        [](const PropertyCategoryGroup& left,
                                           const PropertyCategoryGroup& right)
                                        {
                                            if (left.preferred != right.preferred)
                                            {
                                                return left.preferred;
                                            }
                                            return left.name < right.name;
                                        });

                                    if (groups.empty())
                                    {
                                        context.MutedText(
                                            "No schema properties are registered.");
                                    }
                                    else
                                    {
                                        context.MutedText("Schema");
                                        for (std::size_t groupIndex = 0;
                                             groupIndex < groups.size();
                                             ++groupIndex)
                                        {
                                            auto& group = groups[groupIndex];
                                            std::ranges::stable_sort(
                                                group.types,
                                                [&preferredType](
                                                    const schema::TypeSchema* left,
                                                    const schema::TypeSchema* right)
                                                {
                                                    const bool leftPreferred =
                                                        preferredType.has_value() &&
                                                        left->id == *preferredType;
                                                    const bool rightPreferred =
                                                        preferredType.has_value() &&
                                                        right->id == *preferredType;
                                                    if (leftPreferred != rightPreferred)
                                                    {
                                                        return leftPreferred;
                                                    }
                                                    return left->displayName <
                                                        right->displayName;
                                                });

                                            std::string categoryLabel = group.name;
                                            categoryLabel += std::format(
                                                "##property-picker-category-{}",
                                                groupIndex);
                                            const bool categoryContainsSelected =
                                                selectedPropertyType.has_value() &&
                                                std::ranges::any_of(
                                                    group.types,
                                                    [&selectedPropertyType](
                                                        const schema::TypeSchema* type)
                                                    {
                                                        return type->id ==
                                                            *selectedPropertyType;
                                                    });
                                            context.SetNextTreeItemOpen(
                                                group.preferred ||
                                                categoryContainsSelected);
                                            const auto categoryInteraction =
                                                context.TreeItem(
                                                    categoryLabel,
                                                    false);
                                            if (!categoryInteraction.open)
                                            {
                                                continue;
                                            }

                                            for (const auto* type : group.types)
                                            {
                                                const bool typePreferred =
                                                    preferredType.has_value() &&
                                                    type->id == *preferredType;
                                                std::string typeLabel =
                                                    type->displayName;
                                                if (typePreferred)
                                                {
                                                    typeLabel += " · Current Type";
                                                }
                                                typeLabel +=
                                                    "##property-picker-type-";
                                                typeLabel += type->id.ToString();
                                                const bool typeContainsSelected =
                                                    selectedPropertyType.has_value() &&
                                                    type->id == *selectedPropertyType;
                                                context.SetNextTreeItemOpen(
                                                    typePreferred ||
                                                    typeContainsSelected);
                                                const auto typeInteraction =
                                                    context.TreeItem(
                                                        typeLabel,
                                                        false);
                                                if (!typeInteraction.open)
                                                {
                                                    continue;
                                                }

                                                for (const auto& property :
                                                     type->properties)
                                                {
                                                    std::string option =
                                                        property.name;
                                                    if (!property.unit.empty())
                                                    {
                                                        option += " · ";
                                                        option += property.unit;
                                                    }
                                                    option +=
                                                        "##property-picker-property-";
                                                    option += property.id.ToString();
                                                    if (context.Selectable(
                                                            option,
                                                            selectedPropertyId.has_value() &&
                                                                property.id ==
                                                                    *selectedPropertyId))
                                                    {
                                                        chooseProperty(property);
                                                    }
                                                }
                                                context.TreePop();
                                            }
                                            context.TreePop();
                                        }
                                    }
                                }
                                context.EndPopup();
                            }

                            std::string advancedLabel =
                                "Advanced UUID##quick-create-property-uuid-" +
                                parameter.name;
                            if (context.Section(advancedLabel, false))
                            {
                                static_cast<void>(context.InputText(label, idText));
                            }
                            break;
                        }
                        }
                        }

                        if (!parameter.description.empty())
                        {
                            context.MutedText(parameter.description);
                        }
                    }

                    if (!quickCreateArgumentError_.empty())
                    {
                        context.ErrorText(quickCreateArgumentError_);
                    }

                    if (context.PrimaryButton("Create##quick-create-argument-submit"))
                    {
                        commands::CommandArguments arguments;
                        std::string validationError;

                        for (const auto& parameter : descriptor->parameters)
                        {
                            const bool enabled = parameter.required ||
                                quickCreateArgumentEnabled_[parameter.name];
                            if (!enabled)
                            {
                                continue;
                            }

                            const auto found =
                                quickCreateArguments_.find(parameter.name);
                            if (found == quickCreateArguments_.end())
                            {
                                validationError =
                                    "Missing generated value for " +
                                    parameter.name + ".";
                                break;
                            }

                            if (parameter.kind ==
                                commands::CommandValueKind::ObjectId)
                            {
                                const auto parsed = scene::ObjectId::Parse(
                                    quickCreateIdText_[parameter.name]);
                                if (!parsed.has_value())
                                {
                                    validationError =
                                        parameter.name +
                                        " must be a valid object UUID.";
                                    break;
                                }
                                arguments[parameter.name] = *parsed;
                            }
                            else if (parameter.kind ==
                                     commands::CommandValueKind::PropertyId)
                            {
                                const auto parsed = schema::PropertyId::Parse(
                                    quickCreateIdText_[parameter.name]);
                                if (!parsed.has_value())
                                {
                                    validationError =
                                        parameter.name +
                                        " must be a valid property UUID.";
                                    break;
                                }
                                arguments[parameter.name] = *parsed;
                            }
                            else
                            {
                                arguments[parameter.name] = found->second;
                            }
                        }

                        if (!validationError.empty())
                        {
                            quickCreateArgumentError_ =
                                std::move(validationError);
                        }
                        else
                        {
                            try
                            {
                                registry.Invoke(
                                    quickCreateArgumentCommand_,
                                    arguments);
                                owner_->status_.clear();
                                clearArgumentForm();
                                quickCreateBrowseQuery_.clear();
                                quickCreateBrowseSelection_ = 0;
                                context.CloseCurrentPopup();
                            }
                            catch (const std::exception& exception)
                            {
                                quickCreateArgumentError_ =
                                    exception.what();
                                owner_->status_ = exception.what();
                            }
                        }
                    }

                    if (context.KeyPressed(editor_ui::UiKey::Escape))
                    {
                        clearArgumentForm();
                    }
                }
            }

            context.EndPopup();
            return;
        }

        std::vector<CommandPaletteEntry> creationEntries;
        creationEntries.reserve(commandCatalog.size());
        for (const auto& command : commandCatalog)
        {
            if (!IsQuickCreateCatalogEntry(command))
            {
                continue;
            }

            const auto paletteEntry =
                std::ranges::find_if(
                    palette,
                    [&command](const CommandPaletteEntry& entry)
                    {
                        return entry.command == command.id;
                    });
            if (paletteEntry == palette.end())
            {
                continue;
            }
            creationEntries.push_back(*paletteEntry);
        }

        const auto browsePluginCatalog =
            GlobalStudioUiContributions().Catalog(
                StudioContributionSurface::QuickCreate);
        for (const auto& contribution : browsePluginCatalog)
        {
            if (contribution.kind != StudioContributionKind::Command ||
                !contribution.command.IsValid())
            {
                continue;
            }

            const auto paletteEntry =
                std::ranges::find_if(
                    palette,
                    [&contribution](const CommandPaletteEntry& entry)
                    {
                        return entry.command == contribution.command;
                    });
            if (paletteEntry == palette.end() ||
                std::ranges::any_of(
                    creationEntries,
                    [&contribution](const CommandPaletteEntry& entry)
                    {
                        return entry.command == contribution.command;
                    }))
            {
                continue;
            }

            auto entry = *paletteEntry;
            if (!contribution.label.empty())
            {
                entry.label = contribution.label;
            }
            if (!contribution.category.empty())
            {
                entry.category = contribution.category;
            }
            creationEntries.push_back(std::move(entry));
        }

        const auto matches =
            SearchCommandPalette(
                creationEntries,
                quickCreateBrowseQuery_,
                creationEntries.size());

        std::vector<CommandPaletteEntry> visibleMatches;
        visibleMatches.reserve(12U);
        for (const auto& entry : matches)
        {
            if (!registry.Enablement(entry.command).enabled)
            {
                continue;
            }

            visibleMatches.push_back(entry);
            if (visibleMatches.size() >= 12U)
            {
                break;
            }
        }

        const auto invokeCreation =
            [&](const CommandPaletteEntry& entry)
            {
                if (entry.requiresArguments)
                {
                    beginArgumentForm(entry.command);
                    return false;
                }

                try
                {
                    registry.Invoke(entry.command);
                    owner_->status_.clear();
                    quickCreateBrowseQuery_.clear();
                    quickCreateBrowseSelection_ = 0;
                    context.CloseCurrentPopup();
                    return true;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                    return false;
                }
            };

        if (context.KeyPressed(editor_ui::UiKey::Escape))
        {
            quickCreateBrowseQuery_.clear();
            quickCreateBrowseSelection_ = 0;
            context.CloseCurrentPopup();
        }
        else if (visibleMatches.empty())
        {
            quickCreateBrowseSelection_ = 0;
            context.MutedText(
                "No matching creation commands are enabled in the current context.");
        }
        else
        {
            const i32 visibleCount =
                static_cast<i32>(visibleMatches.size());
            quickCreateBrowseSelection_ = std::clamp(
                quickCreateBrowseSelection_,
                0,
                visibleCount - 1);

            if (context.KeyPressed(editor_ui::UiKey::Down, true))
            {
                quickCreateBrowseSelection_ =
                    (quickCreateBrowseSelection_ + 1) % visibleCount;
            }
            if (context.KeyPressed(editor_ui::UiKey::Up, true))
            {
                quickCreateBrowseSelection_ =
                    (quickCreateBrowseSelection_ + visibleCount - 1) %
                    visibleCount;
            }

            bool invoked = false;
            for (i32 index = 0; index < visibleCount; ++index)
            {
                const auto& entry =
                    visibleMatches[static_cast<std::size_t>(index)];
                std::string label = entry.label;
                const std::string secondary =
                    CommandPaletteSecondaryText(entry);
                if (!secondary.empty())
                {
                    label += "\n";
                    label += secondary;
                }
                label += "##quick-create-browser-";
                label += entry.command.ToString();

                if (context.Selectable(
                        label,
                        index == quickCreateBrowseSelection_))
                {
                    quickCreateBrowseSelection_ = index;
                    invoked = invokeCreation(entry);
                    if (invoked)
                    {
                        break;
                    }
                }
            }

            if (!invoked &&
                context.KeyPressed(editor_ui::UiKey::Enter))
            {
                static_cast<void>(
                    invokeCreation(
                        visibleMatches[static_cast<std::size_t>(
                            quickCreateBrowseSelection_)]));
            }
        }

        context.EndPopup();
    }

}
} // namespace orbit::studio_ui
