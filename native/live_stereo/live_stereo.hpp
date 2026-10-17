#pragma once
#include <windows.h>
#include <d3d12.h>
#include <string>
#include <memory>
class LiveStereo {
public:
    LiveStereo(ID3D12Device*,ID3D12CommandQueue*,const std::wstring& mappingName,float baselineGameUnits=0.5f);
    ~LiveStereo();
    bool Update(); // newest shared frame; no CPU pixel readback
    void Blit(ID3D12Resource* xrTarget,DXGI_FORMAT format,unsigned eye);
    bool TryBlit(ID3D12Resource* target,DXGI_FORMAT format,unsigned eye); // false: source expired/unavailable; no GPU work
    UINT64 FrameId() const noexcept;
    UINT64 CaptureTick() const noexcept;
    UINT Width() const noexcept; // source
    UINT Height() const noexcept;
    void SavePreview(const std::wstring& prefix); // optional test/debug only; RGBA raw 1280x720/eye
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
