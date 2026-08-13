// The Makina viewport: a window you can orbit, frame and click in.
//
// Step 4 of docs/APP_DESIGN.md. No HTML yet -- this is the half of Phase 3 that does not need CEF,
// and it is deliberately the half that comes first, because the feel of a viewport is decided by
// the camera and by picking and both of those are already tested headless (Camera.hpp, Pick.hpp,
// Keymap.hpp). What is added here is only the wiring: messages in, a frame out.
//
//   makina_viewport <scene.makina.json> [--keymap maya|blender]
//                   [--frames N] [--screenshot <path>] [--select <id>] [--keys "W X 5 ENTER"]
//                   [--actions "view.front view.genuine"]
//
// Everything the user can do arrives as an action name from the keymap, never as a hard-coded
// key. That is not indirection for its own sake: Phase 3's exit condition is that someone who uses
// Maya or Blender picks this up, and their first sentence will be about a binding.

#include "dx12_swapchain.hpp"
#include "win32_window.hpp"

#include "../../spike/src/dxc_invoke.hpp"
#include "../../spike/src/image_out.hpp"
#include "../../spike/src/scene_codegen.hpp"

#include <makina/Bounds.hpp>
#include <makina/Camera.hpp>
#include <makina/Edit.hpp>
#include <makina/Flatten.hpp>
#include <makina/Keymap.hpp>
#include <makina/Pick.hpp>
#include <makina/Selection.hpp>
#include <makina/RenderMaterial.hpp>
#include <makina/History.hpp>
#include <makina/SceneJson.hpp>
#include <makina/Transform.hpp>
#include <makina/ViewState.hpp>

#include <chrono>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

namespace {

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
    // Scalars rather than an array: a constant-buffer array puts every element on its own 16-byte
    // boundary in HLSL, so a uint[3] here would be 12 bytes and 48 there.
    std::uint32_t lightCount;    std::uint32_t padA;  std::uint32_t padB;  std::uint32_t padC;
    float selMin[3];    float selValid;
    float selMax[3];    float pad1;
};

/// One transform, applied to every selected solid.
///
/// Through topLevel(), so a solid inside another selected solid moves once. Applied in turn to the
/// scene the previous one produced: each returns a whole tree, and threading them is what makes
/// "move these three" one edit in the history rather than three.
///
/// Refuses as a whole when any one of them refuses. Half a move is worse than none -- the user
/// asked for one gesture and would have to work out which half landed before undoing it.
makina::EditResult transformSelection(const makina::Scene& scene,
                                      const makina::Selection& selection,
                                      makina::TransformKind kind, makina::TransformAxis axis,
                                      double value) {
    makina::EditResult out;
    out.scene = scene;
    out.ok = false;
    const makina::Selection targets = makina::topLevel(scene, selection);
    if (targets.empty()) {
        out.why = "nothing selected";
        return out;
    }
    for (const std::uint32_t id : targets) {
        const makina::EditResult r = makina::applyTransform(out.scene, id, kind, axis, value);
        if (!r.ok) {
            makina::EditResult fail;
            fail.scene = scene;
            fail.ok = false;
            fail.why = r.why;
            return fail;
        }
        out.scene = r.scene;
    }
    out.ok = true;
    return out;
}

/// The box around everything selected, or `fallback` when nothing selected is still in the tree.
makina::Aabb selectionBox(const makina::Scene& scene, const makina::Selection& selection,
                          const makina::Aabb& fallback) {
    makina::Aabb box{};
    for (const std::uint32_t id : selection) {
        const std::uint16_t index = makina::indexOfId(scene, id);
        if (index == makina::kNoChild) {
            continue;
        }
        const makina::BoundsResult r = makina::worldBounds(scene, index);
        if (!r.box.valid) {
            continue;
        }
        if (!box.valid) {
            box = r.box;
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            box.lo[i] = box.lo[i] < r.box.lo[i] ? box.lo[i] : r.box.lo[i];
            box.hi[i] = box.hi[i] > r.box.hi[i] ? box.hi[i] : r.box.hi[i];
        }
    }
    return box.valid ? box : fallback;
}

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
        throw std::runtime_error("could not open '" + path + "'");
    }
    return std::vector<char>((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
}

/// Where the generated shader and its compiled blobs go.
///
/// Not next to the scene. Scenes live in the repository, and a viewport that drops a .hlsl, two
/// .cso and DXC's logs beside every file it opens turns `git status` into noise and eventually
/// commits build output as test data.
std::string scratchDir() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "makina_viewport";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error("could not make a scratch directory for the generated shader: " +
                                 ec.message());
    }
    return dir.string();
}

/// The one pipeline shape this viewport draws with: a full-screen triangle, no depth, no culling.
///
/// Shared by the generated pipeline and the interpreted one so that switching between them during
/// a drag can only change the shader. If the two states drifted apart, the preview would differ
/// from the committed picture for reasons that have nothing to do with the edit.
app::ComPtr<ID3D12PipelineState> createPipeline(ID3D12Device* device, ID3D12RootSignature* rootSig,
                                                const std::vector<char>& vs,
                                                const std::vector<char>& ps) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSig;
    pd.VS = {vs.data(), vs.size()};
    pd.PS = {ps.data(), ps.size()};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;

    app::ComPtr<ID3D12PipelineState> pso;
    app::check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)),
               "CreateGraphicsPipelineState");
    return pso;
}

