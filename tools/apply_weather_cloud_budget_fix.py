from pathlib import Path

path = Path("native/weather/src/weather_native_oklahoma.cpp")
text = path.read_text(encoding="utf-8")

old = "\t\t}\n\t}\n\t}\n}\n\nvoid WeatherNative::vertical_pass"
new = "\t\t}\n\t}\n}\n\nvoid WeatherNative::vertical_pass"
if old not in text:
    raise SystemExit("stale cloud closure brace pattern not found")
text = text.replace(old, new, 1)
path.write_text(text, encoding="utf-8")
print("removed stale cloud closure brace")
