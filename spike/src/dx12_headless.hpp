// Phase S spike: minimal headless DX12 setup.
//
// No window and no swap chain. One offscreen render target, one fullscreen draw, a readback,
// and a timestamp pair around the draw. That is everything the spike needs, and leaving the
// window out removes roughly half the boilerplate.

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace spike {

using Microsoft::WRL::ComPtr;

class Dx12Error : public std::runtime_error {
public:
    explicit Dx12Error(const std::string& what) : std::runtime_error(what) {}
};

inline void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s failed (hr=0x%08lX)", what, static_cast<unsigned long>(hr));
        throw Dx12Error(buf);
    }
}

// A GPU-local buffer plus its staging copy. Keeping the payload off the upload heap matters
// here: upload memory has different caching behaviour and would skew the measurement.
struct GpuBuffer {
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Resource> staging;
};

class HeadlessDevice {
public:
    HeadlessDevice(int width, int height, bool enableDebugLayer)
        : width_(width), height_(height) {
        if (enableDebugLayer) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
            }
        }

        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");
        pickAdapter();
        check(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
              "D3D12CreateDevice");

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");

        check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_)),
              "CreateCommandAllocator");
        check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(), nullptr,
                                         IID_PPV_ARGS(&list_)),
              "CreateCommandList");
        check(list_->Close(), "CommandList::Close");

        check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fenceEvent_ == nullptr) {
            throw Dx12Error("CreateEvent failed for the frame fence");
        }

        createRenderTarget();
        createTimestampQuery();
    }

    ~HeadlessDevice() {
        if (fenceEvent_ != nullptr) {
            CloseHandle(fenceEvent_);
        }
    }

    HeadlessDevice(const HeadlessDevice&) = delete;
    HeadlessDevice& operator=(const HeadlessDevice&) = delete;

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12GraphicsCommandList* list() const { return list_.Get(); }
    const std::wstring& adapterName() const { return adapterName_; }
    std::size_t adapterVramBytes() const { return vramBytes_; }

    // ---------------------------------------------------------------- resources

    GpuBuffer createBufferWithData(const void* data, std::size_t bytes, const wchar_t* debugName) {
        GpuBuffer out;

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        check(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&out.resource)),
              "CreateCommittedResource (default heap buffer)");
        check(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&out.staging)),
              "CreateCommittedResource (upload heap buffer)");

        if (debugName != nullptr) {
            out.resource->SetName(debugName);
        }

        void* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        check(out.staging->Map(0, &noRead, &mapped), "Map (upload heap buffer)");
        std::memcpy(mapped, data, bytes);
        out.staging->Unmap(0, nullptr);

        return out;
    }

    // ---------------------------------------------------------------- submission

    void beginFrame() {
        check(alloc_->Reset(), "CommandAllocator::Reset");
        check(list_->Reset(alloc_.Get(), nullptr), "CommandList::Reset");
    }

    void uploadBuffer(const GpuBuffer& buf, std::size_t bytes, D3D12_RESOURCE_STATES finalState) {
        transition(buf.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->CopyBufferRegion(buf.resource.Get(), 0, buf.staging.Get(), 0, bytes);
        transition(buf.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
    }

    void beginRenderTarget() {
        transition(renderTarget_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &sc);
    }

    void clearRenderTarget(const float color[4]) {
        list_->ClearRenderTargetView(rtvHeap_->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
    }

    void endRenderTarget() {
        transition(renderTarget_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    void writeTimestamp(UINT index) {
        list_->EndQuery(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    }

    void resolveTimestamps() {
        list_->ResolveQueryData(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                queryReadback_.Get(), 0);
    }

    void executeAndWait() {
        check(list_->Close(), "CommandList::Close");
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);

        const UINT64 target = ++fenceValue_;
        check(queue_->Signal(fence_.Get(), target), "CommandQueue::Signal");
        if (fence_->GetCompletedValue() < target) {
            check(fence_->SetEventOnCompletion(target, fenceEvent_), "Fence::SetEventOnCompletion");
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    // Milliseconds spent between the two timestamps of the last submitted frame.
    double lastGpuMilliseconds() {
        UINT64 freq = 0;
        check(queue_->GetTimestampFrequency(&freq), "GetTimestampFrequency");

        UINT64* stamps = nullptr;
        D3D12_RANGE range{0, sizeof(UINT64) * 2};
        check(queryReadback_->Map(0, &range, reinterpret_cast<void**>(&stamps)),
              "Map (timestamp readback)");
        const UINT64 begin = stamps[0];
        const UINT64 end = stamps[1];
        D3D12_RANGE noWrite{0, 0};
        queryReadback_->Unmap(0, &noWrite);

        if (freq == 0 || end <= begin) {
            return 0.0;
        }
        return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(freq);
    }

    // ---------------------------------------------------------------- readback

    void copyRenderTargetToReadback() {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = readbackFootprint_;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = renderTarget_.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    // Hands the caller the mapped readback rows. Valid until unmapReadback().
    const std::uint8_t* mapReadback(int& rowPitch) {
        void* mapped = nullptr;
        D3D12_RANGE range{0, readbackBytes_};
        check(readback_->Map(0, &range, &mapped), "Map (image readback)");
        rowPitch = static_cast<int>(readbackFootprint_.Footprint.RowPitch);
        return static_cast<const std::uint8_t*>(mapped);
    }

    void unmapReadback() {
        D3D12_RANGE noWrite{0, 0};
        readback_->Unmap(0, &noWrite);
    }

private:
    void pickAdapter() {
        ComPtr<IDXGIAdapter1> candidate;
        for (UINT i = 0; factory_->EnumAdapterByGpuPreference(
                             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                             IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND;
             ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                            __uuidof(ID3D12Device), nullptr))) {
                adapter_ = candidate;
                adapterName_ = desc.Description;
                vramBytes_ = desc.DedicatedVideoMemory;
                return;
            }
        }
        throw Dx12Error("no Direct3D 12 capable adapter was found");
    }

    void createRenderTarget() {
        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width_);
        desc.Height = static_cast<UINT>(height_);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = desc.Format;

        check(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_SOURCE, &clear,
                                               IID_PPV_ARGS(&renderTarget_)),
              "CreateCommittedResource (render target)");

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        check(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)), "CreateDescriptorHeap (RTV)");
        device_->CreateRenderTargetView(renderTarget_.Get(), nullptr,
                                        rtvHeap_->GetCPUDescriptorHandleForHeapStart());

        UINT64 total = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &readbackFootprint_, nullptr, nullptr, &total);
        readbackBytes_ = static_cast<std::size_t>(total);

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC rb{};
        rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb.Width = total;
        rb.Height = 1;
        rb.DepthOrArraySize = 1;
        rb.MipLevels = 1;
        rb.Format = DXGI_FORMAT_UNKNOWN;
        rb.SampleDesc.Count = 1;
        rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        check(device_->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rb,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback_)),
              "CreateCommittedResource (readback)");
    }

    void createTimestampQuery() {
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2;
        check(device_->CreateQueryHeap(&qh, IID_PPV_ARGS(&queryHeap_)), "CreateQueryHeap");

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC rb{};
        rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb.Width = sizeof(UINT64) * 2;
        rb.Height = 1;
        rb.DepthOrArraySize = 1;
        rb.MipLevels = 1;
        rb.Format = DXGI_FORMAT_UNKNOWN;
        rb.SampleDesc.Count = 1;
        rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        check(device_->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rb,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&queryReadback_)),
              "CreateCommittedResource (timestamp readback)");
    }

    void transition(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list_->ResourceBarrier(1, &b);
    }

    int width_ = 0;
    int height_ = 0;

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> alloc_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;

    ComPtr<ID3D12Resource> renderTarget_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12Resource> readback_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readbackFootprint_{};
    std::size_t readbackBytes_ = 0;

    ComPtr<ID3D12QueryHeap> queryHeap_;
    ComPtr<ID3D12Resource> queryReadback_;

    std::wstring adapterName_;
    std::size_t vramBytes_ = 0;
};

}  // namespace spike
