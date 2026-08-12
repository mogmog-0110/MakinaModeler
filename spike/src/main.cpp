// Phase S spike: measure the cost of evaluating a CSG tree on the GPU.
//
// Exit criteria from PLAN.md:
//   20 nodes / 720p / no AO   <= 16 ms
//   50 nodes / 720p / with AO <= 33 ms
//
// Run 1: interpreter only. First criterion met, second missed by 15%.
// Run 2: added per-node AABB culling. No effect -- the bounding test costs as much as the
//        primitive it skips.
// Run 3 (this one): interpreter vs a generated per-scene shader. DXIL showed the interpreter's
//        stack living in scratch (`alloca`), which generated straight-line SSA removes.
//        Compile time is measured too, since it decides whether a hybrid is viable.
//
// Headless: one offscreen render target, a timestamp pair around the draw, a BMP per config.

#include "csg_codegen.hpp"
#include "csg_flatten.hpp"
#include "dx12_headless.hpp"
#include "image_out.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Must match cbuffer Params in prelude.hlsl.
struct alignas(256) FrameParams {
    float eye[3];       float tanHalfFov;
    float forward[3];   float aspect;
    float right[3];     std::uint32_t nodeCount;
    float up[3];        std::uint32_t maxSteps;
    float lightDir[3];  float stepScale;
    float farDist;      std::uint32_t enableAo;  std::uint32_t enableCull;  float pad;
};

struct Config {
    int  primitiveCount;
    bool enableAo;
};

std::vector<char> readFile(const std::string& path) {
    std::FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr) {
        throw spike::Dx12Error("could not open '" + path + "'. Was the DXC build step run?");
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    std::vector<char> data(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);

    if (got != data.size()) {
        throw spike::Dx12Error("short read on '" + path + "'");
    }
    return data;
}

void normalize(float v[3]) {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 0.0f) {
        v[0] /= len;  v[1] /= len;  v[2] /= len;
    }
}

void cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

FrameParams makeCamera(int width, int height, std::uint32_t nodeCount, std::uint32_t maxSteps,
                       bool enableAo, bool enableCull) {
    FrameParams p{};
    const float eye[3] = {2.15f, 1.70f, 2.75f};
    const float target[3] = {0.0f, -0.05f, 0.0f};
    const float worldUp[3] = {0.0f, 1.0f, 0.0f};

    float fwd[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    normalize(fwd);

    float right[3];
    cross(fwd, worldUp, right);
    normalize(right);

    float up[3];
    cross(right, fwd, up);
    normalize(up);

    std::memcpy(p.eye, eye, sizeof(eye));
    std::memcpy(p.forward, fwd, sizeof(fwd));
    std::memcpy(p.right, right, sizeof(right));
    std::memcpy(p.up, up, sizeof(up));

    p.tanHalfFov = std::tan(40.0f * 0.5f * 3.14159265358979323846f / 180.0f);
    p.aspect = static_cast<float>(width) / static_cast<float>(height);
    p.nodeCount = nodeCount;
    p.maxSteps = maxSteps;

    float light[3] = {-0.45f, -0.78f, -0.44f};
    normalize(light);
    std::memcpy(p.lightDir, light, sizeof(light));

    // R-03: Difference is max(a,-b), a conservative lower bound, so a full step can tunnel
    // through a seam. Backing the step off is the standard guard.
    p.stepScale = 0.85f;
    p.farDist = 24.0f;
    p.enableAo = enableAo ? 1u : 0u;
    p.enableCull = enableCull ? 1u : 0u;
    return p;
}

spike::ComPtr<ID3D12RootSignature> createRootSignature(ID3D12Device* device) {
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;  // t0: headers
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 1;  // t1: payloads
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3;
    desc.pParameters = params;
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

spike::ComPtr<ID3D12PipelineState> createPipeline(ID3D12Device* device, ID3D12RootSignature* rs,
                                                  const std::string& vsPath, const std::string& psPath) {
    const std::vector<char> vs = readFile(vsPath);
    const std::vector<char> ps = readFile(psPath);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs;
    desc.VS = {vs.data(), vs.size()};
    desc.PS = {ps.data(), ps.size()};

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    spike::ComPtr<ID3D12PipelineState> pso;
    spike::check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)),
                 "CreateGraphicsPipelineState");
    return pso;
}

