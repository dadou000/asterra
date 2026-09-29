from pathlib import Path


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label} anchor count {count}')
    path.write_text(text.replace(old, new, 1), encoding='utf-8')


header = Path('engine/commands/include/orbit/commands/CommandRegistry.hpp')
replace_once(
    header,
    '''struct CommandParameter\n{\n    std::string name;\n''',
    '''struct CommandChoice\n{\n    std::string label;\n    CommandValue value;\n};\n\nstruct CommandParameter\n{\n    std::string name;\n''',
    'CommandChoice declaration')
replace_once(
    header,
    '''    std::optional<f64> minimum;\n    std::optional<f64> maximum;\n};\n''',
    '''    std::optional<f64> minimum;\n    std::optional<f64> maximum;\n    // Enumerated values render as a generated drop-down. Values remain typed\n    // CommandValue instances so the same metadata works for string and numeric modes.\n    std::vector<CommandChoice> choices;\n};\n''',
    'CommandParameter choices field')

registry = Path('engine/commands/src/CommandRegistry.cpp')
replace_once(
    registry,
    '''        if (parameter.name.empty() ||\n            !parameterNames.insert(\n                parameter.name).second)\n        {\n            throw std::invalid_argument(\n                "Command parameters require unique non-empty names.");\n        }\n''',
    '''        if (parameter.name.empty() ||\n            !parameterNames.insert(\n                parameter.name).second)\n        {\n            throw std::invalid_argument(\n                "Command parameters require unique non-empty names.");\n        }\n\n        if (parameter.defaultValue.has_value() &&\n            KindOf(*parameter.defaultValue) != parameter.kind)\n        {\n            throw std::invalid_argument(\n                "Command parameter default value has wrong type: " +\n                parameter.name);\n        }\n\n        std::unordered_set<std::string> choiceLabels;\n        for (const CommandChoice& choice : parameter.choices)\n        {\n            if (choice.label.empty() ||\n                !choiceLabels.insert(choice.label).second)\n            {\n                throw std::invalid_argument(\n                    "Command parameter choices require unique non-empty labels: " +\n                    parameter.name);\n            }\n\n            if (KindOf(choice.value) != parameter.kind)\n            {\n                throw std::invalid_argument(\n                    "Command parameter choice has wrong type: " +\n                    parameter.name);\n            }\n        }\n\n        if (parameter.defaultValue.has_value() &&\n            !parameter.choices.empty())\n        {\n            const bool defaultIsChoice =\n                std::ranges::any_of(\n                    parameter.choices,\n                    [&parameter](const CommandChoice& choice)\n                    {\n                        return choice.value == *parameter.defaultValue;\n                    });\n\n            if (!defaultIsChoice)\n            {\n                throw std::invalid_argument(\n                    "Command parameter default value is not an enumerated choice: " +\n                    parameter.name);\n            }\n        }\n''',
    'CommandParameter validation')
# ranges::any_of is now used by registry validation.
text = registry.read_text(encoding='utf-8')
if '#include <ranges>\n' not in text:
    text = text.replace('#include <stdexcept>\n', '#include <ranges>\n#include <stdexcept>\n', 1)
registry.write_text(text, encoding='utf-8')

volume = Path('engine/editor_model/src/VolumeAuthoringCommands.cpp')
replace_once(
    volume,
    '''                .defaultValue =\n                    commands::CommandValue{std::string{"Empty"}}\n''',
    '''                .defaultValue =\n                    commands::CommandValue{std::string{"Empty"}},\n                .choices = {\n                    {.label = "Empty", .value = commands::CommandValue{std::string{"Empty"}}},\n                    {.label = "Smoke", .value = commands::CommandValue{std::string{"Smoke"}}},\n                    {.label = "Fire", .value = commands::CommandValue{std::string{"Fire"}}},\n                    {.label = "Fog", .value = commands::CommandValue{std::string{"Fog"}}},\n                    {.label = "Dust", .value = commands::CommandValue{std::string{"Dust"}}},\n                    {.label = "Snow", .value = commands::CommandValue{std::string{"Snow"}}},\n                    {.label = "Surface Flow", .value = commands::CommandValue{std::string{"Surface Flow"}}}\n                }\n''',
    'Volume preset choices')
