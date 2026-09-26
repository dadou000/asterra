#include <orbit/content/ContentService.hpp>
#include <orbit/core/StrongId.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/shading/ShaderProgram.hpp>
#include <orbit/shading/ShadingCapture.hpp>
#include <orbit/shading/ShadingContract.hpp>
#include <orbit/shading/ShadingRpc.hpp>
#include <orbit/shading/ShadingWorkspace.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <source_location>
#include <string>

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
    const auto ok = [](const std::string& response)
    { return response.find("\"error\"") == std::string::npos; };

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
        TestTemplatesCompile(compiler);
        TestDiagnosticsPointAtUserFile(compiler);
        TestWorkspace(compiler);
        TestRpc(compiler);
        TestCapture();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Shading test threw: " << exception.what() << std::endl;
        return 1;
    }
    return 0;
}
