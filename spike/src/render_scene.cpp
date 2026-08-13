// Renders a Makina scene end to end: .makina.json -> Scene -> EvalProgram -> generated HLSL ->
// DX12 -> image.
//
// This is the first point at which a real Grasp3D model reaches the new renderer, so it is also
// the first check that the whole chain agrees on what the model is. The camera frames the scene
// from its own bounds; nothing here is tuned per file.

#include "dx12_headless.hpp"
#include "image_out.hpp"
#include "scene_codegen.hpp"

#include <makina/Edit.hpp>
#include <makina/Bounds.hpp>
#include <makina/Pov.hpp>
#include <makina/RenderMaterial.hpp>
#include <makina/SceneJson.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

/// The one light this renderer has, as a unit vector pointing the way the light travels.
///
/// In one function because the .pov written for a comparison has to place its light from the same
/// numbers. Two copies of a direction is how a color comparison ends up measuring a light that
/// moved rather than a shading model that differs.
inline void lightDirection(float out[3]) {
    out[0] = -0.45f;
    out[1] = -0.78f;
    out[2] = -0.44f;
    const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    for (int i = 0; i < 3; ++i) {
        out[i] /= len;
    }
}

/// The POV preamble that asks for the same lighting the shader computes.
///
/// Every line here is a difference that would otherwise show up in the comparison and be blamed on
/// the shading model:
///
///   assumed_gamma 1.0   shading happens in linear light on both sides. Without it POV works in
///                       gamma space and every mid-tone drifts.
///   shadowless          the shader casts none. Leaving POV's on would put every shadow into the
///                       difference, which says nothing about whether finish{} agrees.
///   a distant light     the shader's light is directional. POV has no such thing, so it gets a
///                       point light far enough away that the rays are parallel to within a pixel.
///   no ambient_light    left at POV's default rgb<1,1,1>, which is what mkAmbientTerm assumes.
inline std::string povMatchPreamble(const float light[3]) {
    // Far enough that the spread across a scene-sized object is well under a degree, and POV
    // applies no falloff without fade_power, so the distance costs nothing in brightness.
    constexpr double kDistance = 1.0e4;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "global_settings{ assumed_gamma 1.0 }\n"
                  "background{ color rgb<0,0,0> }\n"
                  "light_source{\n"
                  "\t<%.9g, %.9g, %.9g>\n"
                  "\tcolor rgb<1,1,1>\n"
                  "\tshadowless\n"
                  "}\n",
                  -light[0] * kDistance, -light[1] * kDistance, -light[2] * kDistance);
    return buf;
}

/// The same preamble for a scene that carries its own lights.
///
/// The lamps come from the scene rather than from here, so the two renderers cannot disagree about
/// where they are -- which is the whole reason lights moved into the scene format.
inline std::string povScenePreamble(const makina::Scene& s) {
    return "global_settings{ assumed_gamma 1.0 }\nbackground{ color rgb<0,0,0> }\n" +
           makina::detail::povLights(s);
}

// Must match cbuffer Params in scene_prelude.hlsl.
struct alignas(256) FrameParams {
    float eye[3];       float tanHalfFov;
    float forward[3];   float aspect;
    float right[3];     std::uint32_t nodeCount;
    float up[3];        std::uint32_t maxSteps;
    float lightDir[3];  float stepScale;
    float farDist;      std::uint32_t enableAo;  std::uint32_t debugMode;  float groundY;
    float center[3];    float sceneRadius;
    std::uint32_t programCount;  std::uint32_t materialCount;
    std::uint32_t pigmentCount;  std::uint32_t povMatch;
    std::uint32_t lightCount;    std::uint32_t cameraKind;
    float         cameraAngle;   std::uint32_t padA;
    // Only the interactive viewport highlights a selection; zero here means "nothing selected",
    // which is what every offscreen render wants.
    float selMin[3];    float selValid;
    float selMax[3];    float pad2;
};

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<char> readBinary(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open '" + path + "'. Was the DXC step run?");
    }
    return std::vector<char>((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
}

void normalize3(float v[3]) {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 0.0f) {
        v[0] /= len;  v[1] /= len;  v[2] /= len;
    }
}

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/// Whether any primitive in the scene reaches past every bound. Plane is the only one.
bool hasUnboundedPrimitive(const makina::Scene& s) {
    for (std::uint32_t i = 0; i < s.nodes.count; ++i) {
        if (static_cast<makina::Op>(s.nodes[i].op) == makina::Op::Plane) {
            return true;
        }
    }
    return false;
}