struct GeneratedShader {
    std::string csoPath;
    double      compileMilliseconds = 0.0;
};

// Writes a scene-specialised HLSL file and drives dxc.exe over it. Invoking the compiler as a
// process rather than through the API is crude, but it also measures the latency a hybrid
// scheme would actually pay.
GeneratedShader compileGenerated(const spike::FlatProgram& program, const std::string& shaderDir,
                                 const std::string& outDir, std::size_t tag,
                                 spike::CodegenMode mode) {
    const std::string suffix =
        (mode == spike::CodegenMode::Literals) ? "_lit" : "_struct";
    const std::string hlslPath = outDir + "/gen_" + std::to_string(tag) + suffix + ".hlsl";
    const std::string csoPath = outDir + "/gen_" + std::to_string(tag) + suffix + "_ps.cso";
    const std::string logPath = outDir + "/gen_" + std::to_string(tag) + suffix + ".log";

    {
        std::ofstream out(hlslPath, std::ios::binary);
        if (!out) {
            throw spike::Dx12Error("could not write the generated shader to '" + hlslPath + "'");
        }
        out << spike::generateShader(program, mode);
    }

    const std::string cmd = "\"\"" DXC_PATH "\" -T ps_6_0 -E PSMain -O3 -I \"" + shaderDir +
                            "\" -Fo \"" + csoPath + "\" \"" + hlslPath + "\" > \"" + logPath +
                            "\" 2>&1\"";

    const auto begin = std::chrono::steady_clock::now();
    const int rc = std::system(cmd.c_str());
    const auto end = std::chrono::steady_clock::now();

    if (rc != 0) {
        std::string detail;
        try {
            const std::vector<char> log = readFile(logPath);
            detail.assign(log.begin(), log.end());
        } catch (const std::exception&) {
            detail = "(no compiler output captured)";
        }
        throw spike::Dx12Error("dxc failed on the generated shader:\n" + detail);
    }

    GeneratedShader out;
    out.csoPath = csoPath;
    out.compileMilliseconds =
        std::chrono::duration<double, std::milli>(end - begin).count();
    return out;
}

double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::string dirOf(const std::string& path) {
    const std::size_t cut = path.find_last_of("\\/");
    return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
}

struct RunContext {
    spike::HeadlessDevice& dev;
    ID3D12RootSignature*   rootSig;
    int width;
    int height;
    int warmup;
    int iterations;
    std::uint32_t maxSteps;
    std::string   outDir;
};

