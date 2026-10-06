+++
path = "/rendering/shading"
title = "Shading tab (user shader preview and contract)"
kind = "subsystem"
status = "stable"
summary = "The Shading tab's engine side: the HLSL contract a shader is written against (Shade() with // @param annotations), CPU-only program assembly (ShadingProgram), the preview renderer (shapes, lighting presets, backgrounds, OBJ mesh preview), material thumbnails, screenshot conversion and the shading.* RPC methods."
owner_module = "OrbitShading"
keywords = ["shading", "shader", "preview", "shade.hlsl", "param", "material thumbnail", "obj mesh", "shading rpc", "preview renderer", "screenshot"]
sources = [
  "engine/shading/include/orbit/shading/MaterialThumbnailCache.hpp",
  "engine/shading/include/orbit/shading/ObjMesh.hpp",
  "engine/shading/include/orbit/shading/ShaderPreviewRenderer.hpp",
  "engine/shading/include/orbit/shading/ShaderProgram.hpp",
  "engine/shading/include/orbit/shading/ShadingCapture.hpp",
  "engine/shading/include/orbit/shading/ShadingContract.hpp",
  "engine/shading/include/orbit/shading/ShadingRpc.hpp",
  "engine/shading/include/orbit/shading/ShadingWorkspace.hpp",
  "engine/shading/CMakeLists.txt",
]
symbols = ["MaterialThumbnailCache", "MeshVertex", "ShaderPreviewRenderer", "ShadingProgram", "FloatImage", "EnumEntry", "ShadingRpcHooks", "TreeNode"]
invariants = [
  "CPU-only assembly and compilation (ShadingProgram) is kept separate from the GPU pipeline (ShaderPreviewRenderer) so it can be unit tested and driven from RPC without a device.",
  "OBJ parsing is CPU only; the renderer uploads the result, so it is testable without a device.",
  "shading.* RPC methods throw on failure with error codes 1050 (invalid request) and 1051 (unexpected failure); shading.screenshot reports that no preview target is attached when none is.",
  "A ShadingShader holds only the Shade() function and is not a standalone HLSL stage (/authoring/content).",
]
related = ["/legacy/orbit-shading", "/authoring/content", "/editor/mcp-rpc"]
depends_on = ["/authoring/content", "/authoring/content-wic", "/foundation/core", "/foundation/math", "/foundation/rpc", "/rendering/rhi", "/rendering/shader-compiler"]
verify = [
  "ctest -R Orbit.Shading",
]
verified = "b0a0de7f"

[routes]
"shader contract, parameters, templates, preview" = "/legacy/orbit-shading"
"shading RPC methods" = "/legacy/orbit-shading/rpc-and-mcp-reference"
+++

The authoritative description of the contract, assets, templates and RPC reference is `docs/ORBIT_SHADING.md` (`/legacy/orbit-shading`).