/// Half the diagonal of a box, which is what every camera distance here is a multiple of.
double radiusOf(const makina::Aabb& box) {
    if (!box.valid) {
        return 1.0;
    }
    double diag = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double span = box.hi[i] - box.lo[i];
        diag += span * span;
    }
    const double r = std::sqrt(diag) * 0.5;
    return r > 1e-4 ? r : 1e-4;
}

/// Frames the scene from its own bounds, so any model arrives in view without hand tuning.
FrameParams frameScene(const makina::Aabb& box, int width, int height, std::uint32_t maxSteps) {
    float center[3] = {0.0f, 0.0f, 0.0f};
    float radius = 1.0f;

    if (box.valid) {
        double diag = 0.0;
        for (int i = 0; i < 3; ++i) {
            center[i] = static_cast<float>((box.lo[i] + box.hi[i]) * 0.5);
            const double span = box.hi[i] - box.lo[i];
            diag += span * span;
        }
        radius = static_cast<float>(std::sqrt(diag) * 0.5);
        if (radius < 1e-4f) {
            radius = 1e-4f;
        }
    }

    const float fovDeg = 35.0f;
    const float tanHalf = std::tan(fovDeg * 0.5f * 3.14159265358979323846f / 180.0f);
    // Three quarter view from above, pulled back far enough that the bounding sphere fits.
    const float dist = radius / tanHalf * 1.05f;
    const float dir[3] = {0.55f, 0.42f, 0.72f};

    FrameParams p{};
    for (int i = 0; i < 3; ++i) {
        p.eye[i] = center[i] + dir[i] * dist;
    }

    float fwd[3] = {center[0] - p.eye[0], center[1] - p.eye[1], center[2] - p.eye[2]};
    normalize3(fwd);
    const float worldUp[3] = {0.0f, 1.0f, 0.0f};

    float right[3];
    cross3(fwd, worldUp, right);
    normalize3(right);
    float up[3];
    cross3(right, fwd, up);
    normalize3(up);

    std::memcpy(p.forward, fwd, sizeof(fwd));
    std::memcpy(p.right, right, sizeof(right));
    std::memcpy(p.up, up, sizeof(up));

    p.tanHalfFov = tanHalf;
    p.aspect = static_cast<float>(width) / static_cast<float>(height);
    p.maxSteps = maxSteps;

    float light[3];
    lightDirection(light);
    std::memcpy(p.lightDir, light, sizeof(light));

    p.stepScale = 0.85f;
    // Far plane and every epsilon in the shader scale off this, so millimetre and metre models
    // both resolve without a per-file constant.
    p.farDist = dist + radius * 2.5f;
    p.enableAo = 1u;
    p.debugMode = 0u;
    // The floor sits on the model's own base. A part drawn against nothing reads as floating no
    // matter how good its shading is -- the contact shadow is what puts it somewhere.
    p.groundY = box.valid ? static_cast<float>(box.lo[1]) : -radius;
    std::memcpy(p.center, center, sizeof(center));
    p.sceneRadius = radius;
    return p;
}

