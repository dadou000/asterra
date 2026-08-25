from pathlib import Path

code = Path("tools/apply_weather_polar_patch.py").read_text(encoding="utf-8")
start = code.index('h = replace_exact(\n    h,\n    "target_v_lane[k] = amplitude * 0.78f * std::sin(phase3)')
end = code.index('s = prefix + h', start)
replacement = '''h = replace_exact(
    h,
    "target_v_lane[k] = amplitude * 0.78f * std::sin(phase3)",
    "target_v_lane[k] = polar_forcing_taper * (amplitude * 0.78f * std::sin(phase3)",
    1, "runtime polar-v first line")
h = replace_exact(
    h,
    "+ amplitude * (0.26f * std::cos(phase5) + 0.14f * std::sin(phase7));",
    "+ amplitude * (0.26f * std::cos(phase5) + 0.14f * std::sin(phase7)));",
    1, "runtime polar-v closing line")
'''
code = code[:start] + replacement + code[end:]
exec(compile(code, "tools/apply_weather_polar_patch.py", "exec"))
