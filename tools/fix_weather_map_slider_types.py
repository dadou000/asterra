from pathlib import Path

p = Path("scripts/ui/weather_map.gd")
s = p.read_text(encoding="utf-8")
old = '''\tvar t := clampf((global_x - rect.position.x) / rect.size.x, 0.0, 1.0)\n\tvar raw := lerpf(_speed_slider.min_value, _speed_slider.max_value, t)\n\tvar snapped := round(raw / _speed_slider.step) * _speed_slider.step\n'''
new = '''\tvar t: float = clampf((global_x - rect.position.x) / rect.size.x, 0.0, 1.0)\n\tvar raw: float = lerpf(float(_speed_slider.min_value), float(_speed_slider.max_value), t)\n\tvar slider_step: float = float(_speed_slider.step)\n\tvar snapped: float = roundf(raw / slider_step) * slider_step\n'''
if old not in s:
    raise SystemExit("target slider block not found")
s = s.replace(old, new, 1)
p.write_text(s, encoding="utf-8")
