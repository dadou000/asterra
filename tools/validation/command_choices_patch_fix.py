from pathlib import Path

path = Path('tools/validation/command_choices_patch.py')
text = path.read_text(encoding='utf-8')
old = """    '''                        switch (parameter.kind)\\n                        {\\n                        case commands::CommandValueKind::Boolean:\\n''',
"""
new = """    '''                        auto found = quickCreateArguments_.find(parameter.name);\\n                        if (found == quickCreateArguments_.end())\\n                        {\\n                            continue;\\n                        }\\n\\n                        switch (parameter.kind)\\n                        {\\n                        case commands::CommandValueKind::Boolean:\\n''',
"""
if text.count(old) != 1:
    raise SystemExit(f'choice rendering script anchor count {text.count(old)}')
text = text.replace(old, new, 1)
old_new = """    '''                        if (!parameter.choices.empty())\\n                        {\\n                            std::vector<std::string_view> choiceLabels;\\n"""
new_new = """    '''                        auto found = quickCreateArguments_.find(parameter.name);\\n                        if (found == quickCreateArguments_.end())\\n                        {\\n                            continue;\\n                        }\\n\\n                        if (!parameter.choices.empty())\\n                        {\\n                            std::vector<std::string_view> choiceLabels;\\n"""
if text.count(old_new) != 1:
    raise SystemExit(f'choice rendering replacement anchor count {text.count(old_new)}')
text = text.replace(old_new, new_new, 1)
path.write_text(text, encoding='utf-8')