// Renders one configuration and returns the median GPU milliseconds. The last frame is written
// out as a BMP so the two strategies can be compared visually as well as numerically.
double runConfig(RunContext& ctx, ID3D12PipelineState* pso, const spike::FlatProgram& program,
                 bool enableAo, const char* variant) {
    const std::size_t headerBytes = program.headers.size() * sizeof(spike::NodeHeader);
    const std::size_t payloadBytes = program.payloads.size() * sizeof(spike::NodePayload);

    const FrameParams params = makeCamera(ctx.width, ctx.height,
                                          static_cast<std::uint32_t>(program.headers.size()),
                                          ctx.maxSteps, enableAo, /*enableCull=*/false);

    spike::GpuBuffer headerBuf =
        ctx.dev.createBufferWithData(program.headers.data(), headerBytes, L"csg headers");
    spike::GpuBuffer payloadBuf =
        ctx.dev.createBufferWithData(program.payloads.data(), payloadBytes, L"csg payloads");
    spike::GpuBuffer cbBuf = ctx.dev.createBufferWithData(&params, sizeof(params), L"frame params");

    ctx.dev.beginFrame();
    ctx.dev.uploadBuffer(headerBuf, headerBytes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ctx.dev.uploadBuffer(payloadBuf, payloadBytes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ctx.dev.uploadBuffer(cbBuf, sizeof(params), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    ctx.dev.executeAndWait();

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(ctx.iterations));

    const int total = ctx.warmup + ctx.iterations;
    for (int frame = 0; frame < total; ++frame) {
        const bool lastFrame = (frame == total - 1);

        ctx.dev.beginFrame();
        ctx.dev.beginRenderTarget();

        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ctx.dev.clearRenderTarget(clear);

        ID3D12GraphicsCommandList* cl = ctx.dev.list();
        cl->SetGraphicsRootSignature(ctx.rootSig);
        cl->SetPipelineState(pso);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->SetGraphicsRootConstantBufferView(0, cbBuf.resource->GetGPUVirtualAddress());
        cl->SetGraphicsRootShaderResourceView(1, headerBuf.resource->GetGPUVirtualAddress());
        cl->SetGraphicsRootShaderResourceView(2, payloadBuf.resource->GetGPUVirtualAddress());

        ctx.dev.writeTimestamp(0);
        cl->DrawInstanced(3, 1, 0, 0);
        ctx.dev.writeTimestamp(1);

        ctx.dev.endRenderTarget();
        if (lastFrame) {
            ctx.dev.copyRenderTargetToReadback();
        }
        ctx.dev.resolveTimestamps();
        ctx.dev.executeAndWait();

        if (frame >= ctx.warmup) {
            samples.push_back(ctx.dev.lastGpuMilliseconds());
        }
    }

    int rowPitch = 0;
    const std::uint8_t* pixels = ctx.dev.mapReadback(rowPitch);
    char name[256];
    std::snprintf(name, sizeof(name), "%s/spike_%zu_%s_%s.bmp", ctx.outDir.c_str(),
                  program.headers.size(), enableAo ? "ao" : "noao", variant);
    std::string err;
    if (!spike::writeBmp(name, pixels, ctx.width, ctx.height, rowPitch, err)) {
        std::fprintf(stderr, "warning: %s\n", err.c_str());
    }
    ctx.dev.unmapReadback();

    return median(samples);
}

}  // namespace

int main(int argc, char** argv) {
    int width = 1280;
    int height = 720;
    std::uint32_t maxSteps = 128;
    int warmup = 5;
    int iterations = 30;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--width" && i + 1 < argc)           width = std::atoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc)     height = std::atoi(argv[++i]);
        else if (a == "--steps" && i + 1 < argc)      maxSteps = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--iterations" && i + 1 < argc) iterations = std::atoi(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc)     warmup = std::atoi(argv[++i]);
    }

    const std::string exeDir = dirOf(argv[0]);

    try {
        spike::HeadlessDevice dev(width, height, /*enableDebugLayer=*/false);

        std::printf("Makina Phase S spike -- interpreter vs generated shader\n");
        std::printf("adapter    : %ls\n", dev.adapterName().c_str());
        std::printf("dedicated  : %zu MB\n", dev.adapterVramBytes() / (1024 * 1024));
        std::printf("resolution : %dx%d, max %u sphere-trace steps\n", width, height, maxSteps);
        std::printf("note       : first run of a session reads ~8%% high; spread is +-4%% after that\n\n");

        auto rootSig = createRootSignature(dev.device());
        auto interpPso = createPipeline(dev.device(), rootSig.Get(), exeDir + "/raymarch_vs.cso",
                                        exeDir + "/raymarch_ps.cso");

        RunContext ctx{dev, rootSig.Get(), width, height, warmup, iterations, maxSteps, exeDir};

        const Config configs[] = {
            {11, false}, {11, true},
            {26, false}, {26, true},
            {51, false}, {51, true},
        };

        // Everything is prepared before any timing starts. Interleaving dxc with measurement was
        // enough to corrupt an earlier run: it showed 51-node/no-AO slower than 51-node/with-AO,
        // which cannot happen, and the compile times themselves had tripled.
        // No cached label pointer here: an earlier version kept `const char* label` next to a
        // `char labelBuf[64]` in the same struct, and moving the Case into the vector left the
        // pointer aimed at the moved-from buffer, so every row printed the last case's name.
        // The label is formatted at print time instead.
        struct Case {
            spike::FlatProgram  program;
            bool                enableAo;
            double              compileStructMs;
            double              compileLitMs;
            spike::ComPtr<ID3D12PipelineState> structPso;
            spike::ComPtr<ID3D12PipelineState> litPso;
            double best[3];   // interp, struct, literal
        };

        std::vector<Case> cases;
        cases.reserve(sizeof(configs) / sizeof(configs[0]));

        std::printf("preparing (compiling %zu generated shaders)...\n",
                    (sizeof(configs) / sizeof(configs[0])) * 2);

        for (const Config& cfg : configs) {
            Case c{};
            c.program = spike::flatten(spike::buildMechanicalPart(cfg.primitiveCount));
            c.enableAo = cfg.enableAo;

            const GeneratedShader genStruct =
                compileGenerated(c.program, SPIKE_SHADER_DIR, exeDir, c.program.headers.size(),
                                 spike::CodegenMode::StructureOnly);
            const GeneratedShader genLit =
                compileGenerated(c.program, SPIKE_SHADER_DIR, exeDir, c.program.headers.size(),
                                 spike::CodegenMode::Literals);

            c.compileStructMs = genStruct.compileMilliseconds;
            c.compileLitMs = genLit.compileMilliseconds;
            c.structPso = createPipeline(dev.device(), rootSig.Get(),
                                         exeDir + "/raymarch_vs.cso", genStruct.csoPath);
            c.litPso = createPipeline(dev.device(), rootSig.Get(),
                                      exeDir + "/raymarch_vs.cso", genLit.csoPath);

            for (double& b : c.best) {
                b = 1e30;
            }
            cases.push_back(std::move(c));
        }

        // Minimum over whole sweeps rather than a single pass. Thermal drift only ever adds time,
        // so the minimum is the reading least polluted by it, and sweeping means no configuration
        // sits permanently in the hottest slot.
        const int sweeps = 3;
        std::printf("measuring (%d sweeps, reporting the minimum)...\n\n", sweeps);

        for (int s = 0; s < sweeps; ++s) {
            for (Case& c : cases) {
                ID3D12PipelineState* psos[3] = {interpPso.Get(), c.structPso.Get(), c.litPso.Get()};
                const char* names[3] = {"interp", "struct", "literal"};
                for (int v = 0; v < 3; ++v) {
                    const double t = runConfig(ctx, psos[v], c.program, c.enableAo, names[v]);
                    if (t < c.best[v]) {
                        c.best[v] = t;
                    }
                }
            }
        }

        std::printf("%-18s %6s %9s %9s %9s   %s\n", "config", "nodes",
                    "interp", "struct", "literal", "speedup vs interp");
        std::printf("%-18s %6s %9s %9s %9s   %s\n", "------", "-----",
                    "------", "------", "-------", "-----------------");

        double compileStructTotal = 0.0;
        double compileLitTotal = 0.0;

        for (const Case& c : cases) {
            compileStructTotal += c.compileStructMs;
            compileLitTotal += c.compileLitMs;
            char label[64];
            std::snprintf(label, sizeof(label), "%zu nodes, %s", c.program.headers.size(),
                          c.enableAo ? "AO" : "no AO");
            std::printf("%-18s %6zu %9.2f %9.2f %9.2f   struct %.2fx / literal %.2fx\n",
                        label, c.program.headers.size(), c.best[0], c.best[1], c.best[2],
                        c.best[1] > 0.0 ? c.best[0] / c.best[1] : 0.0,
                        c.best[2] > 0.0 ? c.best[0] / c.best[2] : 0.0);
        }

        if (!cases.empty()) {
            std::printf("\nmean dxc compile: struct %.0f ms, literal %.0f ms\n",
                        compileStructTotal / cases.size(), compileLitTotal / cases.size());
        }

        std::printf("\nexit criteria (PLAN.md Phase S):\n");
        std::printf("  20 nodes / no AO  <= 16.00 ms\n");
        std::printf("  50 nodes / AO     <= 33.00 ms\n");
        return 0;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nspike failed: %s\n", e.what());
        return 1;
    }
}
