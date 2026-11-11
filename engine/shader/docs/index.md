+++
path = "/rendering/shader-compiler"
title = "Shader compiler interface and DXC backend"
kind = "subsystem"
status = "stable"
summary = "shader::Compiler turns CompileRequests (stage, source, entry point, flags) into Binary SPIR-V; the DXC backend (engine/shader/dxc) compiles the engine's embedded HLSL for Vulkan 1.3 and memoises compiles per process."
owner_module = "OrbitShader"
keywords = ["shader", "hlsl", "dxc", "spirv", "compile", "compiler", "memoise", "vulkan 1.3", "shader model"]
sources = [
  "engine/shader/include/orbit/shader/ShaderCompiler.hpp",
  "engine/shader/dxc/include/orbit/shader/dxc/DxcShaderCompiler.hpp",
  "engine/shader/dxc/CMakeLists.txt",
  "engine/shader/CMakeLists.txt",
]
symbols = ["Compiler", "CompileRequest", "Binary", "DxcShaderCompiler"]
invariants = [
  "The DXC backend replaces the legacy D3DCompile (FXC, shader model 5.1) path, which cannot target SPIR-V at all.",
  "DxcShaderCompiler memoises results per process keyed by stage, shader model, flags, entry point and source text, so rebuilding a renderer or pipeline from unchanged HLSL does not recompile; edited sources hash differently, so live shader editing is unaffected (/rendering/terrain/clipmaps/rebuild-hitches).",
  "Embedded HLSL compiles at startup, so a failed shader leaves the running generation alive (/rules/hot-iteration).",
]
related = ["/rendering/terrain/clipmaps/rebuild-hitches", "/rendering/rhi", "/rules/hot-iteration"]
depends_on = ["/foundation/core"]
used_by = ["/celestial/compact-render", "/celestial/far-render", "/celestial/globe", "/celestial/magnetosphere-render", "/celestial/rings", "/editor/ui-toolkit", "/rendering/lighting/radiance-cache", "/rendering/post-process", "/rendering/render-view", "/rendering/shading", "/rendering/terrain/gpu-passes", "/rendering/volumes/render", "/rendering/volumes/solver", "/tools/eye-adaptation-module"]
verify = [
  "ctest -R Orbit.ShaderCompiler",
]
verified = "b0a0de7f"
+++


