+++
path = "/rendering/render-view"
title = "Render views and captures"
kind = "subsystem"
status = "stable"
summary = "RenderView is one viewport's render target set and camera (CameraState, ViewRay, imported targets, composite renderer); Capture reads the final display target back to memory, BMP or PNG for screenshots and reports."
owner_module = "OrbitRenderView"
keywords = ["render view", "viewport", "camera state", "view ray", "capture", "screenshot", "png", "bmp", "display target", "picking"]
sources = [
  "engine/render_view/include/orbit/render_view/Capture.hpp",
  "engine/render_view/include/orbit/render_view/RenderView.hpp",
  "engine/render_view/CMakeLists.txt",
]
symbols = ["CaptureResult", "CameraState"]
invariants = [
  "Scene colour is the renderer-facing target; Display is a distinct presentation target so post-process passes never sample the texture they write.",
  "A completed view frame leaves colour/picking sampleable and depth in DepthWrite; new graphs import those exact states.",
  "Capture is synchronous and returns the exposed, tone-mapped and graded RGBA8 image Studio presents: the caller must ensure rendering to the view has completed (Studio does this after waiting its previous-frame fence).",
  "PNG capture drops the display alpha and needs the Windows imaging codec (it throws elsewhere); large captures stream in row chunks.",
]
related = ["/rendering/render-graph", "/editor/viewport"]
depends_on = ["/foundation/core", "/foundation/frames", "/foundation/math", "/rendering/lighting", "/rendering/render-graph", "/rendering/rhi"]
used_by = ["/celestial/compact-render", "/celestial/far-render", "/celestial/globe", "/celestial/magnetosphere-render", "/celestial/rings", "/rendering/volumes/render"]
verify = [
  "ctest -R Orbit.ViewRay",
  "ctest -R Orbit.CapturePng",
]
verified = "b0a0de7f"
+++


