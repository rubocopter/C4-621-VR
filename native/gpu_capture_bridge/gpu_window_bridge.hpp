#pragma once
#include <windows.h>
#include <d3d12.h>
#include <memory>
#include <cstdint>

// Single-thread-affine. Caller initializes a WinRT MTA before construction and keeps
// that apartment alive through destruction. Construction starts capture of AC6 PID only.
class GpuWindowBridge {
public:
    GpuWindowBridge(DWORD gamePid, ID3D12Device*, ID3D12CommandQueue*);
    ~GpuWindowBridge();
    GpuWindowBridge(const GpuWindowBridge&) = delete;
    GpuWindowBridge& operator=(const GpuWindowBridge&) = delete;
    bool Update();
    void Blit(ID3D12Resource* xrTarget, DXGI_FORMAT targetFormat);
    UINT CaptureWidth() const noexcept;
    UINT CaptureHeight() const noexcept;
    std::uint64_t CaptureCounter() const noexcept;
    // WGC SystemRelativeTime: monotonic QPC-based 100ns ticks; zero before first frame.
    std::int64_t FreshnessTick() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
