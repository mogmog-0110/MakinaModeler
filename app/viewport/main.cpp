// The Makina viewport: a window you can orbit, frame and click in.
//
// Step 4 of docs/APP_DESIGN.md. No HTML yet -- this is the half of Phase 3 that does not need CEF,
// and it is deliberately the half that comes first, because the feel of a viewport is decided by
// the camera and by picking and both of those are already tested headless (Camera.hpp, Pick.hpp,
// Keymap.hpp). What is added here is only the wiring: messages in, a frame out.
//
//   makina_viewport <scene.makina.json> [--keymap maya|blender]
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
#include <makina/SceneJson.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
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
    std::uint32_t programCount;  std::uint32_t pad0[3];
    float selMin[3];    float selValid;
    float selMax[3];    float pad1;
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
        throw std::runtime_error("could not open '" + path + "'");
    }
    return std::vector<char>((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
}

std::string dirOf(const std::string& path) {
    const std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
}

app::ComPtr<ID3D12RootSignature> createRootSignature(ID3D12Device* device) {
    // b0 for the frame, t0 for the evaluation program -- the same signature the offscreen renderer
    // uses, so the generated shaders are interchangeable between them.
    D3D12_ROOT_PARAMETER param[2]{};
    param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param[0].Descriptor.ShaderRegister = 0;
    param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    param[1].Descriptor.ShaderRegister = 0;
    param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
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
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--keymap" && i + 1 < argc) {
            keymapName = argv[++i];
        } else if (a == "--frames" && i + 1 < argc) {
            frameLimit = std::atoi(argv[++i]);
        } else if (a == "--screenshot" && i + 1 < argc) {
            screenshot = argv[++i];
        } else {
            scenePath = a;
        }
    }
    if (scenePath.empty()) {
        std::fprintf(stderr,
                     "usage: makina_viewport <scene.makina.json> [--keymap maya|blender]\n");
        return 2;
    }

    try {
        const makina::Scene scene = makina::parseScene(readFile(scenePath));
        const makina::EvalProgram prog = makina::flatten(scene);
        if (prog.nodes.empty()) {
            std::fprintf(stderr, "error: '%s' has nothing renderable in it\n", scenePath.c_str());
            return 1;
        }
        const makina::BoundsResult bounds = makina::worldBounds(scene);

        makina::Keymap keymap;
        std::string keymapError;
        const char* keymapJson =
            keymapName == "blender" ? makina::blenderKeymapJson() : makina::mayaKeymapJson();
        if (!keymap.load(keymapJson, keymapError)) {
            std::fprintf(stderr, "error: the built-in keymap did not load: %s\n",
                         keymapError.c_str());
            return 1;
        }

        app::Window window(L"Makina viewport", 1280, 720);
        app::SwapchainDevice dev(window.handle(), 1280, 720);
        std::wprintf(L"adapter    : %ls\n", dev.adapterName().c_str());
        std::printf("keymap     : %s\n", keymap.name().c_str());
        std::printf("%u authoring nodes -> %zu program nodes\n\n", scene.nodes.count,
                    prog.nodes.size());

        // Compiled once, here, because the scene does not change yet. When editing arrives this
        // becomes "recompile on edit", which is the 150-250 ms the modeller can afford and the
        // engine cannot (docs/SPIKE_PERF.md 9).
        const std::string outDir = dirOf(scenePath);
        const std::string hlslPath = outDir + "/viewport.gen.hlsl";
        {
            std::ofstream out(hlslPath, std::ios::binary);
            out << spike::generateShader(prog, "scene_shading.hlsl");
        }
        const spike::DxcPaths dxc{DXC_PATH, SPIKE_SHADER_DIR, MAKINA_CORE_INCLUDE};
        const std::string vsPath = outDir + "/viewport_vs.cso";
        const std::string psPath = outDir + "/viewport_ps.cso";
        spike::compileHlsl(dxc, hlslPath, "vs_6_0", "VSMain", vsPath);
        spike::compileHlsl(dxc, hlslPath, "ps_6_0", "PSMain", psPath);

        const std::vector<char> vs = readBinary(vsPath);
        const std::vector<char> ps = readBinary(psPath);

        app::ComPtr<ID3D12RootSignature> rootSig = createRootSignature(dev.device());

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rootSig.Get();
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
        app::check(dev.device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)),
                   "CreateGraphicsPipelineState");

        RingBuffer constants(dev.device(), sizeof(FrameParams));

        // The program never changes while the scene is fixed, so one buffer on the upload heap is
        // enough. Editing will move this to the ring alongside the constants.
        RingBuffer program(dev.device(), prog.nodes.size() * sizeof(makina::EvalNode));
        const D3D12_GPU_VIRTUAL_ADDRESS programAddress =
            program.write(prog.nodes.data(), prog.nodes.size() * sizeof(makina::EvalNode));

        double sceneRadius = 1.0;
        if (bounds.box.valid) {
            double diag = 0.0;
            for (int i = 0; i < 3; ++i) {
                const double span = bounds.box.hi[i] - bounds.box.lo[i];
                diag += span * span;
            }
            sceneRadius = std::sqrt(diag) * 0.5;
        }

        makina::Camera camera;
        camera = makina::frameBox(camera, bounds.box, 1280.0 / 720.0);

        std::uint32_t selectedId = 0;
        int           descendDepth = 0;
        std::uint32_t lastPickedId = 0;

        std::printf("orbit / pan / dolly per the keymap. F fits the selection, A fits everything.\n");
        std::printf("Escape quits.\n\n");

        int frame = 0;
        while (window.alive()) {
            const app::FrameInput in = window.pump();
            if (window.consumeResize()) {
                dev.resize(in.width, in.height);
            }
            const double aspect = in.aspect();

            int modifiers = makina::mods::kNone;
            if (in.shift) modifiers |= makina::mods::kShift;
            if (in.ctrl) modifiers |= makina::mods::kCtrl;
            if (in.alt) modifiers |= makina::mods::kAlt;

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
                    if (action == "view.orbit") {
                        camera = makina::orbit(camera, in.dx, in.dy);
                    } else if (action == "view.pan") {
                        // dy is inverted: screen y grows downward, the camera's up does not.
                        camera = makina::pan(camera, in.dx, -in.dy, aspect);
                    } else if (action == "view.dolly") {
                        camera = makina::dolly(camera, -in.dy * 20.0, sceneRadius);
                    }
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

                if (action == "select.pick" || action == "select.descend") {
                    // Clicking the same thing again with the descend modifier goes one level in;
                    // clicking anything else starts at the top again. Without the reset, a click
                    // on a different part would inherit a depth that means nothing there.
                    const makina::PickResult probe = makina::pickThroughCamera(
                        scene, camera, in.cursorU, in.cursorV, aspect, 0);
                    if (!probe.hit) {
                        selectedId = 0;
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
                            scene, camera, in.cursorU, in.cursorV, aspect, descendDepth);
                        selectedId = r.id;
                        const std::uint16_t index = makina::indexOfId(scene, r.id);
                        std::printf("selected id %u  %-16s  %s   (%d level(s) further in)\n", r.id,
                                    index == makina::kNoChild ? "" : scene.nameOf(scene.nodes[index]),
                                    index == makina::kNoChild
                                        ? ""
                                        : makina::opName(
                                              static_cast<makina::Op>(scene.nodes[index].op)),
                                    r.remainingDepth);
                    }
                }
            }

            // --- keys ----------------------------------------------------------------------
            for (const std::string& k : in.keysPressed) {
                if (k == "ESCAPE") {
                    return 0;
                }
                makina::InputEvent e;
                e.key = k;
                e.modifiers = modifiers;
                const makina::Action action = keymap.resolve(e);

                if (action == "view.fitAll") {
                    camera = makina::frameBox(camera, bounds.box, aspect);
                } else if (action == "view.fitSelected") {
                    const std::uint16_t index = makina::indexOfId(scene, selectedId);
                    const makina::Aabb box =
                        index == makina::kNoChild ? bounds.box : makina::worldBounds(scene, index).box;
                    camera = makina::frameBox(camera, box, aspect);
                } else if (action == "view.front") {
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
                } else if (action == "view.toggleOrthographic") {
                    camera = makina::setOrthographic(camera, !camera.orthographic);
                } else if (action == "select.clear") {
                    selectedId = 0;
                }
            }

            // --- draw ----------------------------------------------------------------------
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
            p.nodeCount = scene.nodes.count;
            p.maxSteps = 192;
            p.stepScale = 0.85f;
            p.farDist = static_cast<float>(camera.distance + sceneRadius * 2.5);
            p.enableAo = 1u;
            p.sceneRadius = static_cast<float>(sceneRadius);
            p.groundY = bounds.box.valid ? static_cast<float>(bounds.box.lo[1])
                                         : static_cast<float>(-sceneRadius);
            p.programCount = static_cast<std::uint32_t>(prog.nodes.size());

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
            const std::uint16_t selIndex = makina::indexOfId(scene, selectedId);
            if (selIndex != makina::kNoChild) {
                const makina::BoundsResult sel = makina::worldBounds(scene, selIndex);
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
            cl->SetPipelineState(pso.Get());
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cl->SetGraphicsRootConstantBufferView(0, cbAddress);
            cl->SetGraphicsRootShaderResourceView(1, programAddress);
            cl->DrawInstanced(3, 1, 0, 0);
            dev.end();

            if (frameLimit > 0 && ++frame >= frameLimit) {
                break;
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
