// A DX12 device that draws to a window.
//
// The spike's device renders offscreen and reads back, which is what a comparison harness wants
// and exactly what an interactive viewport does not: it would stall on every frame. This one
// presents instead, and keeps enough frames in flight that the CPU is not waiting on the GPU to
// finish the frame it just submitted.
//
// Everything here is the minimum a single full-screen pass needs. No depth buffer -- the ray march
// resolves visibility itself, along the ray, which is one of the things that makes this renderer
// small. No descriptor heaps beyond the render targets, because the constants and the evaluation
// program are bound as root parameters.

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace app {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

class Dx12Error : public std::runtime_error {
public:
    explicit Dx12Error(const std::string& what) : std::runtime_error(what) {}
};

inline void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s failed (hr=0x%08lX)", what,
                      static_cast<unsigned long>(hr));
        throw Dx12Error(buf);
    }
}

/// Frames the CPU may run ahead of the GPU.
///
/// Three is the usual answer for a windowed swap chain: two lets the CPU stall waiting for the
/// present, and more than three adds latency between a mouse move and the picture without adding
/// throughput. Latency is the thing this viewport is most sensitive to.
constexpr std::uint32_t kFrameCount = 3;

class SwapchainDevice {
public:
    SwapchainDevice(HWND hwnd, int width, int height) : m_width(width), m_height(height) {
#ifdef _DEBUG
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
            }
        }
#endif
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory2");

        ComPtr<IDXGIAdapter1> adapter = pickAdapter();
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)),
              "D3D12CreateDevice");

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)), "CreateCommandQueue");

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.BufferCount = kFrameCount;
        sd.Width = static_cast<UINT>(width);
        sd.Height = static_cast<UINT>(height);
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swap1;
        check(m_factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &sd, nullptr, nullptr, &swap1),
              "CreateSwapChainForHwnd");
        // Alt+Enter would put the swap chain into an exclusive fullscreen the app does not manage,
        // and the first resize after that fails in a way that reads as a driver problem.
        m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        check(swap1.As(&m_swap), "IDXGISwapChain3 QueryInterface");

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = kFrameCount;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        check(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)), "CreateDescriptorHeap");
        m_rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        createRenderTargets();

        for (std::uint32_t i = 0; i < kFrameCount; ++i) {
            check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&m_alloc[i])),
                  "CreateCommandAllocator");
        }
        check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[0].Get(),
                                          nullptr, IID_PPV_ARGS(&m_list)),
              "CreateCommandList");
        check(m_list->Close(), "CommandList::Close");

        check(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)),
              "CreateFence");
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_event == nullptr) {
            throw Dx12Error("CreateEvent failed");
        }
        m_frame = m_swap->GetCurrentBackBufferIndex();
    }

    ~SwapchainDevice() {
        waitForGpu();
        if (m_event != nullptr) {
            CloseHandle(m_event);
        }
    }

    SwapchainDevice(const SwapchainDevice&) = delete;
    SwapchainDevice& operator=(const SwapchainDevice&) = delete;

    [[nodiscard]] ID3D12Device* device() const noexcept { return m_device.Get(); }

    /// The queue everything here is submitted on.
    ///
    /// Exposed for the shell: the engine's CEF layer uploads the page's pixels through a queue of
    /// its own choosing, and giving it a second one would put two producers on the same texture
    /// with nothing ordering them.
    [[nodiscard]] ID3D12CommandQueue* queue() const noexcept { return m_queue.Get(); }

    /// The render target this frame draws into.
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv() const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(m_frame) * m_rtvSize;
        return rtv;
    }
    [[nodiscard]] ID3D12GraphicsCommandList* list() const noexcept { return m_list.Get(); }
    [[nodiscard]] const std::wstring& adapterName() const noexcept { return m_adapterName; }
    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }

    /// Opens the command list for this frame's back buffer, cleared and ready to draw into.
    void begin(const float clear[4]) {
        // Waits only for the frame that used this slot, not for the one just submitted. Waiting
        // for the latest is the easy mistake and it serialises the CPU behind the GPU.
        waitForFrame(m_fenceValues[m_frame]);

        check(m_alloc[m_frame]->Reset(), "CommandAllocator::Reset");
        check(m_list->Reset(m_alloc[m_frame].Get(), nullptr), "CommandList::Reset");

        transition(m_targets[m_frame].Get(), D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = backBufferRtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        m_list->ClearRenderTargetView(rtv, clear, 0, nullptr);

        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height),
                          0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &scissor);
    }

    /// Closes, submits and presents.
    void end(bool vsync = true) {
        transition(m_targets[m_frame].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PRESENT);
        check(m_list->Close(), "CommandList::Close");

        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        check(m_swap->Present(vsync ? 1 : 0, 0), "Present");

        m_fenceValues[m_frame] = ++m_fenceValue;
        check(m_queue->Signal(m_fence.Get(), m_fenceValue), "Signal");
        m_frame = m_swap->GetCurrentBackBufferIndex();
    }

    /// Rebuilds the back buffers at a new size.
    ///
    /// The GPU has to be idle first: the buffers about to be released may still be referenced by a
    /// frame in flight, and releasing those is a device removal rather than an error.
    void resize(int width, int height) {
        if (width <= 0 || height <= 0 || (width == m_width && height == m_height)) {
            return;
        }
        waitForGpu();
        for (std::uint32_t i = 0; i < kFrameCount; ++i) {
            m_targets[i].Reset();
            m_fenceValues[i] = m_fenceValue;
        }
        check(m_swap->ResizeBuffers(kFrameCount, static_cast<UINT>(width),
                                    static_cast<UINT>(height), DXGI_FORMAT_R8G8B8A8_UNORM, 0),
              "ResizeBuffers");
        m_width = width;
        m_height = height;
        createRenderTargets();
        m_frame = m_swap->GetCurrentBackBufferIndex();
    }

    /// Copies the back buffer that was last presented into system memory.
    ///
    /// For a smoke test more than for the user: an interactive window cannot be looked at by
    /// anything automated, and "it compiled and did not crash" is not evidence that it drew
    /// anything. `rowPitch` comes back because the readback layout is padded.
    ///
    /// Stalls the GPU. That is fine for a screenshot and would not be for a per-frame path.
    std::vector<std::uint8_t> capture(int& widthOut, int& heightOut, int& rowPitch) {
        waitForGpu();

        // The frame index has already advanced past the one that was presented.
        const std::uint32_t shown = (m_frame + kFrameCount - 1) % kFrameCount;

        D3D12_RESOURCE_DESC desc = m_targets[shown]->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT64 totalBytes = 0;
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &totalBytes);

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = totalBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> readback;
        check(m_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&readback)),
              "CreateCommittedResource (readback)");

        check(m_alloc[m_frame]->Reset(), "CommandAllocator::Reset");
        check(m_list->Reset(m_alloc[m_frame].Get(), nullptr), "CommandList::Reset");

        transition(m_targets[shown].Get(), D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = m_targets[shown].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = layout;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        transition(m_targets[shown].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_PRESENT);
        check(m_list->Close(), "CommandList::Close");

        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        waitForGpu();

        widthOut = m_width;
        heightOut = m_height;
        rowPitch = static_cast<int>(layout.Footprint.RowPitch);

        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(totalBytes));
        void* mapped = nullptr;
        D3D12_RANGE all{0, static_cast<SIZE_T>(totalBytes)};
        check(readback->Map(0, &all, &mapped), "Map (readback)");
        std::memcpy(pixels.data(), mapped, static_cast<std::size_t>(totalBytes));
        D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        return pixels;
    }

    void waitForGpu() {
        if (!m_queue || !m_fence) {
            return;
        }
        const std::uint64_t target = ++m_fenceValue;
        check(m_queue->Signal(m_fence.Get(), target), "Signal");
        waitForFrame(target);
        for (std::uint32_t i = 0; i < kFrameCount; ++i) {
            m_fenceValues[i] = target;
        }
    }