spike::ComPtr<ID3D12RootSignature> createRootSignature(ID3D12Device* device) {
    // b0 for the frame, t0 for the evaluation program. The program is a *root* SRV rather than a
    // descriptor table: it is one buffer bound once per draw, and a heap plus a descriptor would
    // be machinery around a single pointer. The generated-code path leaves t0 unbound, which is
    // legal because its shader never declares it.
    D3D12_ROOT_PARAMETER param[5]{};
    param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param[0].Descriptor.ShaderRegister = 0;
    param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    param[1].Descriptor.ShaderRegister = 0;
    param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // t1 is the material table. Bound even by the generated path, which has the program inlined
    // and leaves t0 unused -- one signature for both paths means the two are interchangeable.
    param[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    param[2].Descriptor.ShaderRegister = 1;
    param[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // t2 the pigments, t3 the lights.
    param[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    param[3].Descriptor.ShaderRegister = 2;
    param[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    param[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    param[4].Descriptor.ShaderRegister = 3;
    param[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 5;
    desc.pParameters = param;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    spike::ComPtr<ID3DBlob> blob;
    spike::ComPtr<ID3DBlob> error;
    const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) {
        const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no detail)";
        throw spike::Dx12Error(std::string("D3D12SerializeRootSignature failed: ") + msg);
    }

    spike::ComPtr<ID3D12RootSignature> rs;
    spike::check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&rs)),
                 "CreateRootSignature");
    return rs;
}

struct CompiledShader {
    std::string vsPath;
    std::string psPath;
    double      milliseconds = 0.0;
};

CompiledShader compileScene(const makina::EvalProgram& prog, const std::string& shaderDir,
                            const std::string& coreInclude, const std::string& outDir,
                            const std::string& tag, const std::string& shadingInclude,
                            bool interpret) {
    const std::string hlsl = outDir + "/scene_" + tag + ".hlsl";
    const std::string log = outDir + "/scene_" + tag + ".log";

    {
        std::ofstream out(hlsl, std::ios::binary);
        if (!out) {
            throw std::runtime_error("could not write the generated shader to '" + hlsl + "'");
        }
        out << spike::generateShader(prog, shadingInclude, interpret);
    }

    CompiledShader r;
    r.vsPath = outDir + "/scene_" + tag + "_vs.cso";
    r.psPath = outDir + "/scene_" + tag + "_ps.cso";

    const auto begin = std::chrono::steady_clock::now();
    for (int stage = 0; stage < 2; ++stage) {
        const char* profile = stage == 0 ? "vs_6_0" : "ps_6_0";
        const char* entry = stage == 0 ? "VSMain" : "PSMain";
        const std::string outPath = stage == 0 ? r.vsPath : r.psPath;

        // Two include roots: the shader-side glue, and makina-core so Sdf.hpp resolves. The
        // second is the whole point -- the shader compiles the evaluator's own source.
        const std::string cmd = "\"\"" DXC_PATH "\" -T " + std::string(profile) + " -E " + entry +
                                " -O3 -I \"" + shaderDir + "\" -I \"" + coreInclude +
                                "\" -Fo \"" + outPath + "\" \"" + hlsl + "\" > \"" + log +
                                "\" 2>&1\"";
        if (std::system(cmd.c_str()) != 0) {
            std::string detail;
            try {
                const std::vector<char> l = readBinary(log);
                detail.assign(l.begin(), l.end());
            } catch (const std::exception&) {
                detail = "(no compiler output captured)";
            }
            throw std::runtime_error("dxc failed on the generated shader:\n" + detail);
        }
    }
    const auto end = std::chrono::steady_clock::now();
    r.milliseconds = std::chrono::duration<double, std::milli>(end - begin).count();
    return r;
}

std::string dirOf(const std::string& path) {
    const std::size_t cut = path.find_last_of("\\/");
    return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
}

std::string stemOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

}  // namespace

