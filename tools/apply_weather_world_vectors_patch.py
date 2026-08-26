from pathlib import Path

p = Path("scripts/ui/weather_map.gd")
s = p.read_text(encoding="utf-8")

literal_tabs = s.count("\\t")
if literal_tabs < 20:
    raise SystemExit(f"expected generated literal tab escapes, found only {literal_tabs}")
s = s.replace("\\t", "\t")

old_tip = "Discrete weather warp: pause, 0.25×, 0.5×, 1×, then powers of two to 8192×. Dragging previews and commits on release."
new_tip = "Discrete weather warp: pause, 0.25×, 0.5×, 1×, then powers of two to 8192×. Dragging applies continuously, even outside the slider."
if old_tip not in s:
    raise SystemExit("warp tooltip source text not found")
s = s.replace(old_tip, new_tip, 1)

p.write_text(s, encoding="utf-8")
print(f"fixed {literal_tabs} generated tab escapes")