private:
    ComPtr<IDXGIAdapter1> pickAdapter() {
        ComPtr<IDXGIAdapter1> best;
        SIZE_T bestVram = 0;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            if (m_factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                         __uuidof(ID3D12Device), nullptr))) {
                continue;
            }
            // Most video memory wins. On a laptop with both, that is the discrete part, which is
            // the one someone modelling would want.
            if (desc.DedicatedVideoMemory > bestVram) {
                bestVram = desc.DedicatedVideoMemory;
                best = candidate;
                m_adapterName = desc.Description;
            }
        }
        if (!best) {
            throw Dx12Error("no Direct3D 12 adapter found");
        }
        return best;
    }

    void createRenderTargets() {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t i = 0; i < kFrameCount; ++i) {
            check(m_swap->GetBuffer(i, IID_PPV_ARGS(&m_targets[i])), "GetBuffer");
            m_device->CreateRenderTargetView(m_targets[i].Get(), nullptr, rtv);
            rtv.ptr += m_rtvSize;
        }
    }

    void waitForFrame(std::uint64_t value) {
        if (m_fence->GetCompletedValue() >= value) {
            return;
        }
        check(m_fence->SetEventOnCompletion(value, m_event), "SetEventOnCompletion");
        WaitForSingleObject(m_event, INFINITE);
    }

    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_list->ResourceBarrier(1, &b);
    }

    ComPtr<IDXGIFactory4>             m_factory;
    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12CommandQueue>        m_queue;
    ComPtr<IDXGISwapChain3>           m_swap;
    ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;
    ComPtr<ID3D12Resource>            m_targets[kFrameCount];
    ComPtr<ID3D12CommandAllocator>    m_alloc[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<ID3D12Fence>               m_fence;

    HANDLE        m_event = nullptr;
    std::uint64_t m_fenceValue = 0;
    std::uint64_t m_fenceValues[kFrameCount]{};
    std::uint32_t m_frame = 0;
    UINT          m_rtvSize = 0;
    int           m_width = 0;
    int           m_height = 0;
    std::wstring  m_adapterName;
};

}  // namespace app