int main(int argc, char** argv) {
    int width = 1280;
    int height = 720;
    std::uint32_t maxSteps = 192;
    std::string shading = "scene_shading.hlsl";
    std::string prefix = "render";
    bool debugSweep = false;
    // One draw on this iGPU has come back anywhere between 10 and 44 ms for the same scene: the
    // clock ramps, and a single sample measures the clock, not the shader. Anything quoted as a
    // cost has to be the minimum of several.
    int repeat = 1;
    // --mask also writes the .pov, because the whole point of the mask is to be compared against
    // POV-Ray's render of the same scene, and the two have to share one camera. Deriving the
    // camera twice is how a silhouette comparison ends up measuring a camera mismatch.
    bool writePov = false;
    // Renders the shaded picture *and* the .pov that should match it, from one camera and one set
    // of light directions. The point is to compare colors, which only means anything if the two
    // are asked for the same thing -- so this mode also turns off the terms POV has no equivalent
    // for (scene_prelude.hlsl, gPovMatch).
    bool povMatch = false;
    // Which camera turns a pixel into a ray. Only the ray generator changes, which is why five of
    // them cost five lines here rather than five pipelines.
    std::uint32_t cameraKind = 0;
    // Loop over a buffered program instead of generating straight-line code. Slower, but the
    // shader is the same for every scene -- which is what a runtime that cannot ship a shader
    // compiler needs (PLAN.md D-04).
    bool interpret = false;
    std::vector<std::string> scenes;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--width" && i + 1 < argc)       width = std::atoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc) height = std::atoi(argv[++i]);
        else if (a == "--steps" && i + 1 < argc)  maxSteps = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--repeat" && i + 1 < argc) { const int n = std::atoi(argv[++i]); repeat = n > 1 ? n : 1; }
        else if (a == "--weathered") { shading = "scene_weathered.hlsl"; prefix = "weathered"; }
        else if (a == "--fields") { shading = "scene_fields_debug.hlsl"; prefix = "field"; debugSweep = true; }
        else if (a == "--mask") { shading = "scene_mask.hlsl"; prefix = "mask"; writePov = true; }
        else if (a == "--pov-match") { prefix = "shaded"; writePov = true; povMatch = true; }
        else if (a == "--interpret") { interpret = true; prefix = "interp"; }
        else if (a == "--camera" && i + 1 < argc) {
            const std::string k = argv[++i];
            if (k == "perspective")           { cameraKind = 0; }
            else if (k == "ortho")            { cameraKind = 1; }
            else if (k == "fisheye")          { cameraKind = 2; }
            else if (k == "ultra")            { cameraKind = 3; }
            else if (k == "panoramic")        { cameraKind = 4; }
            else {
                std::fprintf(stderr,
                             "error: '%s' is not a camera this renderer has; it takes "
                             "perspective, ortho, fisheye, ultra or panoramic\n",
                             k.c_str());
                return 2;
            }
            prefix = "cam_" + k;
        }
        else scenes.push_back(a);
    }

    if (scenes.empty()) {
        std::fprintf(stderr, "usage: render_scene [--width N] [--height N] [--steps N] "
                             "<scene.makina.json> ...\n");
        return 2;
    }

    const std::string exeDir = dirOf(argv[0]);
    int failures = 0;

    try {
        spike::HeadlessDevice dev(width, height, /*enableDebugLayer=*/false);
        std::printf("Makina scene renderer\n");
        std::printf("adapter    : %ls\n", dev.adapterName().c_str());
        std::printf("resolution : %dx%d, max %u sphere-trace steps\n\n", width, height, maxSteps);

        auto rootSig = createRootSignature(dev.device());

        for (const std::string& scenePath : scenes) {
            const std::string tag = stemOf(scenePath);
            std::printf("%s\n", tag.c_str());

            try {
                // The solid, not the tree that was authored. A muted subtree is not part of
                // the shape (Op.hpp), and a tool that skipped this step would draw or
                // export something the modeller does not show.
                const makina::Scene scene =
                    makina::withoutMuted(makina::parseScene(readFile(scenePath)));
                const makina::EvalProgram prog = makina::flatten(scene);
                const makina::BoundsResult bounds = makina::worldBounds(scene);

                std::printf("    %u authoring nodes -> %zu program nodes, stack %d\n",
                            scene.nodes.count, prog.nodes.size(), prog.maxStackDepth);
                if (prog.report.skippedFaces > 0) {
                    std::printf("    %d zero-thickness face(s) skipped: a surface of measure zero "
                                "has nothing for a ray to hit\n", prog.report.skippedFaces);
                }
                if (prog.report.skippedUnsupported > 0) {
                    std::printf("    %d unsupported op(s) skipped\n", prog.report.skippedUnsupported);
                }
                if (prog.nodes.empty()) {
                    std::printf("    nothing renderable; skipped\n\n");
                    continue;
                }

                if (writePov && hasUnboundedPrimitive(scene)) {
                    // POV-Ray has no far plane; the march does. An infinite Plane therefore fills
                    // POV's frame and stops at a circle in ours, and the comparison measures the
                    // far distance rather than the geometry. Skipping is the honest answer -- the
                    // silhouette of an unbounded solid is not a thing the two can agree on.
                    std::printf("    no .pov: the scene has unbounded geometry, whose silhouette "
                                "depends on the far plane\n");
                } else if (writePov) {
                    const FrameParams fp = frameScene(bounds.box, width, height, maxSteps);
                    makina::PovOptions po;
                    po.silhouette = !povMatch;
                    po.title = tag;
                    if (povMatch) {
                        float light[3];
                        lightDirection(light);
                        po.preamble = scene.lights.count > 0
                                          ? povScenePreamble(scene)
                                          : povMatchPreamble(light);
                    }
                    for (int k = 0; k < 3; ++k) {
                        po.camera.eye[k] = fp.eye[k];
                        po.camera.lookAt[k] = fp.eye[k] + fp.forward[k];
                        po.camera.up[k] = fp.up[k];
                    }
                    po.camera.fovY = 2.0 * std::atan(fp.tanHalfFov) * 180.0 / 3.14159265358979323846;
                    po.camera.aspect = fp.aspect;
                    // The same camera POV has to use, or every pixel differs for a reason that
                    // has nothing to do with the model.
                    switch (cameraKind) {
                        case 1: po.camera.kind = makina::PovCameraKind::Orthographic; break;
                        case 2: po.camera.kind = makina::PovCameraKind::Fisheye; break;
                        case 3: po.camera.kind = makina::PovCameraKind::UltraWideAngle; break;
                        case 4: po.camera.kind = makina::PovCameraKind::Panoramic; break;
                        default: po.camera.kind = makina::PovCameraKind::Perspective; break;
                    }
                    po.camera.orthoHalfWidth = radiusOf(bounds.box) * 1.1;
                    if (cameraKind >= 2) {
                        // 160 degrees, matching what the shader is given.
                        po.camera.fovY = 160.0;
                        po.camera.aspect = 1.0;
                    }

                    const std::string povPath =
                        exeDir + "/" + (povMatch ? "shaded_" : "mask_") + tag + ".pov";
                    std::ofstream pov(povPath, std::ios::binary);
                    pov << makina::writePov(scene, po);
                    std::printf("    wrote %s\n", povPath.c_str());
                }

                const CompiledShader shader =
                    compileScene(prog, SPIKE_SHADER_DIR, MAKINA_CORE_INCLUDE, exeDir, tag, shading,
                                 interpret);

                const std::vector<char> vs = readBinary(shader.vsPath);
                const std::vector<char> ps = readBinary(shader.psPath);

                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                pd.pRootSignature = rootSig.Get();
                pd.VS = {vs.data(), vs.size()};
                pd.PS = {ps.data(), ps.size()};
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.RasterizerState.DepthClipEnable = TRUE;
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                pd.SampleDesc.Count = 1;

                spike::ComPtr<ID3D12PipelineState> pso;
                spike::check(dev.device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)),
                             "CreateGraphicsPipelineState");

                // One draw per field when sweeping; the shader is the same, only gDebugMode moves.
                static const char* const kFieldNames[] = {
                    "", "curvature", "ao", "thickness", "upfacing", "normal"};
                const int passes = debugSweep ? 5 : 1;

                for (int pass = 0; pass < passes; ++pass) {
                    FrameParams params = frameScene(bounds.box, width, height, maxSteps);
                    params.nodeCount = static_cast<std::uint32_t>(prog.nodes.size());
                    params.materialCount = scene.materials.count;
                    params.pigmentCount = scene.pigments.count;
                    params.povMatch = povMatch ? 1u : 0u;
                    params.lightCount = scene.lights.count;
                    params.cameraKind = cameraKind;
                    // A fisheye or a panoramic wants a much wider view than the framing angle,
                    // and an orthographic wants a distance rather than an angle at all. Derived
                    // from the same bounds the framing uses, so no camera needs hand tuning.
                    params.cameraAngle =
                        cameraKind == 1u
                            ? static_cast<float>(radiusOf(bounds.box) * 1.1)
                            : (cameraKind == 0u ? 0.0f : 2.7925f);   // 160 degrees
                    if (povMatch) {
                        // POV has no ambient occlusion at all. Leaving it on would darken every
                        // crease on one side only.
                        params.enableAo = 0u;
                    }
                    params.debugMode = debugSweep ? static_cast<std::uint32_t>(pass + 1) : 0u;

                    params.programCount = static_cast<std::uint32_t>(prog.nodes.size());

                    spike::GpuBuffer cb =
                        dev.createBufferWithData(&params, sizeof(params), L"frame params");

                    // Always uploaded, even for the generated path: the two then differ only in
                    // the shader, so a timing comparison between them measures the shader.
                    spike::GpuBuffer programBuffer = dev.createBufferWithData(
                        prog.nodes.data(), prog.nodes.size() * sizeof(makina::EvalNode),
                        L"evaluation program");

                    // A scene with no materials still needs a buffer: an unbound root SRV that
                    // the shader declares is undefined behaviour, not an empty table. One default
                    // entry costs 48 bytes and the shader never reads it, because a surface with
                    // no material takes mkDefaultMaterial instead.
                    std::vector<makina::GpuMaterial> mats = makina::gpuMaterials(scene);
                    if (mats.empty()) {
                        mats.push_back(makina::defaultGpuMaterial());
                    }
                    spike::GpuBuffer materialBuffer = dev.createBufferWithData(
                        mats.data(), mats.size() * sizeof(makina::GpuMaterial), L"materials");

                    // From the program, not from the scene: a pattern is fixed in the space of
                    // the object wearing it, so the table is one entry per (pattern, place)
                    // pair and only the flatten knows the places.
                    //
                    // Same rule as the materials: a declared SRV must be bound to something.
                    std::vector<makina::GpuPigment> pigs = prog.pigments;
                    if (pigs.empty()) {
                        pigs.push_back(makina::GpuPigment{});
                    }
                    spike::GpuBuffer pigmentBuffer = dev.createBufferWithData(
                        pigs.data(), pigs.size() * sizeof(makina::GpuPigment), L"pigments");

                    std::vector<makina::Light> lights;
                    for (std::uint32_t li = 0; li < scene.lights.count; ++li) {
                        lights.push_back(scene.lights[li]);
                    }
                    if (lights.empty()) {
                        lights.push_back(makina::Light{});
                    }
                    spike::GpuBuffer lightBuffer = dev.createBufferWithData(
                        lights.data(), lights.size() * sizeof(makina::Light), L"lights");

                    dev.beginFrame();
                    dev.uploadBuffer(cb, sizeof(params),
                                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                    dev.uploadBuffer(programBuffer, prog.nodes.size() * sizeof(makina::EvalNode),
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    dev.uploadBuffer(materialBuffer, mats.size() * sizeof(makina::GpuMaterial),
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    dev.uploadBuffer(pigmentBuffer, pigs.size() * sizeof(makina::GpuPigment),
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    dev.uploadBuffer(lightBuffer, lights.size() * sizeof(makina::Light),
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    dev.executeAndWait();

                    double best = 1e30;
                    double worst = 0.0;
                    for (int rep = 0; rep < repeat; ++rep) {
                        dev.beginFrame();
                        dev.beginRenderTarget();
                        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                        dev.clearRenderTarget(clear);

                        ID3D12GraphicsCommandList* cl = dev.list();
                        cl->SetGraphicsRootSignature(rootSig.Get());
                        cl->SetPipelineState(pso.Get());
                        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        cl->SetGraphicsRootConstantBufferView(
                            0, cb.resource->GetGPUVirtualAddress());
                        cl->SetGraphicsRootShaderResourceView(
                            1, programBuffer.resource->GetGPUVirtualAddress());
                        cl->SetGraphicsRootShaderResourceView(
                            2, materialBuffer.resource->GetGPUVirtualAddress());
                        cl->SetGraphicsRootShaderResourceView(
                            3, pigmentBuffer.resource->GetGPUVirtualAddress());
                        cl->SetGraphicsRootShaderResourceView(
                            4, lightBuffer.resource->GetGPUVirtualAddress());

                        dev.writeTimestamp(0);
                        cl->DrawInstanced(3, 1, 0, 0);
                        dev.writeTimestamp(1);

                        dev.endRenderTarget();
                        dev.copyRenderTargetToReadback();
                        dev.resolveTimestamps();
                        dev.executeAndWait();

                        const double ms = dev.lastGpuMilliseconds();
                        best = ms < best ? ms : best;
                        worst = ms > worst ? ms : worst;
                    }

                    int rowPitch = 0;
                    const std::uint8_t* pixels = dev.mapReadback(rowPitch);
                    const std::string suffix =
                        debugSweep ? std::string("_") + kFieldNames[pass + 1] : std::string();
                    const std::string bmp = exeDir + "/" + prefix + "_" + tag + suffix + ".bmp";
                    std::string err;
                    if (!spike::writeBmp(bmp, pixels, width, height, rowPitch, err)) {
                        std::fprintf(stderr, "    warning: %s\n", err.c_str());
                    }
                    dev.unmapReadback();

                    if (repeat > 1) {
                        std::printf("    compiled in %.0f ms, drew in %.2f ms (min of %d, worst "
                                    "%.2f) -> %s\n",
                                    shader.milliseconds, best, repeat, worst, bmp.c_str());
                    } else {
                        std::printf("    compiled in %.0f ms, drew in %.2f ms -> %s\n",
                                    shader.milliseconds, best, bmp.c_str());
                    }
                }
                std::printf("\n");

            } catch (const std::exception& e) {
                std::printf("    FAILED: %s\n\n", e.what());
                ++failures;
            }
        }

    } catch (const std::exception& e) {
        std::fprintf(stderr, "renderer failed: %s\n", e.what());
        return 1;
    }

    return failures == 0 ? 0 : 1;
}