replace_once(
    volume,
    '''                .displayName = "Source Type",\n                .description =\n                    "Source adapter: Brush, Texture / Mask, Terrain, Spline, Mesh / SDF, Collision Proxy, Particles, Object Motion, or World Motion."\n''',
    '''                .displayName = "Source Type",\n                .description =\n                    "Source adapter used by the selected universal Volume.",\n                .choices = {\n                    {.label = "Brush", .value = commands::CommandValue{std::string{"Brush"}}},\n                    {.label = "Texture / Mask", .value = commands::CommandValue{std::string{"Texture / Mask"}}},\n                    {.label = "Terrain", .value = commands::CommandValue{std::string{"Terrain"}}},\n                    {.label = "Spline", .value = commands::CommandValue{std::string{"Spline"}}},\n                    {.label = "Mesh / SDF", .value = commands::CommandValue{std::string{"Mesh / SDF"}}},\n                    {.label = "Collision Proxy", .value = commands::CommandValue{std::string{"Collision Proxy"}}},\n                    {.label = "Particles", .value = commands::CommandValue{std::string{"Particles"}}},\n                    {.label = "Object Motion", .value = commands::CommandValue{std::string{"Object Motion"}}},\n                    {.label = "World Motion", .value = commands::CommandValue{std::string{"World Motion"}}}\n                }\n''',
    'Volume source choices')
replace_once(
    volume,
    '''                .displayName = "Effector Type",\n                .description =\n                    "Effector type: Obstacle, Drag, Wind, Temperature, or Dissipation."\n''',
    '''                .displayName = "Effector Type",\n                .description =\n                    "Obstacle or force behavior applied by the selected universal Volume.",\n                .choices = {\n                    {.label = "Obstacle", .value = commands::CommandValue{std::string{"Obstacle"}}},\n                    {.label = "Drag", .value = commands::CommandValue{std::string{"Drag"}}},\n                    {.label = "Wind", .value = commands::CommandValue{std::string{"Wind"}}},\n                    {.label = "Temperature", .value = commands::CommandValue{std::string{"Temperature"}}},\n                    {.label = "Dissipation", .value = commands::CommandValue{std::string{"Dissipation"}}}\n                }\n''',
    'Volume effector choices')

shell = Path('engine/studio_ui/src/StudioExpansionShell.cpp')
replace_once(
    shell,
    '''                            continue;\n                        }\n                    }\n\n                    switch (parameter.kind)\n''',
    '''                            continue;\n                        }\n                    }\n\n                    if (!parameter.choices.empty())\n                    {\n                        quickCreateArguments_[parameter.name] =\n                            parameter.choices.front().value;\n                        continue;\n                    }\n\n                    switch (parameter.kind)\n''',
    'choice initialization')
replace_once(
    shell,
    '''                        switch (parameter.kind)\n                        {\n                        case commands::CommandValueKind::Boolean:\n''',
    '''                        if (!parameter.choices.empty())\n                        {\n                            std::vector<std::string_view> choiceLabels;\n                            choiceLabels.reserve(parameter.choices.size());\n\n                            i32 choiceIndex = 0;\n                            bool currentChoiceFound = false;\n                            for (std::size_t choice = 0U;\n                                 choice < parameter.choices.size();\n                                 ++choice)\n                            {\n                                const auto& commandChoice =\n                                    parameter.choices[choice];\n                                choiceLabels.push_back(commandChoice.label);\n                                if (commandChoice.value == found->second)\n                                {\n                                    choiceIndex = static_cast<i32>(choice);\n                                    currentChoiceFound = true;\n                                }\n                            }\n\n                            if (!currentChoiceFound)\n                            {\n                                choiceIndex = 0;\n                                found->second = parameter.choices.front().value;\n                            }\n\n                            if (context.Combo(label, choiceLabels, choiceIndex))\n                            {\n                                found->second =\n                                    parameter.choices[\n                                        static_cast<std::size_t>(choiceIndex)].value;\n                            }\n                        }\n                        else\n                        {\n                        switch (parameter.kind)\n                        {\n                        case commands::CommandValueKind::Boolean:\n''',
    'choice rendering start')
replace_once(
    shell,
    '''                        }\n                        }\n\n                        if (!parameter.description.empty())\n''',
    '''                        }\n                        }\n                        }\n\n                        if (!parameter.description.empty())\n''',
    'choice rendering close')
