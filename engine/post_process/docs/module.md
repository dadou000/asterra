+++
path = "/rendering/post-process"
title = "Post-process (exposure, tone mapping, LUT, highlights, display resolve)"
kind = "subsystem"
status = "stable"
summary = "The display pipeline after lighting: luminance histogram metering, human eye adaptation (cd/m^2), exposure, tone mapping with a shoulder, highlight effects, display-referred 3D colour LUT grading, output transform and the final display resolve."
owner_module = "OrbitPostProcess"
keywords = ["post process", "tone mapping", "exposure", "histogram", "color lut", "lut", "grading", "display resolve", "output transform", "hdr", "highlight effects", "metering"]
sources = [
  "engine/post_process/include/orbit/post_process/AntiAliasing.hpp",
  "engine/post_process/include/orbit/post_process/ColorLut.hpp",
  "engine/post_process/include/orbit/post_process/DisplayResolve.hpp",
  "engine/post_process/include/orbit/post_process/HighlightEffects.hpp",
  "engine/post_process/include/orbit/post_process/HumanEyeAdaptation.hpp",
  "engine/post_process/include/orbit/post_process/HumanEyeAdaptationHotReload.hpp",
  "engine/post_process/include/orbit/post_process/LuminanceHistogram.hpp",
  "engine/post_process/include/orbit/post_process/OutputTransform.hpp",
  "engine/post_process/include/orbit/post_process/ToneMapping.hpp",
  "engine/post_process/CMakeLists.txt",
]
symbols = ["AntiAliasingRenderer", "ColorLutMetadata", "DisplayResolveSettings", "HighlightEffectsConfig", "HumanEyeAdaptationConfig", "HumanEyeAdaptationHotReloadInterface", "LuminanceHistogramConfig", "OutputDisplayCapabilities", "ToneMappingConfig"]
invariants = [
  "Anti-aliasing runs on the HDR scene colour after every scene pass and before exposure and tone mapping, only in the lit view. TAA jitters the camera by a sub-pixel rotation (so no scene pass knows about it) and reprojects its history from the jittered camera pair plus depth; any frame without usable history (first frame, over 2 km camera jump, lens change, resize) runs FXAA instead. Both work on a Karis-compressed copy of the colour.",
  "Physical/radiometric scene encoding happens before display resolve and must not depend on camera adaptation: view/presentation exposure is applied in the display stage.",
  "Tone mapping's shoulder begins at an exposed scene-linear value (default 1.0, keeping nominal reference white in the linear region) and approaches the peak headroom exponentially.",
  "Colour LUTs are display-referred: exposure and tone mapping are owned by the presentation pipeline, the LUT pass only grades display-linear data and rejects scene-referred or shaped LUT assets before GPU upload; a 3D LUT is stored as N horizontal NxN slices in one 2D texture with explicit blue-slice interpolation so correction is still trilinear.",
  "Highlight thresholds are in exposed scene-linear units and the photopic ceiling is supplied separately so authored calibration stays in the eye state.",
  "Luminance histogram visualisation runs after display/LUT output and never feeds back into metering or physical lighting; its GPU readback ABI uses only scalar 32-bit fields so HLSL byte-address offsets stay explicit.",
]
related = ["/rendering/lighting/eye-adaptation", "/rendering/lighting", "/tools/eye-adaptation-module"]
depends_on = ["/foundation/core", "/rendering/rhi", "/rendering/shader-compiler"]
used_by = ["/apps/studio", "/editor/studio-ui"]
verify = [
  "ctest -R Orbit.ColorLut",
  "ctest -R Orbit.LuminanceHistogram",
  "ctest -R Orbit.HumanEyeAdaptation",
  "ctest -R Orbit.HighlightEffects",
  "ctest -R Orbit.ToneMapping",
  "ctest -R Orbit.OutputTransform",
]
verified = "b0a0de7f"
+++


