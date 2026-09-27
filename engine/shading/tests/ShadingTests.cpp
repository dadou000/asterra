#include <orbit/content/ContentService.hpp>
#include <orbit/core/StrongId.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/shading/ShaderProgram.hpp>
#include <orbit/shading/ShadingCapture.hpp>
#include <orbit/shading/ShadingContract.hpp>
#include <orbit/shading/ShadingRpc.hpp>
#include <orbit/shading/ShadingWorkspace.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <source_location>
#include <string>
#include <vector>

namespace
{
using namespace orbit;

void Check(
    const bool condition,
    const std::source_location location = std::source_location::current())
{
    if (!condition)
    {
        std::cerr << "Shading test failed at " << location.file_name() << ':'
                  << location.line() << '\n';
        std::exit(1);
    }
}

template <typename Fn>
[[nodiscard]] bool Throws(Fn&& fn)
{
    try
    {
        fn();
    }
    catch (const std::exception&)
    {
        return true;
    }
    return false;
}

void TestEnumsAndParameters()
{
    Check(shading::ParseShape("sphere") == shading::PreviewShape::Sphere);
    Check(shading::ParseShape("plane") == shading::PreviewShape::Plane);
    Check(!shading::ParseShape("torus").has_value());
    Check(shading::ParseLightingPreset("space") == shading::LightingPreset::Space);
    Check(shading::ParseBackground("checker") == shading::PreviewBackground::Checker);
    Check(shading::ShapeKey(shading::PreviewShape::Cube) == "cube");

    const auto layout = shading::ParseShaderParameters(
        "// header\n"
        "// @param tint  color3 0.8 0.7 0.6\n"
        "// @param rough float  0.5 | 0 1\n"
        "// @param uvs   float2 2 3\n");
    Check(layout.problems.empty());
    Check(layout.parameters.size() == 3);
    Check(layout.parameters[0].name == "tint");
    Check(layout.parameters[0].isColor && layout.parameters[0].components == 3);
    Check(layout.parameters[0].offset == 0);
    Check(layout.parameters[1].offset == 3);
    Check(layout.parameters[1].hasRange && layout.parameters[1].maximum == 1.0);
    Check(layout.parameters[2].offset == 4);
    Check(layout.totalFloats == 6);
    Check(layout.parameters[2].defaults[1] == 3.0);

    // Problems are reported with their line number and do not abort parsing.
    const auto bad = shading::ParseShaderParameters(
        "// @param a float 1\n"
        "// @param a float 2\n"
        "// @param b vec9 1\n"
        "// @param c float2 1\n"
        "// @param d float 1 | 5 1\n"
        "// @param 9x float 1\n");
    Check(bad.parameters.size() == 1);
    Check(bad.problems.size() == 5);
    Check(bad.problems[0].starts_with("line 2:"));

    // The push-constant budget is enforced with a clear message.
    const auto over = shading::ParseShaderParameters(
        "// @param a float4 0 0 0 0\n"
        "// @param b float4 0 0 0 0\n"
        "// @param c float  0\n");
    Check(over.parameters.size() == 2);
    Check(over.problems.size() == 1);
    Check(over.problems[0].find("at most 8") != std::string::npos);
}

void TestTextureParametersAndDisplacement(const shader::Compiler& compiler)
{
    // texture2d takes a single path token in place of numeric defaults, and
    // doesn't consume the scalar push-constant budget.
    const auto layout = shading::ParseShaderParameters(
        "// @param albedo texture2d Content/Textures/Rock/Albedo.jpg\n"
        "// @param tint   color3 1 1 1\n");
    Check(layout.problems.empty());
    Check(layout.parameters.size() == 1 && layout.totalFloats == 3);
    Check(layout.textures.size() == 1);
    Check(layout.textures[0].name == "albedo");
    Check(layout.textures[0].defaultPath == "Content/Textures/Rock/Albedo.jpg");
    Check(layout.textures[0].slot == 0);
    Check(layout.FindTexture("albedo") != nullptr);
    Check(!layout.HasDisplacement());

    // Two textures fit; a third does not, and is reported like the scalar
    // budget is.
    const auto twoTextures = shading::ParseShaderParameters(
        "// @param a texture2d a.jpg\n"
        "// @param b texture2d b.jpg\n"
        "// @param c texture2d c.jpg\n");
    Check(twoTextures.textures.size() == 2);
    Check(twoTextures.textures[1].slot == 1);
    Check(twoTextures.problems.size() == 1);
    Check(twoTextures.problems[0].find("at most 2 texture2d") != std::string::npos);

    // A name can't be both a texture and a scalar parameter.
    const auto collision = shading::ParseShaderParameters(
        "// @param a texture2d a.jpg\n"
        "// @param a float 1\n");
    Check(collision.textures.size() == 1 && collision.parameters.empty());
    Check(collision.problems.size() == 1);
    Check(collision.problems[0].find("declared twice") != std::string::npos);

    // `height` (texture2d) + `displacement` (float) together, and only
    // together, enable the reserved displacement convention.
    const auto heightOnly = shading::ParseShaderParameters(
        "// @param height texture2d Height.jpg\n");
    Check(!heightOnly.HasDisplacement());

    const auto displaced = shading::ParseShaderParameters(
        "// @param height       texture2d Content/Textures/Rock/Height.jpg\n"
        "// @param displacement float 0.15 | 0 1\n");
    Check(displaced.HasDisplacement());

    // A shader with a plain (non-reserved-name) texture compiles as an
    // ordinary pixel-only program: no vertex binary.
    const auto textured = shading::CompileShadingProgram(
        compiler, "textured.shade.hlsl",
        "// @param albedo texture2d Content/Textures/Rock/Albedo.jpg\n"
        "float4 Shade(OrbitSurface s, OrbitLighting l)\n"
        "{\n"
        "    return float4(Sample_albedo(s.uv).rgb, 1.0);\n"
        "}\n");
    if (!textured.ok)
    {
        std::cerr << "Textured program failed:\n" << textured.diagnostics << '\n';
    }
    Check(textured.ok);
    Check(!textured.pixel.bytecode.empty());
    Check(textured.vertex.bytecode.empty());

    // The height+displacement convention compiles a matching vertex binary
    // too, and BuildObjectVertexSource can be exercised directly.
    const auto displacedProgram = shading::CompileShadingProgram(
        compiler, "rock_ground.shade.hlsl",
        "// @param height       texture2d Content/Textures/Rock/Height.jpg\n"
        "// @param displacement float 0.15 | 0 1\n"
        "// @param albedo       texture2d Content/Textures/Rock/Albedo.jpg\n"
        "float4 Shade(OrbitSurface s, OrbitLighting l)\n"
        "{\n"
        "    return float4(Sample_albedo(s.uv).rgb, 1.0);\n"
        "}\n");
    if (!displacedProgram.ok)
    {
        std::cerr << "Displaced program failed:\n" << displacedProgram.diagnostics << '\n';
    }
    Check(displacedProgram.ok);
    Check(!displacedProgram.pixel.bytecode.empty());
    Check(!displacedProgram.vertex.bytecode.empty());

    const std::string vertexSource =
        shading::BuildObjectVertexSource(displacedProgram.layout);
    Check(vertexSource.find("SampleLevel_height") != std::string::npos);
    Check(vertexSource.find("Param_displacement") != std::string::npos);
    // Only the reserved texture is declared in the vertex stage, not every
    // texture2d parameter the shader happens to have.
    Check(vertexSource.find("SampleLevel_albedo") == std::string::npos);

    // BuildObjectVertexSource refuses a layout without the convention rather
    // than emitting HLSL that references an undeclared texture.
    Check(Throws([&] { (void)shading::BuildObjectVertexSource(layout); }));
}

void TestTemplatesCompile(const shader::Compiler& compiler)
{
    // Every shipped template must compile against the contract; this is what
    // validates the HLSL prelude and the parameter accessors end to end.
    for (const auto name : shading::ShaderTemplateNames())
    {
        const auto source = shading::ShaderTemplateSource(name);
        Check(source.has_value());

        const auto program = shading::CompileShadingProgram(
            compiler, std::string(name) + ".shade.hlsl", *source);
        if (!program.ok)
        {
            std::cerr << "Template '" << name << "' failed:\n"
                      << program.diagnostics << '\n';
        }
        Check(program.ok);
        Check(!program.pixel.bytecode.empty());
        Check(program.diagnostics.empty());
    }

    // Fixed engine stages compile too.
    Check(!compiler.Compile({.source = shading::PreviewVertexSource(),
                             .entryPoint = "main",
                             .stage = shader::Stage::Vertex})
               .bytecode.empty());
    Check(!compiler.Compile({.source = shading::BackgroundVertexSource(),
                             .entryPoint = "main",
                             .stage = shader::Stage::Vertex})
               .bytecode.empty());
    const std::string background = shading::BackgroundPixelSource();
    Check(!compiler.Compile({.source = background,
                             .entryPoint = "main",
                             .stage = shader::Stage::Pixel})
               .bytecode.empty());
}

void TestDiagnosticsPointAtUserFile(const shader::Compiler& compiler)
{
    // Line 3 is broken; the message must name the asset and line 3 - not a
    // line offset by the engine prelude.
    const auto program = shading::CompileShadingProgram(
        compiler,
        "Broken.shade.hlsl",
        "// line 1\n"
        "float4 Shade(OrbitSurface s, OrbitLighting l)\n"
        "{ return notAFunction(s); }\n");

    Check(!program.ok);
    Check(program.diagnostics.find("Broken.shade.hlsl") != std::string::npos);
    Check(program.diagnostics.find("Broken.shade.hlsl:3") != std::string::npos);

    // A parameter problem fails before invoking the compiler at all.
    const auto badParam = shading::CompileShadingProgram(
        compiler,
        "P.shade.hlsl",
        "// @param x nonsense 1\nfloat4 Shade(OrbitSurface s, OrbitLighting l) { return 1; }\n");
    Check(!badParam.ok);
    Check(badParam.diagnostics.find("line 1") != std::string::npos);
}

std::filesystem::path MakeProject()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("orbit-shading-" + core::StrongId<struct ShadingTag>::Random().ToString());
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Content");
    return root;
}

