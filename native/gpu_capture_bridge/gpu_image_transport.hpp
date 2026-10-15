#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <memory>
#include <cstdint>

namespace gpu_capture {
// Shared production transport, also exercised by the synthetic test.
// Single calling thread; no concurrent calls or concurrent use of Device11's context.
class GpuImageTransport {
public:
    GpuImageTransport(ID3D12Device*, ID3D12CommandQueue*, UINT width, UINT height);
    ~GpuImageTransport();
    GpuImageTransport(const GpuImageTransport&) = delete;
    GpuImageTransport& operator=(const GpuImageTransport&) = delete;
    ID3D11Device* Device11() const noexcept;
    // Source: same D3D11 device, BGRA UNORM, one mip/slice/sample, exact dimensions.
    // Keep source (and its WGC frame) alive and unchanged until ProducerComplete/Drain/Blit.
    void Submit(ID3D11Texture2D* source);
    bool ProducerComplete() const;
    void Drain(); // bounded completion of all submitted work
    // Target: caller's device, RT-capable 2D, single slice/mip/sample, in RENDER_TARGET.
    // Supports RGBA/BGRA UNORM and UNORM_SRGB, compatible typed/typeless resources.
    // Returns with all GPU work complete and target still in RENDER_TARGET.
    void Blit(ID3D12Resource*, DXGI_FORMAT);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