app::ComPtr<ID3D12RootSignature> createRootSignature(ID3D12Device* device) {
    // b0 for the frame, t0 for the evaluation program -- the same signature the offscreen renderer
    // uses, so the generated shaders are interchangeable between them.
    D3D12_ROOT_PARAMETER param[5]{};
    param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param[0].Descriptor.ShaderRegister = 0;
    param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    param[1].Descriptor.ShaderRegister = 0;
    param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // t1 the material table, t2 the pigments. Declared by every shading wrapper, so both are
    // bound on both paths.
    param[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    param[2].Descriptor.ShaderRegister = 1;
    param[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
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

    app::ComPtr<ID3DBlob> blob;
    app::ComPtr<ID3DBlob> error;
    const HRESULT hr =
        D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) {
        const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "(none)";
        throw app::Dx12Error(std::string("D3D12SerializeRootSignature: ") + msg);
    }
    app::ComPtr<ID3D12RootSignature> rs;
    app::check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&rs)),
               "CreateRootSignature");
    return rs;
}

/// One upload-heap buffer per frame in flight.
///
/// A single buffer would be written by the CPU while the GPU still reads last frame's contents,
/// and the tearing that produces is the sort that looks like a camera bug. Upload heap rather than
/// default: this is written every frame, so a staging copy would cost more than it saves.
class RingBuffer {
public:
    RingBuffer(ID3D12Device* device, std::size_t bytes) : m_bytes(bytes) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (std::uint32_t i = 0; i < app::kFrameCount; ++i) {
            app::check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&m_slots[i])),
                       "CreateCommittedResource (upload)");
            D3D12_RANGE noRead{0, 0};
            app::check(m_slots[i]->Map(0, &noRead, &m_mapped[i]), "Map");
        }
    }

    D3D12_GPU_VIRTUAL_ADDRESS write(const void* data, std::size_t bytes) {
        std::memcpy(m_mapped[m_at], data, bytes < m_bytes ? bytes : m_bytes);
        const D3D12_GPU_VIRTUAL_ADDRESS address = m_slots[m_at]->GetGPUVirtualAddress();
        m_at = (m_at + 1) % app::kFrameCount;
        return address;
    }

private:
    app::ComPtr<ID3D12Resource> m_slots[app::kFrameCount];
    void*                       m_mapped[app::kFrameCount]{};
    std::size_t                 m_bytes = 0;
    std::uint32_t               m_at = 0;
};

}  // namespace

