from pathlib import Path

path = Path("native/weather/src/weather_native_oklahoma.cpp")
text = path.read_text(encoding="utf-8")

start_marker = r"\t// Single-owner sparse cloud microphysics."
end_marker = "\nvoid WeatherNative::vertical_pass"
start = text.find(start_marker)
end = text.find(end_marker, start)
if start < 0 or end < 0:
    raise SystemExit("escaped cloud microphysics block not found")

block = text[start:end]
block = block.replace(r"\t", "\t").replace(r"\n", "\n")
text = text[:start] + block + text[end:]
path.write_text(text, encoding="utf-8")
print("normalized escaped cloud microphysics block")