// A minimal uncompressed 24bpp BMP, small enough to hand-encode without a
// PNG/JPEG library -- content_wic (WIC) decodes it exactly like the .jpg
// files an artist actually imports.
void WriteTestBmp(const std::filesystem::path& path, const u32 width, const u32 height)
{
    const u32 rowBytes = ((width * 3U + 3U) / 4U) * 4U;
    const u32 pixelBytes = rowBytes * height;
    const u32 fileSize = 54U + pixelBytes;

    std::vector<u8> bytes(fileSize, 0U);
    const auto put16 = [&](const std::size_t offset, const u16 value)
    { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
    const auto put32 = [&](const std::size_t offset, const u32 value)
    { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };

    bytes[0] = 'B';
    bytes[1] = 'M';
    put32(2, fileSize);
    put32(10, 54U); // pixel data offset
    put32(14, 40U); // DIB header size
    put32(18, width);
    put32(22, height); // positive: bottom-up rows
    put16(26, 1U);      // planes
    put16(28, 24U);     // bits per pixel
    put32(30, 0U);      // BI_RGB, uncompressed

    for (u32 row = 0U; row < height; ++row)
    {
        for (u32 column = 0U; column < width; ++column)
        {
            const std::size_t at = 54U + row * rowBytes + column * 3U;
            // B, G, R -- a different colour per pixel so a real decode (not a
            // solid-fill accident) is what makes the test pass.
            bytes[at + 0] = static_cast<u8>(row * 64U);
            bytes[at + 1] = static_cast<u8>(column * 64U);
            bytes[at + 2] = 200U;
        }
    }

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

void TestWorkspace(const shader::Compiler& compiler)
{
    const auto root = MakeProject();
    content::ContentService content(root);
    content.Scan();
    shading::ShadingWorkspace workspace(content, &compiler);

    // Organise: folders and a shader from a template (content-relative paths
    // are accepted).
    workspace.CreateFolder("Shading");
    workspace.CreateFolder("Shading/Lunar");
    const auto shaderPath =
        workspace.CreateShader("Shading/Lunar", "Regolith", "lunar_regolith");
    Check(shaderPath.generic_string() ==
          "Content/Shading/Lunar/Regolith.shade.hlsl");
    Check(Throws([&] { (void)workspace.CreateShader("Shading/Lunar", "Regolith", "lit"); }));
    Check(Throws([&] { (void)workspace.CreateShader("Shading/Lunar", "X", "no_such_template"); }));

    // The tree shows folders first and marks previewable kinds.
    const auto tree = workspace.Tree();
    Check(tree.name == "Content" && tree.folder);
    Check(tree.children.size() == 1 && tree.children[0].name == "Shading");
    const auto& lunar = tree.children[0].children.at(0);
    Check(lunar.name == "Lunar" && lunar.folder);
    Check(lunar.children.size() == 1 && lunar.children[0].previewable);

    // Selecting compiles and exposes the declared parameters.
    workspace.Select(shaderPath);
    Check(workspace.Status().compiled);
    Check(workspace.Program() != nullptr);
    const auto revisionAfterSelect = workspace.Status().programRevision;
    Check(workspace.Parameters().size() == 5);
    Check(workspace.Parameters()[2].declaration.name == "craters");

    // A broken edit keeps the last good program live and reports the error.
    workspace.SetEditorText(workspace.EditorText() + "\nfloat4 Oops() { return nope; }\n");
    Check(workspace.Status().dirty);
    workspace.Recompile();
    Check(!workspace.Status().compiled);
    Check(!workspace.Status().diagnostics.empty());
    Check(workspace.Program() != nullptr);
    Check(workspace.Status().programRevision == revisionAfterSelect);

    // Save the fix: compiles, bumps the revision, clears dirty.
    workspace.Revert();
    Check(!workspace.Status().dirty && workspace.Status().compiled);
    workspace.SetEditorText(
        std::string(*shading::ShaderTemplateSource("lit")));
    workspace.Save();
    Check(workspace.Status().compiled);
    Check(workspace.Status().programRevision > revisionAfterSelect);
    Check(!workspace.Status().dirty);
    Check(workspace.Parameters().size() == 3);

    // Hot path: an external editor saves the file. Only ContentService is
    // touched here (as the watcher would), then the next frame's Update()
    // reloads and recompiles with no explicit call.
    const auto beforeExternal = workspace.Status().programRevision;
    content.WriteText(
        shaderPath,
        "// @param glow float 1 | 0 4\n"
        "float4 Shade(OrbitSurface s, OrbitLighting l) { return float4(Param_glow().xxx, 1); }\n");
    workspace.Update(0.016);
    Check(workspace.Status().compiled);
    Check(workspace.Status().programRevision > beforeExternal);
    Check(workspace.Parameters().size() == 1);
    Check(workspace.EditorText().find("Param_glow") != std::string::npos);

    // ...but unsaved edits are never clobbered by an external change.
    workspace.SetEditorText("// my unsaved edit\n" + workspace.EditorText());
    content.WriteText(shaderPath, "float4 Shade(OrbitSurface s, OrbitLighting l) { return 0; }\n");
    workspace.Update(0.016);
    Check(workspace.Status().changedOnDisk);
    Check(workspace.EditorText().starts_with("// my unsaved edit"));
    workspace.Revert();
    Check(!workspace.Status().changedOnDisk);

    // Live compile is debounced: nothing until the pause elapses.
    workspace.SetEditorText(
        "// @param k float 2\nfloat4 Shade(OrbitSurface s, OrbitLighting l) { return Param_k(); }\n");
    const auto beforeLive = workspace.Status().programRevision;
    workspace.Update(0.05);
    Check(workspace.Status().programRevision == beforeLive);
    workspace.Update(0.5);
    Check(workspace.Status().compiled);
    Check(workspace.Status().programRevision > beforeLive);
    Check(workspace.Status().dirty);

    // Shader materials: created bound to the shader, edited parameter values
    // persist to the asset and are packed for the preview in declaration order.
    workspace.Revert();
    workspace.SetEditorText(std::string(*shading::ShaderTemplateSource("lit")));
    workspace.Save();

    const auto materialPath =
        workspace.CreateShaderMaterial("Shading/Lunar", "Dust", shaderPath);
    workspace.Select(materialPath);
    Check(workspace.Status().material == materialPath);
    Check(workspace.Status().shader == shaderPath);
    Check(workspace.Status().compiled);

    const std::vector<f64> tint{0.1, 0.2, 0.3};
    workspace.SetParameter("tint", tint);
    Check(workspace.Parameters()[0].overridden);
    Check(workspace.PackedParameters()[1] == static_cast<f32>(0.2));
    // Untouched parameters keep the shader's declared default.
    Check(workspace.PackedParameters()[3] == static_cast<f32>(0.55));
    Check(Throws([&] { workspace.SetParameter("tint", std::vector<f64>{1.0}); }));
    Check(Throws([&] { workspace.SetParameter("nope", std::vector<f64>{1.0}); }));

    const auto* materialRecord = content.FindByPath(materialPath);
    Check(materialRecord != nullptr && materialRecord->shaderMaterial.has_value());
    Check(materialRecord->shaderMaterial->parameters.size() == 1);
    Check(materialRecord->shaderMaterial->parameters[0].values[2] == 0.3);

    workspace.ResetParameter("tint");
    Check(!workspace.Parameters()[0].overridden);
    Check(content.FindByPath(materialPath)->shaderMaterial->parameters.empty());

    // Rename / move keep the selection pointing at the same entry.
    const auto renamed = workspace.Rename(materialPath, "DustB.orbitshadermaterial");
    Check(workspace.Selected() == renamed);
    workspace.CreateFolder("Shading/Archive");
    const auto moved = workspace.Move(renamed, "Shading/Archive");
    Check(workspace.Selected() == moved);
    Check(Throws([&] { (void)workspace.Move("Shading", "Shading/Archive"); }));

    // Trash clears the selection and is reversible (lands under .orbit/Trash).
    const auto trashed = workspace.Trash(moved);
    Check(workspace.Selected().empty());
    Check(std::filesystem::is_regular_file(root / trashed));

    // Escaping the Content mount is refused.
    Check(Throws([&] { workspace.CreateFolder("../Escape"); }));

    std::filesystem::remove_all(root);
}

void TestRpc(const shader::Compiler& compiler)
{
    const auto root = MakeProject();
    content::ContentService content(root);
    content.Scan();
    shading::ShadingWorkspace workspace(content, &compiler);

    rpc::Dispatcher dispatcher;
    shading::RegisterShadingRpc(dispatcher, workspace);

    const auto call = [&](const std::string& method, const std::string& params)
    {
        const auto response = dispatcher.Dispatch(
            R"({"jsonrpc":"2.0","id":1,"method":")" + method +
            R"(","params":)" + params + "}");
        Check(response.has_value());
        return *response;
    };
    // A JSON-RPC failure carries an error *object*; status results have their
    // own harmless "error":"" text fields.
    const auto ok = [](const std::string& response)
    { return response.find("\"error\":{") == std::string::npos; };

    // Every documented method is registered.
    for (const auto& name : shading::ShadingRpcMethodNames())
    {
        bool found = false;
        for (const auto& descriptor : dispatcher.Catalog())
        {
            found |= descriptor.name == name;
        }
        Check(found);
    }

    Check(ok(call("shading.options", "{}")));
    Check(ok(call("shading.folder_create", R"({"path":"Shading"})")));
    Check(ok(call("shading.shader_create",
                  R"({"folder":"Shading","name":"Mirror","template":"unlit"})")));
    const auto written = call(
        "shading.source_write",
        R"({"path":"Shading/Mirror.shade.hlsl","text":"// @param k float 1\nfloat4 Shade(OrbitSurface s, OrbitLighting l) { return Param_k(); }\n"})");
    Check(ok(written));
    Check(written.find("\"compiled\":true") != std::string::npos);

    // Compile errors are a normal result (diagnostics), not an RPC failure.
    const auto broken = call(
        "shading.source_write",
        R"({"path":"Shading/Mirror.shade.hlsl","text":"float4 Shade(OrbitSurface s, OrbitLighting l) { return x; }\n"})");
    Check(ok(broken));
    Check(broken.find("\"compiled\":false") != std::string::npos);
    Check(broken.find("Mirror.shade.hlsl") != std::string::npos);

    // texture2d parameters over RPC: declared, listed in status, set by a
    // path string (not a number), reported loaded with real dimensions, and
    // rejected with a clear message if given a number instead.
    std::filesystem::create_directories(root / "Content" / "Textures");
    WriteTestBmp(root / "Content" / "Textures" / "Rock.bmp", 3U, 2U);
    content.Scan();

    const auto texturedWritten = call(
        "shading.source_write",
        R"({"path":"Shading/Mirror.shade.hlsl","text":"// @param albedo texture2d Content/Textures/Rock.bmp\nfloat4 Shade(OrbitSurface s, OrbitLighting l) { return Sample_albedo(s.uv); }\n"})");
    Check(ok(texturedWritten));
    Check(texturedWritten.find("\"compiled\":true") != std::string::npos);
    Check(texturedWritten.find("\"name\":\"albedo\"") != std::string::npos);
    Check(texturedWritten.find("\"loaded\":true") != std::string::npos);
    Check(texturedWritten.find("\"width\":3") != std::string::npos);

    const auto rejectedNumber =
        call("shading.param_set", R"({"name":"albedo","value":1.0})");
    Check(rejectedNumber.find("-32602") != std::string::npos);
    Check(rejectedNumber.find("texture2d") != std::string::npos);

    WriteTestBmp(root / "Content" / "Textures" / "Rock2.bmp", 5U, 4U);
    content.Scan();
    const auto retextured = call(
        "shading.param_set",
        R"({"name":"albedo","value":"Content/Textures/Rock2.bmp"})");
    Check(ok(retextured));
    Check(retextured.find("\"width\":5") != std::string::npos);
    Check(retextured.find("\"overridden\":true") != std::string::npos);

    const auto textureReset = call("shading.param_reset", R"({"name":"albedo"})");
    Check(ok(textureReset));
    Check(textureReset.find("\"width\":3") != std::string::npos);

    Check(ok(call("shading.preview_set",
                  R"({"shape":"cube","lighting":"space","background":"checker","exposure":2,"camera":{"yaw_degrees":30,"distance":5}})")));
    Check(workspace.Preview().shape == shading::PreviewShape::Cube);
    Check(workspace.Preview().lighting == shading::LightingPreset::Space);
    Check(workspace.Preview().exposure == 2.0F);

    // Invalid requests map onto code 1050 with a useful message.
    const auto badShape = call("shading.preview_set", R"({"shape":"torus"})");
    Check(badShape.find("1050") != std::string::npos);
    Check(badShape.find("torus") != std::string::npos);
    Check(call("shading.source_write", R"({"path":"Shading/x.txt","text":"a"})")
              .find("1050") != std::string::npos);
    Check(call("shading.folder_create", R"({"path":"../Escape"})").find("1050") !=
          std::string::npos);
    Check(call("shading.screenshot", R"({"path":"x.bmp"})").find("1051") !=
          std::string::npos);

    std::filesystem::remove_all(root);
}
constexpr const char* kCubeObj =
    "# unit cube, quads, negative indices on the last face\n"
    "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\n"
    "v -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
    "f 1 2 3 4\nf 5 8 7 6\nf 1 5 6 2\nf 2 6 7 3\nf 3 7 8 4\nf -4 -1 -5 -8\n";

void TestObjParser()
{
    const auto cube = shading::ParseObj(kCubeObj);
    // 6 quads fan into 12 triangles; corners are shared per (v, vt, vn).
    Check(cube.TriangleCount() == 12);
    Check(cube.sourceFaces == 6);
    Check(!cube.hadNormals && !cube.hadUvs);
    Check(cube.vertices.size() == 8);

    // Fitted to the preview: centred and inside a unit sphere.
    Check(std::abs(cube.sourceRadius - std::sqrt(3.0F)) < 1e-4F);
    float maxLength = 0.0F;
    for (const auto& vertex : cube.vertices)
    {
        const float length = std::sqrt(
            vertex.position[0] * vertex.position[0] +
            vertex.position[1] * vertex.position[1] +
            vertex.position[2] * vertex.position[2]);
        maxLength = std::max(maxLength, length);
        // Generated normals are unit length.
        const float n = std::sqrt(
            vertex.normal[0] * vertex.normal[0] +
            vertex.normal[1] * vertex.normal[1] +
            vertex.normal[2] * vertex.normal[2]);
        Check(std::abs(n - 1.0F) < 1e-4F);
    }
    Check(std::abs(maxLength - 1.0F) < 1e-4F);

    // An off-centre, large model is recentred and scaled.
    const auto shifted = shading::ParseObj(
        "v 100 100 100\nv 110 100 100\nv 100 110 100\nf 1 2 3\n");
    float centreX = 0.0F;
    for (const auto& vertex : shifted.vertices)
    {
        centreX += vertex.position[0];
        Check(std::abs(vertex.position[0]) <= 1.0001F);
    }
    Check(std::abs(centreX / 3.0F) < 0.6F);

    // Explicit normals and UVs are honoured, and vertices split on differing
    // texcoords.
    const auto textured = shading::ParseObj(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\nvt 1 1\nvn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\nf 2/2/1 4/4/1 3/3/1\n");
    Check(textured.hadNormals && textured.hadUvs);
    Check(textured.vertices.size() == 4);
    Check(textured.vertices[0].normal[2] == 1.0F);

    // "v//vn" (no texcoord) parses and gets generated UVs.
    const auto noUv = shading::ParseObj(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n");
    Check(noUv.hadNormals && !noUv.hadUvs);

    // Malformed input names the line.
    const auto fails = [](const std::string& text, const std::string& contains)
    {
        try
        {
            (void)shading::ParseObj(text);
        }
        catch (const std::runtime_error& error)
        {
            return std::string(error.what()).find(contains) != std::string::npos;
        }
        return false;
    };
    Check(fails("v 0 0 0\n", "no faces"));
    Check(fails("v 0 0 0\nv 1 0 0\nf 1 2 9\n", "line 3"));
    Check(fails("v 0 0 0\nv 1 0 0\nf 1 2 9\n", "out of range"));
    Check(fails("v 0 nan 0\n", "line 1"));
    Check(fails("v 0 0\n", "line 1"));
    Check(fails("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 0\n", "line 4"));
}

void TestTextureWorkspace(const shader::Compiler& compiler)
{
    const auto root = MakeProject();
    content::ContentService content(root);
    content.Scan();
    shading::ShadingWorkspace workspace(content, &compiler);

    std::filesystem::create_directories(root / "Content" / "Textures");
    WriteTestBmp(root / "Content" / "Textures" / "Albedo.bmp", 4U, 3U);
    WriteTestBmp(root / "Content" / "Textures" / "Height.bmp", 2U, 2U);
    content.Scan();

    workspace.CreateFolder("Ground");
    workspace.WriteSource(
        "Ground/Rock.shade.hlsl",
        "// @param albedo       texture2d Content/Textures/Albedo.bmp\n"
        "// @param height       texture2d Content/Textures/Height.bmp\n"
        "// @param displacement float 0.15 | 0 1\n"
        "float4 Shade(OrbitSurface s, OrbitLighting l)\n"
        "{\n"
        "    return float4(Sample_albedo(s.uv).rgb, 1.0);\n"
        "}\n");
    Check(workspace.Status().compiled);
    Check(workspace.Program()->layout.HasDisplacement());
    Check(!workspace.Program()->vertex.bytecode.empty());

    // Both texture2d parameters resolved to their declared defaults and
    // decoded without SetTextureParameter ever being called.
    {
        const auto textures = workspace.TextureParameters();
        Check(textures.size() == 2);
        Check(textures[0].declaration.name == "albedo" &&
              textures[0].declaration.slot == 0);
        Check(textures[1].declaration.name == "height" &&
              textures[1].declaration.slot == 1);
        Check(!textures[0].overridden && !textures[1].overridden);
        Check(textures[0].loaded && textures[0].error.empty());
        Check(textures[0].width == 4 && textures[0].height == 3);
        Check(textures[1].loaded && textures[1].width == 2 && textures[1].height == 2);

        const auto packed = workspace.PackedTextures();
        Check(packed[0] != nullptr && packed[0]->width == 4);
        Check(packed[1] != nullptr && packed[1]->width == 2);
        const auto revisions = workspace.TextureRevisions();
        Check(revisions[0] != 0 && revisions[1] != 0);
    }

    // Make a shader material and override one texture; the override
    // persists to the .orbitshadermaterial and PackedTextures reflects it.
    const auto materialPath = workspace.CreateShaderMaterial(
        "Ground", "RockGround", "Ground/Rock.shade.hlsl");
    workspace.Select(materialPath);
    Check(workspace.TextureParameters()[0].loaded);

    std::filesystem::create_directories(root / "Content" / "Textures" / "Variant");
    WriteTestBmp(
        root / "Content" / "Textures" / "Variant" / "Albedo2.bmp", 6U, 5U);
    content.Scan();

    workspace.SetTextureParameter(
        "albedo", "Content/Textures/Variant/Albedo2.bmp");
    {
        const auto textures = workspace.TextureParameters();
        Check(textures[0].overridden);
        Check(textures[0].path == "Content/Textures/Variant/Albedo2.bmp");
        Check(textures[0].loaded && textures[0].width == 6 && textures[0].height == 5);
    }
    {
        std::ifstream materialFile(root / materialPath);
        const std::string materialText(
            (std::istreambuf_iterator<char>(materialFile)),
            std::istreambuf_iterator<char>());
        Check(materialText.find("[[shader_material.texture]]") != std::string::npos);
        Check(materialText.find("Content/Textures/Variant/Albedo2.bmp") !=
              std::string::npos);
    }

    // A missing file keeps the last good texture and reports an error,
    // exactly like a mesh that fails to reparse.
    workspace.SetTextureParameter("albedo", "Content/Textures/NoSuchFile.bmp");
    {
        const auto textures = workspace.TextureParameters();
        Check(!textures[0].error.empty());
    }

    // Resetting drops back to the shader's declared default.
    workspace.ResetTextureParameter("albedo");
    {
        const auto textures = workspace.TextureParameters();
        Check(!textures[0].overridden);
        Check(textures[0].path == "Content/Textures/Albedo.bmp");
    }

    std::filesystem::remove_all(root);
}

void TestMeshWorkspace(const shader::Compiler& compiler)
{
    const auto root = MakeProject();
    content::ContentService content(root);
    content.Scan();
    shading::ShadingWorkspace workspace(content, &compiler);

    workspace.CreateFolder("Meshes");
    content.WriteText("Content/Meshes/Box.obj", kCubeObj);
    content.WriteText("Content/Meshes/Skin.fbx", "not really");

    // A shader is open; choosing a mesh must not disturb it.
    workspace.CreateFolder("Shading");
    const auto shader = workspace.CreateShader("Shading", "S", "lit");
    workspace.Select(shader);
    Check(workspace.Status().compiled);

    workspace.Select("Meshes/Box.obj");
    Check(workspace.Selected() == shader);
    Check(workspace.Status().shader == shader);
    Check(workspace.Preview().shape == shading::PreviewShape::Mesh);
    Check(workspace.Preview().mesh == "Content/Meshes/Box.obj");
    Check(workspace.PreviewMeshStatus().loaded);
    Check(workspace.PreviewMeshStatus().triangles == 12);
    Check(workspace.PreviewMeshData() != nullptr);
    const auto firstRevision = workspace.PreviewMeshStatus().revision;

    // Only .obj can be previewed; other mesh formats and non-meshes are refused.
    Check(Throws([&] { workspace.SetPreviewMesh("Meshes/Skin.fbx"); }));
    Check(Throws([&] { workspace.SetPreviewMesh(shader); }));

    // An external save reloads with no explicit call (hot path).
    content.WriteText(
        "Content/Meshes/Box.obj",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    workspace.Update(0.016);
    Check(workspace.PreviewMeshStatus().triangles == 1);
    Check(workspace.PreviewMeshStatus().revision > firstRevision);

    // A save that does not parse keeps the last good mesh and says why.
    const auto goodRevision = workspace.PreviewMeshStatus().revision;
    content.WriteText("Content/Meshes/Box.obj", "v 0 0 0\nf 1 2 3\n");
    workspace.Update(0.016);
    Check(!workspace.PreviewMeshStatus().error.empty());
    Check(workspace.PreviewMeshStatus().error.find("Box.obj") != std::string::npos);
    Check(workspace.PreviewMeshStatus().loaded);
    Check(workspace.PreviewMeshData() != nullptr);
    Check(workspace.PreviewMeshData()->TriangleCount() == 1);
    (void)goodRevision;

    // Fixing it recovers.
    content.WriteText("Content/Meshes/Box.obj", kCubeObj);
    workspace.Update(0.016);
    Check(workspace.PreviewMeshStatus().error.empty());
    Check(workspace.PreviewMeshStatus().triangles == 12);

    // Rename and move follow the mesh; trash clears it and drops the shape back
    // to a sphere.
    const auto renamed = workspace.Rename("Meshes/Box.obj", "Crate.obj");
    Check(workspace.Preview().mesh == renamed.generic_string());
    Check(workspace.PreviewMeshStatus().loaded);
    workspace.CreateFolder("Meshes/Old");
    const auto moved = workspace.Move(renamed, "Meshes/Old");
    Check(workspace.Preview().mesh == moved.generic_string());
    (void)workspace.Trash(moved);
    Check(workspace.Preview().mesh.empty());
    Check(!workspace.PreviewMeshStatus().loaded);
    Check(workspace.PreviewMeshData() == nullptr);
    Check(workspace.Preview().shape == shading::PreviewShape::Sphere);

    std::filesystem::remove_all(root);
}

void TestMeshRpc(const shader::Compiler& compiler)
{
    const auto root = MakeProject();
    content::ContentService content(root);
    content.Scan();
    shading::ShadingWorkspace workspace(content, &compiler);
    content.WriteText("Content/Box.obj", kCubeObj);

    rpc::Dispatcher dispatcher;
    shading::RegisterShadingRpc(dispatcher, workspace);

    const auto call = [&](const std::string& method, const std::string& params)
    {
        const auto response = dispatcher.Dispatch(
            R"({"jsonrpc":"2.0","id":1,"method":")" + method +
            R"(","params":)" + params + "}");
        Check(response.has_value());
        return *response;
    };

    Check(call("shading.options", "{}").find("\"mesh\"") != std::string::npos);

    const auto set = call("shading.preview_set", R"({"mesh":"Box.obj"})");
    Check(set.find("\"error\":{") == std::string::npos);
    Check(set.find("\"shape\":\"mesh\"") != std::string::npos);
    Check(set.find("Content/Box.obj") != std::string::npos);

    const auto status = call("shading.status", "{}");
    Check(status.find("\"triangles\":12") != std::string::npos);
    Check(status.find("\"loaded\":true") != std::string::npos);

    // An explicit shape after a mesh wins; null clears the mesh.
    Check(call("shading.preview_set", R"({"mesh":"Box.obj","shape":"cube"})")
              .find("\"shape\":\"cube\"") != std::string::npos);

    // Clearing the mesh while it is the shape falls back to a sphere.
    Check(call("shading.preview_set", R"({"mesh":"Box.obj"})")
              .find("\"shape\":\"mesh\"") != std::string::npos);
    const auto cleared = call("shading.preview_set", R"({"mesh":null})");
    Check(cleared.find("\"shape\":\"sphere\"") != std::string::npos);
    Check(cleared.find("\"mesh\":\"\"") != std::string::npos);

    // Choosing the Mesh shape with no mesh is allowed (a sphere stands in).
    Check(call("shading.preview_set", R"({"shape":"mesh"})")
              .find("\"shape\":\"mesh\"") != std::string::npos);

    Check(call("shading.preview_set", R"({"mesh":"Nope.obj"})").find("1050") !=
          std::string::npos);
    Check(call("shading.preview_set", R"({"mesh":7})").find("1050") !=
          std::string::npos);

    std::filesystem::remove_all(root);
}

void TestCapture()
{
    const auto dir = MakeProject();
    const auto ofb = dir / "shot.ofb";

    {
        // 2x1 image: a pure red pixel and an over-range green one.
        std::ofstream out(ofb, std::ios::binary);
        out.write("OFB1", 4);
        const std::uint32_t header[3]{2U, 1U, 4U};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        const float pixels[8]{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 3.0F, 0.0F, 1.0F};
        out.write(reinterpret_cast<const char*>(pixels), sizeof(pixels));
    }

    const auto image = shading::ReadFloatCapture(ofb);
    Check(image.width == 2 && image.height == 1);

    shading::WriteBmp32(dir / "shot.bmp", image);
    std::string bytes;
    {
        // Scoped so the handle is closed before the directory is removed.
        std::ifstream bmp(dir / "shot.bmp", std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(bmp), {});
    }
    Check(bytes.size() == 54 + 8);
    Check(bytes[0] == 'B' && bytes[1] == 'M');
    // BGRA: first pixel red, second green (over-range clamps to 255).
    Check(static_cast<unsigned char>(bytes[54 + 2]) == 255);
    Check(static_cast<unsigned char>(bytes[54 + 4 + 1]) == 255);
    Check(static_cast<unsigned char>(bytes[54 + 4 + 2]) == 0);

    Check(Throws([&] { (void)shading::ReadFloatCapture(dir / "missing.ofb"); }));
    Check(Throws([&] { shading::WriteBmp32(dir / "bad.bmp", {}); }));

    std::filesystem::remove_all(dir);
}
} // namespace

int main()
{
    // An uncaught exception would surface as a bare fast-fail exit code; say
    // what happened instead.
    try
    {
        shader::dxc::DxcShaderCompiler compiler;

        TestEnumsAndParameters();
        TestTextureParametersAndDisplacement(compiler);
        TestTemplatesCompile(compiler);
        TestDiagnosticsPointAtUserFile(compiler);
        TestWorkspace(compiler);
        TestTextureWorkspace(compiler);
        TestRpc(compiler);
        TestObjParser();
        TestMeshWorkspace(compiler);
        TestMeshRpc(compiler);
        TestCapture();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Shading test threw: " << exception.what() << std::endl;
        return 1;
    }
    return 0;
}