int main(int argc, char** argv) {
    std::string scenePath;
    std::string keymapName = "maya";
    // A window cannot be looked at by anything automated, so "it built and did not crash" would
    // be the only evidence there is. These two turn it into a picture that can be checked.
    int         frameLimit = 0;
    std::string screenshot;
    // Synthetic input, for the same reason --screenshot exists: a transform is a state machine
    // spread over several frames, and nothing automated can press G, X, 5, Enter. One key per
    // frame, because that is how the real thing arrives -- feeding them all at once would test a
    // code path the user never takes.
    std::vector<std::string> scriptedKeys;
    makina::Selection        scriptedSelection;
    // Writes the tree out on exit, so an edit can be read as numbers rather than judged from a
    // picture. "The image changed" only says something changed.
    std::string savePath;
    std::string statePath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--keymap" && i + 1 < argc) {
            keymapName = argv[++i];
        } else if (a == "--frames" && i + 1 < argc) {
            frameLimit = std::atoi(argv[++i]);
        } else if (a == "--screenshot" && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (a == "--actions" && i + 1 < argc) {
            // Action names, not keys. The toolbar dispatches "view.genuine"; it does not press
            // anything, and view.genuine has no binding in either preset because neither Maya
            // nor Blender has the concept. Without this door it could only be reached by hand.
            //
            // They ride the scripted-key queue with a marker in front, so one action a frame and
            // the same ordering rules as --keys, rather than a second queue that could disagree
            // with the first about what happened when.
            std::istringstream names(argv[++i]);
            for (std::string n; names >> n;) {
                scriptedKeys.push_back("@" + n);
            }
        } else if (a == "--keys" && i + 1 < argc) {
            std::istringstream keys(argv[++i]);
            for (std::string k; keys >> k;) {
                scriptedKeys.push_back(k);
            }
        } else if (a == "--select" && i + 1 < argc) {
            // A comma-separated list, so a scripted run can exercise a selection of several.
            // That path has no mouse, and a check that could only ever select one thing would
            // leave the multi-selection edits with nothing driving them.
            const std::string list = argv[++i];
            std::size_t at = 0;
            while (at < list.size()) {
                const std::size_t comma = list.find(',', at);
                const std::string one = list.substr(at, comma == std::string::npos
                                                           ? std::string::npos
                                                           : comma - at);
                if (!one.empty()) {
                    scriptedSelection.push_back(static_cast<std::uint32_t>(std::atoi(one.c_str())));
                }
                if (comma == std::string::npos) {
                    break;
                }
                at = comma + 1;
            }
        } else if (a == "--save" && i + 1 < argc) {
            savePath = argv[++i];
        } else if (a == "--dump-state" && i + 1 < argc) {
            // What the shell would be reading. The bridge that pushes it does not exist yet, so
            // this is the seam where it will attach -- and until then the only way to check that
            // the *application* produces the state, rather than that the header can.
            statePath = argv[++i];
        } else {
            scenePath = a;
        }
    }
    if (scenePath.empty()) {
        std::fprintf(stderr,
                     "usage: makina_viewport <scene.makina.json> [--keymap maya|blender]\n"
                     "       [--frames N] [--screenshot <path>]\n"
                     "       [--select <id>] [--keys \"W X 5 ENTER\"] [--save <path>]\n"
                     "       [--actions \"view.front view.genuine\"] [--dump-state <path>]\n");
        return 2;
    }

    try {
        makina::History history(makina::parseScene(readFile(scenePath)), 64);
        if (makina::flatten(history.current()).nodes.empty()) {
            std::fprintf(stderr, "error: '%s' has nothing renderable in it\n", scenePath.c_str());
            return 1;
        }

        makina::Keymap keymap;
        std::string keymapError;
        const char* keymapJson =
            keymapName == "blender" ? makina::blenderKeymapJson() : makina::mayaKeymapJson();
        if (!keymap.load(keymapJson, keymapError)) {
            std::fprintf(stderr, "error: the built-in keymap did not load: %s\n",
                         keymapError.c_str());
            return 1;
        }

        // Which modifier holds the snap on, taken from the keymap rather than assumed.
        //
        // Only a modifier: snapping is held down through a drag, and the input this app reads
        // reports Shift, Control and Alt as held state and every other key as a press. A binding
        // to anything else is refused here rather than silently never firing.
        int snapModifier = 0;
        for (const makina::InputEvent& e : keymap.bindingsFor("snap.hold")) {
            if (e.key == "SHIFT")        { snapModifier |= makina::mods::kShift; }
            else if (e.key == "CONTROL") { snapModifier |= makina::mods::kCtrl; }
            else if (e.key == "ALT")     { snapModifier |= makina::mods::kAlt; }
            else {
                std::fprintf(stderr, "error: snap.hold is bound to '%s'; this build can only hold "
                                     "it on SHIFT, CONTROL or ALT\n", e.key.c_str());
                return 1;
            }
        }

        app::Window window(L"Makina viewport", 1280, 720);
        app::SwapchainDevice dev(window.handle(), 1280, 720);
        std::wprintf(L"adapter    : %ls\n", dev.adapterName().c_str());
        std::printf("keymap     : %s\n", keymap.name().c_str());

        const std::string outDir = scratchDir();
        const std::string hlslPath = outDir + "/viewport.gen.hlsl";
        const std::string vsPath = outDir + "/viewport_vs.cso";
        const std::string psPath = outDir + "/viewport_ps.cso";
        const spike::DxcPaths dxc{DXC_PATH, SPIKE_SHADER_DIR, MAKINA_CORE_INCLUDE};

        app::ComPtr<ID3D12RootSignature> rootSig = createRootSignature(dev.device());

        // Everything that depends on the tree, rebuilt whenever the tree changes.
        //
        // Generating and compiling costs 150-250 ms, which is what the modeller can afford once
        // per committed edit and a game cannot afford at all (docs/SPIKE_PERF.md 9). It is far too
        // slow to do per frame, which is what the interpreted pipeline below is for.
        makina::EvalProgram             prog;
        makina::BoundsResult            bounds;
        double                          sceneRadius = 1.0;
        app::ComPtr<ID3D12PipelineState> pso;
        std::unique_ptr<RingBuffer>      program;
        D3D12_GPU_VIRTUAL_ADDRESS        programAddress = 0;
        std::unique_ptr<RingBuffer>      materials;
        D3D12_GPU_VIRTUAL_ADDRESS        materialAddress = 0;
        std::uint32_t                    materialCount = 0;
        std::unique_ptr<RingBuffer>      pigments;
        D3D12_GPU_VIRTUAL_ADDRESS        pigmentAddress = 0;
        std::uint32_t                    pigmentCount = 0;
        std::unique_ptr<RingBuffer>      lights;
        D3D12_GPU_VIRTUAL_ADDRESS        lightAddress = 0;
        std::uint32_t                    lightCount = 0;

        // The tree that gets drawn. Edit.hpp honours the mute flag in exactly one place, and
        // this is where that place is called: everything downstream sees an ordinary scene.
        const auto visible = [](const makina::Scene& s) {
            return makina::hasMuted(s) ? makina::withoutMuted(s) : s;
        };

        auto rebuild = [&]() {
            prog = makina::flatten(visible(history.current()));
            bounds = makina::worldBounds(visible(history.current()));

            // Rebuilt with the tree because an edit can add or change a material. A scene with
            // none still gets one entry: the shader declares t1, and a root SRV that is declared
            // but never bound is undefined behaviour rather than an empty table.
            std::vector<makina::GpuMaterial> mats = makina::gpuMaterials(history.current());
            materialCount = history.current().materials.count;
            if (mats.empty()) {
                mats.push_back(makina::defaultGpuMaterial());
            }
            const std::size_t materialBytes = mats.size() * sizeof(makina::GpuMaterial);
            materials = std::make_unique<RingBuffer>(dev.device(), materialBytes);
            materialAddress = materials->write(mats.data(), materialBytes);

            // The program's table, not the scene's: a pattern is fixed in the space of the
            // solid wearing it, so one pattern on two solids in different places is two
            // entries and only the flatten knows the places.
            std::vector<makina::GpuPigment> pigs = prog.pigments;
            pigmentCount = static_cast<std::uint32_t>(pigs.size());
            if (pigs.empty()) {
                pigs.push_back(makina::GpuPigment{});
            }
            const std::size_t pigmentBytes = pigs.size() * sizeof(makina::GpuPigment);
            pigments = std::make_unique<RingBuffer>(dev.device(), pigmentBytes);
            pigmentAddress = pigments->write(pigs.data(), pigmentBytes);

            std::vector<makina::Light> lit;
            for (std::uint32_t i = 0; i < history.current().lights.count; ++i) {
                lit.push_back(history.current().lights[i]);
            }
            lightCount = history.current().lights.count;
            if (lit.empty()) {
                lit.push_back(makina::Light{});
            }
            const std::size_t lightBytes = lit.size() * sizeof(makina::Light);
            lights = std::make_unique<RingBuffer>(dev.device(), lightBytes);
            lightAddress = lights->write(lit.data(), lightBytes);

            sceneRadius = 1.0;
            if (bounds.box.valid) {
                double diag = 0.0;
                for (int i = 0; i < 3; ++i) {
                    const double span = bounds.box.hi[i] - bounds.box.lo[i];
                    diag += span * span;
                }
                sceneRadius = std::sqrt(diag) * 0.5;
                if (sceneRadius < 1e-6) {
                    sceneRadius = 1e-6;
                }
            }
            // Deleting the last solid is a legal thing to do. The generated shader has no form
            // when there is nothing to generate it from, so the old pipeline is kept and the draw
            // is skipped -- otherwise the frame would show the geometry that was just deleted.
            if (prog.nodes.empty()) {
                return;
            }

            {
                std::ofstream out(hlslPath, std::ios::binary);
                out << spike::generateShader(prog, "scene_shading.hlsl");
            }
            spike::compileHlsl(dxc, hlslPath, "vs_6_0", "VSMain", vsPath);
            spike::compileHlsl(dxc, hlslPath, "ps_6_0", "PSMain", psPath);

            const std::vector<char> vs = readBinary(vsPath);
            const std::vector<char> ps = readBinary(psPath);

            // The pipeline and the buffer about to be replaced may still be referenced by a frame
            // in flight. Releasing those is a device removal, not an error message.
            dev.waitForGpu();
            pso = createPipeline(dev.device(), rootSig.Get(), vs, ps);

            const std::size_t bytes = prog.nodes.size() * sizeof(makina::EvalNode);
            program = std::make_unique<RingBuffer>(dev.device(), bytes);
            programAddress = program->write(prog.nodes.data(), bytes);
        };
        rebuild();
        std::printf("%u authoring nodes -> %zu program nodes\n",
                    history.current().nodes.count, prog.nodes.size());

        // The preview pipeline: the same shading over an interpreted program instead of a
        // generated one. It is built once, because it does not depend on the scene -- the tree
        // travels in the buffer rather than in the source, which is exactly what makes it usable
        // while the mouse is still moving.
        //
        // It is the slower of the two (5.6x at 25 nodes, 11.4x at 75, docs/SPIKE_PERF.md), and
        // that is the right trade here: a frame that costs several times more still arrives this
        // frame, and a shader compile does not arrive until the drag is over.
        app::ComPtr<ID3D12PipelineState> interpretPso;
        std::unique_ptr<RingBuffer>      previewProgram;
        {
            const std::string ipath = outDir + "/viewport.interp.hlsl";
            const std::string ivs = outDir + "/viewport_interp_vs.cso";
            const std::string ips = outDir + "/viewport_interp_ps.cso";
            {
                std::ofstream out(ipath, std::ios::binary);
                out << spike::generateShader(prog, "scene_shading.hlsl", true);
            }
            spike::compileHlsl(dxc, ipath, "vs_6_0", "VSMain", ivs);
            spike::compileHlsl(dxc, ipath, "ps_6_0", "PSMain", ips);
            interpretPso =
                createPipeline(dev.device(), rootSig.Get(), readBinary(ivs), readBinary(ips));

            // Sized for the largest program the scene format allows, because the previewed tree
            // can be bigger than the committed one: moving a node that has no transform grows one.
            previewProgram = std::make_unique<RingBuffer>(
                dev.device(), makina::Scene::kMaxNodes * sizeof(makina::EvalNode));
        }
        std::printf("\n");

        // The frame constants are the same size whatever the scene, so this one outlives a rebuild.
        RingBuffer constants(dev.device(), sizeof(FrameParams));

        makina::Camera camera;
        camera = makina::frameBox(camera, bounds.box, 1280.0 / 720.0);
        // The camera from before the first axis snap, so "Genuine" has something to return
        // to. Written only when leaving the free camera, not every frame: while onAxis is
        // false the two would be the same, and copying them anyway invites one of the pair
        // to be forgotten in a branch added later.
        makina::Camera genuine = camera;
        bool onAxis = false;

        // A list, not an id, and every edit goes through topLevel() so a solid inside another
        // selected solid is reached once rather than twice.
        makina::Selection selection = scriptedSelection;
        // A rectangle in progress. Screen coordinates, because that is what the drag is in and
        // converting to world here would have to be undone to test against the film.
        bool   boxing = false;
        double boxStartU = 0.0;
        double boxStartV = 0.0;
        int           descendDepth = 0;
        std::uint32_t lastPickedId = 0;
        makina::TransformSession transform;
        std::string   lastStatus;
        // The tree as the frame should show it, which during a drag is not the committed one.
        // Everything the frame derives from the scene -- the program, the bounds, the ground, the
        // selection box -- has to come from the same tree, or the picture jumps on commit for
        // reasons the user did not ask for.
        makina::Scene       previewScene;
        makina::EvalProgram previewProg;
        bool                previewing = false;

        std::printf("orbit / pan / dolly per the keymap. F fits the selection, A fits everything.\n");
        std::printf("Escape quits.\n\n");

        int          frame = 0;
        std::size_t  scriptAt = 0;
        // The last frame's wall time, for the status bar. One frame rather than an average: the
        // question a modeller is asking is "did that edit just cost me something", and a running
        // mean smooths away the spike they are asking about.
        double lastFrameMs = 0.0;
        while (window.alive()) {
            const auto frameStart = std::chrono::steady_clock::now();
            app::FrameInput in = window.pump();
            int scriptedMods = makina::mods::kNone;
            if (scriptAt < scriptedKeys.size()) {
                // "CTRL+Z" rather than a bare key: undo and redo are the bindings most worth
                // driving from a script, and both are modified ones in every preset.
                std::string k = scriptedKeys[scriptAt++];
                for (std::size_t plus = k.find('+'); plus != std::string::npos;
                     plus = k.find('+')) {
                    const std::string m = k.substr(0, plus);
                    k.erase(0, plus + 1);
                    if (m == "CTRL") scriptedMods |= makina::mods::kCtrl;
                    else if (m == "SHIFT") scriptedMods |= makina::mods::kShift;
                    else if (m == "ALT") scriptedMods |= makina::mods::kAlt;
                    else std::fprintf(stderr, "warning: --keys: unknown modifier '%s'\n", m.c_str());
                }
                in.keysPressed.push_back(k);
            }
            if (window.consumeResize()) {
                dev.resize(in.width, in.height);
            }
            const double aspect = in.aspect();

            // A scripted run ignores the physical keyboard entirely. GetKeyState reports whichever
            // modifiers are held anywhere on the machine, so a Shift held while the check happens
            // to be running would stop CTRL+Y matching and the run would quietly not redo. That
            // showed up once as a check that failed and then passed.
            const bool live = scriptedKeys.empty();
            int modifiers = scriptedMods;
            if (live && in.shift) modifiers |= makina::mods::kShift;
            if (live && in.ctrl) modifiers |= makina::mods::kCtrl;
            if (live && in.alt) modifiers |= makina::mods::kAlt;

            // --- dragging ------------------------------------------------------------------
            if (in.dx != 0.0 || in.dy != 0.0) {
                makina::InputEvent e;
                e.modifiers = modifiers;
                e.dragging = true;
                e.button = in.leftDown     ? makina::MouseButton::Left
                           : in.middleDown ? makina::MouseButton::Middle
                           : in.rightDown  ? makina::MouseButton::Right
                                           : makina::MouseButton::None;

                if (e.button != makina::MouseButton::None) {
                    const makina::Action action = keymap.resolve(e);
                    if (action == "select.box") {
                        // The rectangle is remembered rather than acted on: what it selects is
                        // decided when the button comes up. Selecting every frame of the drag
                        // would make the selection whatever the cursor last passed over.
                        if (!boxing) {
                            boxing = true;
                            boxStartU = in.cursorU;
                            boxStartV = in.cursorV;
                        }
                    } else if (action == "view.orbit") {
                        camera = makina::orbit(camera, in.dx, in.dy);
                        onAxis = false;
                    } else if (action == "view.pan") {
                        // dy is inverted: screen y grows downward, the camera's up does not.
                        camera = makina::pan(camera, in.dx, -in.dy, aspect);
                        onAxis = false;
                    } else if (action == "view.dolly") {
                        camera = makina::dolly(camera, -in.dy * 20.0, sceneRadius);
                        onAxis = false;
                    }
                }
            }

            // The rectangle, resolved on release. A drag that stayed inside a few pixels is a
            // click that shook, and treating it as a rectangle would clear the selection the user
            // had just made.
            if (boxing && !in.leftDown) {
                boxing = false;
                const double du = in.cursorU - boxStartU;
                const double dv = in.cursorV - boxStartV;
                if (du * du + dv * dv > 1.0e-5) {
                    selection = makina::pickInRect(history.current(), camera, boxStartU, boxStartV,
                                                   in.cursorU, in.cursorV, aspect);
                    std::printf("rectangle selected %zu node(s)\n", selection.size());
                }
            }

            // --- wheel ---------------------------------------------------------------------
            if (in.wheel != 0.0) {
                camera = makina::dollyToCursor(camera, in.wheel, in.cursorU, in.cursorV, aspect,
                                               sceneRadius);
            }

            // --- clicking ------------------------------------------------------------------
            if (in.leftPressed || in.middlePressed || in.rightPressed) {
                makina::InputEvent e;
                e.modifiers = modifiers;
                e.button = in.leftPressed     ? makina::MouseButton::Left
                           : in.middlePressed ? makina::MouseButton::Middle
                                              : makina::MouseButton::Right;
                const makina::Action action = keymap.resolve(e);

                if (action == "select.pick" || action == "select.add" ||
                    action == "select.descend") {
                    // Clicking the same thing again with the descend modifier goes one level in;
                    // clicking anything else starts at the top again. Without the reset, a click
                    // on a different part would inherit a depth that means nothing there.
                    const makina::PickResult probe = makina::pickThroughCamera(
                        history.current(), camera, in.cursorU, in.cursorV, aspect, 0);
                    if (!probe.hit) {
                        // Clicking empty space clears, even with the add modifier held. Blender
                        // and Maya both do this, and the alternative -- a missed click leaving a
                        // selection of ten untouched -- reads as the click not registering.
                        selection.clear();
                        descendDepth = 0;
                        std::printf("selection cleared\n");
                    } else {
                        if (action == "select.descend" && probe.primitiveId == lastPickedId) {
                            ++descendDepth;
                        } else {
                            descendDepth = 0;
                        }
                        lastPickedId = probe.primitiveId;
                        const makina::PickResult r = makina::pickThroughCamera(
                            history.current(), camera, in.cursorU, in.cursorV, aspect, descendDepth);
                        selection = action == "select.add"
                                        ? makina::toggleSelected(selection, r.id)
                                        : makina::selectOnly(r.id);
                        const std::uint16_t index = makina::indexOfId(history.current(), r.id);
                        std::printf("selected id %u  %-16s  %s   (%d level(s) further in)\n", r.id,
                                    index == makina::kNoChild
                                        ? ""
                                        : history.current().nameOf(history.current().nodes[index]),
                                    index == makina::kNoChild
                                        ? ""
                                        : makina::opName(static_cast<makina::Op>(
                                              history.current().nodes[index].op)),
                                    r.remainingDepth);
                    }
                }
            }

            // --- a transform in progress ---------------------------------------------------
            //
            // Handled before anything else, because while one is running the same keys mean
            // different things: X is an axis, not delete, and Escape cancels rather than quits.
            //
            // Whether one was running is remembered, because the block below can end it: Escape
            // cancels a transform, and without this the same Escape would fall through to the key
            // loop and quit the application.
            const bool wasTransforming = transform.active();
            previewing = false;
            if (transform.active()) {
                // Through the keymap rather than off in.ctrl. Reading the modifier directly made
                // the "snap.hold" binding decorative: moving it to another key in a keymap file
                // changed nothing, and the app would have gone on snapping with Control while
                // claiming otherwise.
                transform.setSnap(live && (modifiers & snapModifier) != 0);
                if (in.dx != 0.0 || in.dy != 0.0) {
                    // One screen width is one unit for a move, ninety degrees for a rotate.
                    const double gain = transform.kind() == makina::TransformKind::Rotate ? 90.0
                                        : transform.kind() == makina::TransformKind::Scale ? 2.0
                                                                                           : 1.0;
                    transform.mouseDelta(in.dx * gain * sceneRadius /
                                         (transform.kind() == makina::TransformKind::Move
                                              ? 1.0
                                              : sceneRadius));
                }

                bool finish = false;
                bool abandon = false;
                for (const std::string& k : in.keysPressed) {
                    if (k == "ESCAPE") {
                        abandon = true;
                    } else if (k == "ENTER") {
                        finish = true;
                    } else if (k == "BACKSPACE") {
                        transform.backspace();
                    } else if (k.size() == 1 && transform.type(k[0])) {
                        // Consumed by the number.
                    } else {
                        makina::InputEvent e;
                        e.key = k;
                        e.modifiers = modifiers;
                        e.context = makina::KeyContext::Transform;
                        const makina::Action a = keymap.resolve(e);
                        if (a == "axis.x") transform.setAxis(makina::TransformAxis::X);
                        else if (a == "axis.y") transform.setAxis(makina::TransformAxis::Y);
                        else if (a == "axis.z") transform.setAxis(makina::TransformAxis::Z);
                    }
                }
                // Clicking confirms too, which is what a hand already on the mouse expects.
                if (in.leftPressed) {
                    finish = true;
                }

                const std::string status = transform.status();
                if (status != lastStatus) {
                    std::printf("  %s\n", status.c_str());
                    std::fflush(stdout);
                    lastStatus = status;
                }

                // The picture follows the number, every frame, through the interpreter. Applying
                // the transform to a copy rather than to the history is what makes this free to
                // throw away: a cancel has nothing to undo because nothing was ever committed.
                if (!abandon && !finish) {
                    const makina::EditResult p = transformSelection(
                        history.current(), selection, transform.kind(), transform.axis(),
                        transform.value());
                    if (p.ok) {
                        previewScene = p.scene;
                        previewProg = makina::flatten(visible(previewScene));
                        previewing = !previewProg.nodes.empty();
                    } else {
                        // A transform with no axis yet is refused, and that is not an error to
                        // report once per frame -- the header already reads "Move:" with no axis.
                        previewing = false;
                    }
                }

                if (abandon) {
                    // Nothing was applied, so there is nothing to put back. That is the whole
                    // reason the session holds no scene.
                    transform.cancel();
                    std::printf("\ncancelled\n");
                } else if (finish) {
                    const makina::EditResult r = transformSelection(
                        history.current(), selection, transform.kind(), transform.axis(),
                        transform.value());
                    if (r.ok) {
                        history.commit(r.scene, transform.status());
                        std::printf("\n%s -> committed\n", transform.status().c_str());
                        rebuild();
                    } else {
                        std::printf("\nrefused: %s\n", r.why.c_str());
                    }
                    transform.cancel();
                }
            }

            // --- keys ----------------------------------------------------------------------
            for (const std::string& k : in.keysPressed) {
                if (wasTransforming) {
                    break;
                }
                if (k == "ESCAPE") {
                    return 0;
                }
                makina::InputEvent e;
                e.key = k;
                e.modifiers = modifiers;
                const makina::Action action =
                    k.rfind('@', 0) == 0 ? k.substr(1) : keymap.resolve(e);

                if (action == "edit.move" || action == "edit.rotate" || action == "edit.scale") {
                    if (selection.empty()) {
                        std::printf("nothing selected\n");
                    } else {
                        transform.begin(action == "edit.move"     ? makina::TransformKind::Move
                                        : action == "edit.rotate" ? makina::TransformKind::Rotate
                                                                  : makina::TransformKind::Scale);
                        lastStatus.clear();
                    }
                    continue;
                }
                if (action == "edit.delete" || action == "edit.duplicate") {
                    if (selection.empty()) {
                        std::printf("nothing selected\n");
                        continue;
                    }
                    const bool removing = action == "edit.delete";
                    const makina::Selection targets =
                        makina::topLevel(history.current(), selection);
                    makina::Scene next = history.current();
                    makina::Selection copies;
                    bool refused = false;
                    for (const std::uint32_t id : targets) {
                        const makina::EditResult r = removing ? makina::removeSubtree(next, id)
                                                              : makina::duplicateSubtree(next, id);
                        if (!r.ok) {
                            std::printf("refused: %s\n", r.why.c_str());
                            refused = true;
                            break;
                        }
                        next = r.scene;
                        if (!removing) {
                            copies.push_back(r.newId);
                        }
                    }
                    if (refused) {
                        // Nothing is committed when one of them is refused. Half of a delete is
                        // worse than none: the user asked for one thing and would have to work out
                        // which half happened before they could undo it.
                        continue;
                    }
                    history.commit(next, (removing ? "delete " : "duplicate ") +
                                             std::to_string(targets.size()) + " node(s)");
                    if (removing) {
                        // The selection has to go with them. Leaving the ids behind would let the
                        // next W move something that is no longer in the tree, and the refusal
                        // would name an id the user cannot see.
                        std::printf("deleted %zu node(s)\n", targets.size());
                        selection.clear();
                        lastPickedId = 0;
                        descendDepth = 0;
                    } else {
                        // The copies become the selection, so a duplicate-then-move reads as one
                        // gesture rather than as moving the originals by mistake.
                        selection = copies;
                        std::printf("duplicated %zu node(s)\n", copies.size());
                    }
                    rebuild();
                    continue;
                }
                if (action == "edit.toggleMute") {
                    // Muted, not hidden. There is nothing to hide behind in a CSG tree: mute the
                    // cutter of a difference and the hole fills in, because the shape is the
                    // picture. Op.hpp says so where the flag is declared.
                    if (selection.empty()) {
                        // With nothing selected this brings everything back. There is no outliner
                        // yet, so a muted node cannot be clicked -- it is not drawn -- and a mute
                        // with no way back would be a delete wearing a friendlier name.
                        makina::Scene all = history.current();
                        int freed = 0;
                        for (std::uint32_t i = 0; i < all.nodes.count; ++i) {
                            if ((all.nodes[i].flags & makina::flags::kMuted) != 0) {
                                all.nodes[i].flags &= static_cast<std::uint16_t>(
                                    ~makina::flags::kMuted);
                                ++freed;
                            }
                        }
                        if (freed == 0) {
                            std::printf("nothing selected, and nothing is muted\n");
                            continue;
                        }
                        history.commit(all, "unmute everything");
                        std::printf("%d node(s) back in the solid\n", freed);
                        rebuild();
                        continue;
                    }
                    const makina::Selection targets =
                        makina::topLevel(history.current(), selection);
                    makina::Scene next = history.current();
                    int muted = 0;
                    for (const std::uint32_t id : targets) {
                        const std::uint16_t index = makina::indexOfId(next, id);
                        if (index == makina::kNoChild || index == 0) {
                            continue;
                        }
                        next.nodes[index].flags ^= makina::flags::kMuted;
                        if ((next.nodes[index].flags & makina::flags::kMuted) != 0) {
                            ++muted;
                        }
                    }
                    history.commit(next, "mute " + std::to_string(targets.size()) + " node(s)");
                    std::printf("%d of %zu node(s) now out of the solid\n", muted, targets.size());
                    rebuild();
                    continue;
                }
                if (action == "edit.undo") {
                    if (history.undo()) {
                        std::printf("undo\n");
                        rebuild();
                    } else {
                        std::printf("nothing to undo\n");
                    }
                    continue;
                }
                if (action == "edit.redo") {
                    if (history.redo()) {
                        std::printf("redo\n");
                        rebuild();
                    } else {
                        std::printf("nothing to redo\n");
                    }
                    continue;
                }

                // Framing is the user placing the camera by hand as much as orbiting is, so
                // it is the free camera from here on and "Genuine" should come back to it.
                if (action == "view.fitAll") {
                    camera = makina::frameBox(camera, bounds.box, aspect);
                    onAxis = false;
                } else if (action == "view.fitSelected") {
                    // Everything selected, not just the last one picked: fitting to one of five
                    // and calling it "fit selected" would leave the other four off screen.
                    const makina::Aabb box = selectionBox(history.current(), selection, bounds.box);
                    camera = makina::frameBox(camera, box, aspect);
                    onAxis = false;
                } else if (action == "view.front" || action == "view.back" ||
                           action == "view.right" || action == "view.left" ||
                           action == "view.top" || action == "view.bottom") {
                    // Stashed once. Snapping from Front to Right must not overwrite the
                    // free camera with the Front one, or "Genuine" returns to a view the
                    // user never framed.
                    if (!onAxis) {
                        genuine = camera;
                        onAxis = true;
                    }
                    if (action == "view.front") {
                        camera = makina::lookAlong(camera, makina::ViewAxis::Front);
                    } else if (action == "view.back") {
                        camera = makina::lookAlong(camera, makina::ViewAxis::Front, true);
                    } else if (action == "view.right") {
                        camera = makina::lookAlong(camera, makina::ViewAxis::Right);
                    } else if (action == "view.left") {
                        camera = makina::lookAlong(camera, makina::ViewAxis::Right, true);
                    } else if (action == "view.top") {
                        camera = makina::lookAlong(camera, makina::ViewAxis::Top);
                    } else if (action == "view.bottom") {
                        camera = makina::lookAlong(camera, makina::ViewAxis::Top, true);
                    }
                } else if (action == "view.toggleOrthographic") {
                    // onAxis is left alone. An axis view is usually looked at without
                    // perspective, so switching projection is part of being there rather
                    // than a way out of it.
                    camera = makina::setOrthographic(camera, !camera.orthographic);
                } else if (action == "view.genuine") {
                    if (onAxis) {
                        camera = genuine;
                        onAxis = false;
                    }
                } else if (action == "select.clear") {
                    selection.clear();
                }
            }

            // --- draw ----------------------------------------------------------------------
            // Everything below reads the tree through this, never through history directly.
            const makina::Scene& shown = previewing ? previewScene : history.current();
            const makina::BoundsResult shownBounds =
                previewing ? makina::worldBounds(shown) : bounds;
            double shownRadius = sceneRadius;
            if (previewing && shownBounds.box.valid) {
                double diag = 0.0;
                for (int i = 0; i < 3; ++i) {
                    const double span = shownBounds.box.hi[i] - shownBounds.box.lo[i];
                    diag += span * span;
                }
                shownRadius = std::sqrt(diag) * 0.5;
                if (shownRadius < 1e-6) {
                    shownRadius = 1e-6;
                }
            }

            FrameParams p{};
            double eye[3], fwd[3], right[3], up[3];
            makina::cameraEye(camera, eye);
            makina::cameraForward(camera, fwd);
            makina::cameraBasis(camera, right, up);
            for (int i = 0; i < 3; ++i) {
                p.eye[i] = static_cast<float>(eye[i]);
                p.forward[i] = static_cast<float>(fwd[i]);
                p.right[i] = static_cast<float>(right[i]);
                p.up[i] = static_cast<float>(up[i]);
                p.center[i] = static_cast<float>(camera.pivot[i]);
            }
            p.tanHalfFov = static_cast<float>(std::tan(camera.fovY * 3.14159265358979 / 360.0));
            p.aspect = static_cast<float>(aspect);
            p.nodeCount = shown.nodes.count;
            p.maxSteps = 192;
            p.stepScale = 0.85f;
            p.farDist = static_cast<float>(camera.distance + shownRadius * 2.5);
            p.enableAo = 1u;
            p.sceneRadius = static_cast<float>(shownRadius);
            p.groundY = shownBounds.box.valid ? static_cast<float>(shownBounds.box.lo[1])
                                              : static_cast<float>(-shownRadius);
            // The interpreter reads this; the generated shader has the count built into its code.
            p.programCount = static_cast<std::uint32_t>(
                previewing ? previewProg.nodes.size() : prog.nodes.size());
            // Not the previewed scene's: a transform never adds a material, and the buffer bound
            // below is the committed one. Counting the preview's would let the shader index past
            // the end of what is actually bound.
            p.materialCount = materialCount;
            p.pigmentCount = pigmentCount;
            p.lightCount = lightCount;

            float light[3] = {-0.45f, -0.78f, -0.44f};
            const float len =
                std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
            for (int i = 0; i < 3; ++i) {
                p.lightDir[i] = light[i] / len;
            }

            // The highlight is the selected subtree's box, not its surface. Approximate on
            // purpose: an exact one needs the shader to know which nodes belong to the selection,
            // and that is a change to the generated program rather than to a constant. Good
            // enough to answer "did my click land on what I meant", which is what step 4 is for.
            // Read from the previewed tree while a drag is running, so the box travels with what
            // is being moved instead of staying where the object used to be.
            if (!selection.empty()) {
                const makina::Aabb selBox = selectionBox(shown, selection, makina::Aabb{});
                const makina::BoundsResult sel{selBox, 0};
                if (sel.box.valid) {
                    for (int i = 0; i < 3; ++i) {
                        p.selMin[i] = static_cast<float>(sel.box.lo[i]);
                        p.selMax[i] = static_cast<float>(sel.box.hi[i]);
                    }
                    p.selValid = 1.0f;
                }
            }

            const D3D12_GPU_VIRTUAL_ADDRESS cbAddress = constants.write(&p, sizeof(p));

            const float clear[4] = {0.04f, 0.045f, 0.055f, 1.0f};
            dev.begin(clear);
            ID3D12GraphicsCommandList* cl = dev.list();
            cl->SetGraphicsRootSignature(rootSig.Get());
            cl->SetPipelineState(previewing ? interpretPso.Get() : pso.Get());
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cl->SetGraphicsRootConstantBufferView(0, cbAddress);
            if (previewing) {
                const std::size_t bytes = previewProg.nodes.size() * sizeof(makina::EvalNode);
                cl->SetGraphicsRootShaderResourceView(
                    1, previewProgram->write(previewProg.nodes.data(), bytes));
            } else {
                cl->SetGraphicsRootShaderResourceView(1, programAddress);
            }
            cl->SetGraphicsRootShaderResourceView(2, materialAddress);
            cl->SetGraphicsRootShaderResourceView(3, pigmentAddress);
            cl->SetGraphicsRootShaderResourceView(4, lightAddress);
            if (previewing || !prog.nodes.empty()) {
                cl->DrawInstanced(3, 1, 0, 0);
            }
            dev.end();

            lastFrameMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - frameStart)
                              .count();

            if (frameLimit > 0 && ++frame >= frameLimit) {
                break;
            }
        }

        if (!statePath.empty()) {
            // Built from the same three things the bridge will have: the tree as it stands, what
            // is selected, and the numbers the frame loop already keeps.
            makina::ViewNumbers numbers;
            numbers.distance = camera.distance;
            numbers.frameMs = lastFrameMs;
            numbers.live = transform.active() ? transform.status() : std::string();

            std::ofstream out(statePath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "warning: could not write the view state to '%s'\n",
                             statePath.c_str());
            } else {
                out << "{";
                bool first = true;
                for (const auto& kv : makina::viewState(history.current(), selection, numbers)) {
                    out << (first ? "" : ",") << "\n  " << makina::detail::jsonQuote(kv.first)
                        << ": ";
                    // A list is already JSON and goes in as it is; anything else is a string.
                    out << (kv.second.size() > 1 && kv.second.front() == '['
                                ? kv.second
                                : makina::detail::jsonQuote(kv.second));
                    first = false;
                }
                out << "\n}\n";
                std::printf("wrote %s\n", statePath.c_str());
            }
        }

        if (!savePath.empty()) {
            std::ofstream out(savePath, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "warning: could not write the scene to '%s'\n",
                             savePath.c_str());
            } else {
                out << makina::writeScene(history.current());
                std::printf("wrote %s\n", savePath.c_str());
            }
        }

        if (!screenshot.empty()) {
            int w = 0, h = 0, pitch = 0;
            const std::vector<std::uint8_t> pixels = dev.capture(w, h, pitch);
            std::string err;
            if (!spike::writeBmp(screenshot, pixels.data(), w, h, pitch, err)) {
                std::fprintf(stderr, "warning: %s\n", err.c_str());
            } else {
                std::printf("wrote %s\n", screenshot.c_str());
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
