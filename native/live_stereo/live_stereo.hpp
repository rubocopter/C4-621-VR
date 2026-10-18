#pragma once
#include <windows.h>
#include <d3d12.h>
#include <string>
#include <memory>
#include "../reprojection/projection.hpp"
class LiveStereo {
public:
    LiveStereo(ID3D12Device*,ID3D12CommandQueue*,const std::wstring& mappingName,float baselineGameUnits=0.5f,UINT eyeWidth=1280,UINT eyeHeight=720,
               reprojection::ProjectionCalibration calibration=reprojection::ProjectionCalibration::LegacyObservedAc6());
    ~LiveStereo();
    bool Update(); // newest shared frame; no CPU pixel readback
    bool HasFreshFrame() const noexcept; // current publication still usable; no GPU work
    void Blit(ID3D12Resource* xrTarget,DXGI_FORMAT format,unsigned eye);
    bool TryBlit(ID3D12Resource* target,DXGI_FORMAT format,unsigned eye); // false: source expired/unavailable; no GPU work
    UINT64 FrameId() const noexcept;
    UINT64 CaptureTick() const noexcept;
    UINT Width() const noexcept; // source
    UINT Height() const noexcept;
    UINT EyeWidth() const noexcept;
    UINT EyeHeight() const noexcept;
    const reprojection::ProjectionCalibration& Calibration() const noexcept;
    void SavePreview(const std::wstring& prefix); // optional test/debug only; RGBA raw EyeWidth() x EyeHeight() per eye
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
