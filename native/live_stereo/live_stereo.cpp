#include "live_stereo.hpp"
#include "../live_pair_stream/protocol.hpp"
#include "shader.hpp"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT hr,const char* operation) {
    if(FAILED(hr)) { char message[256];std::snprintf(message,sizeof(message),"%s (HRESULT 0x%08lX)",operation,static_cast<unsigned long>(hr));throw std::runtime_error(message); }
}
struct Handle { HANDLE value=nullptr;~Handle() { if(value) CloseHandle(value); } };
struct View { live_pair::Header* value=nullptr;~View() { if(value) UnmapViewOfFile(value); } };
struct Claim {
    live_pair::Header* header=nullptr;
    ~Claim() { if(header) InterlockedCompareExchange(&header->consumer_pid,0,LONG(GetCurrentProcessId())); }
};
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};return b;
}
bool SameLayout(const live_pair::Header& a,const live_pair::Header& b) {
    return a.version==b.version&&a.producer_pid==b.producer_pid&&a.adapter.LowPart==b.adapter.LowPart&&a.adapter.HighPart==b.adapter.HighPart&&
        a.width==b.width&&a.height==b.height&&a.color_format==b.color_format&&a.depth_pitch==b.depth_pitch&&a.color_pitch==b.color_pitch&&
        a.depth_offset==b.depth_offset&&a.color_offset==b.color_offset&&a.buffer_bytes==b.buffer_bytes;
}
struct Params { UINT sw,sh,depthOffset,depthPitch,colorOffset,colorPitch,colorFormat;float eyeX;UINT tw,th;float fx,a,b,wsign,scaleX,scaleY,offsetX,offsetY; };
static_assert(sizeof(Params)==72&&offsetof(Params,fx)==40&&offsetof(Params,wsign)==52&&offsetof(Params,scaleX)==56&&offsetof(Params,offsetY)==68);
ComPtr<ID3DBlob> Compile(const char* entry,bool distinctIntrinsics) {
    ComPtr<ID3DBlob> code,errors;
    const D3D_SHADER_MACRO macros[]={{"AC6_DISTINCT_INTRINSICS","1"},{nullptr,nullptr}};
    HRESULT hr=D3DCompile(LiveStereoHlsl,sizeof(LiveStereoHlsl)-1,"live_stereo",distinctIntrinsics?macros:nullptr,nullptr,entry,"cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors);
    if(FAILED(hr)&&errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
    Check(hr,"Compile compute shader");return code;
}
}
struct LiveStereo::Impl {
    // Declaration order retains the mapping through Claim destruction.
    Handle mapping;
    View view;
    Claim claim;
    live_pair::Header layout{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    std::array<ComPtr<ID3D12Resource>,3> sources;
    ComPtr<ID3D12Fence> ready,released,completion;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,4> passes;
    ComPtr<ID3D12Resource> closest,winner;
    std::array<ComPtr<ID3D12Resource>,2> rgba,bgra;
    Handle event;
    UINT64 timeline=0,last=0,tick=0,releasedFloor=0;
    const UINT eyeWidth,eyeHeight,eyePixels;
    const UINT64 eyeBytes;
    float baseline;
    const reprojection::ProjectionCalibration calibration;
    std::array<reprojection::PixelTransform,2> imageTransforms;
    // Queue waits cannot be cancelled. Never unwind mappings, source buffers,
    // or caller targets after an in-flight failure; only this process exits.
    [[noreturn]] void InFlightFailure(const char* reason) const noexcept {
        std::fprintf(stderr,"LiveStereo: %s; D3D12 removal=0x%08lX; terminating standalone process with GPU resources retained\n",reason,static_cast<unsigned long>(device->GetDeviceRemovedReason()));
        std::fflush(stderr);TerminateProcess(GetCurrentProcess(),3);ExitProcess(3);
    }
    void Drain() const noexcept {
        if(!timeline) return;
        auto completed=completion->GetCompletedValue();
        if(completed==UINT64_MAX) InFlightFailure("device removed");
        if(completed>=timeline) return;
        if(FAILED(completion->SetEventOnCompletion(timeline,event.value))) InFlightFailure("SetEventOnCompletion");
        if(WaitForSingleObject(event.value,5000)!=WAIT_OBJECT_0) InFlightFailure("GPU completion timeout/failure (5000ms)");
        completed=completion->GetCompletedValue();
        if(completed==UINT64_MAX||completed<timeline) InFlightFailure("GPU completion failed");
    }
    ~Impl() { Drain(); } // PID clears after GPU completion, before unmapping
    bool StreamValid() const {
        const auto status=live_pair::Read32(view.value->status);
        return (status==live_pair::Ready||status==live_pair::Stopped)&&SameLayout(*view.value,layout);
    }
    void Begin() {
        Drain();Check(allocator->Reset(),"Reset command allocator");Check(list->Reset(allocator.Get(),nullptr),"Reset command list");
    }
    void Submit(UINT64 releaseId=0) {
        Check(list->Close(),"Close command list");
        if(releaseId&&FAILED(queue->Wait(ready.Get(),releaseId))) InFlightFailure("queue Wait ready");
        ID3D12CommandList* commands[]={list.Get()};queue->ExecuteCommandLists(1,commands);
        if(releaseId&&FAILED(queue->Signal(released.Get(),releaseId))) InFlightFailure("queue Signal released");
        if(FAILED(queue->Signal(completion.Get(),++timeline))) InFlightFailure("queue Signal completion");
        Drain();
    }
    void Barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
        auto barrier=Transition(r,a,b);list->ResourceBarrier(1,&barrier);
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,bool uav=false) {
        D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> result;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&result)),"Create buffer");return result;
    }
    template<class T> void Open(const std::wstring& name,ComPtr<T>& result) {
        Handle handle;Check(device->OpenSharedHandleByName(name.c_str(),GENERIC_ALL,&handle.value),"OpenSharedHandleByName");
        Check(device->OpenSharedHandle(handle.value,IID_PPV_ARGS(&result)),"OpenSharedHandle");
    }
    Impl(ID3D12Device* d,ID3D12CommandQueue* q,const std::wstring& name,float base,UINT width,UINT height,reprojection::ProjectionCalibration projection):device(d),queue(q),eyeWidth(width),eyeHeight(height),eyePixels(width*height),eyeBytes(4ull*width*height),baseline(base),calibration(projection) {
        if(!((width==1280&&height==720)||(width==1920&&height==1080)||(width==2560&&height==1440)))
            throw std::invalid_argument("Unsupported eye resolution; use 1280x720, 1920x1080, or 2560x1440");
        calibration.Validate(); // Before opening shared state or allocating GPU resources.
        imageTransforms={calibration.Transform(0),calibration.Transform(1)};
        if(!d||!q||name.empty()||!std::isfinite(base)||base<0) throw std::invalid_argument("Invalid device, queue, mapping name, or baseline");
        ComPtr<ID3D12Device> owner;Check(q->GetDevice(IID_PPV_ARGS(&owner)),"queue GetDevice");
        if(owner.Get()!=d||q->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT||q->GetDesc().NodeMask>1||d->GetNodeCount()!=1)
            throw std::invalid_argument("LiveStereo requires caller device's DIRECT queue on a single-node adapter");
        mapping.value=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,name.c_str());
        if(!mapping.value) Check(HRESULT_FROM_WIN32(GetLastError()),"OpenFileMapping READ|WRITE");
        view.value=static_cast<live_pair::Header*>(MapViewOfFile(mapping.value,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(live_pair::Header)));
        if(!view.value) Check(HRESULT_FROM_WIN32(GetLastError()),"MapViewOfFile header");
        auto status=live_pair::Read32(view.value->status);
        if(status!=live_pair::Ready&&status!=live_pair::Stopped) throw std::runtime_error("Stream is not Ready or Stopped");
        layout=*view.value;
        if(!live_pair::Valid(layout)) throw std::runtime_error("Invalid shared header/version/plane ranges");
        auto luid=d->GetAdapterLuid();
        if(luid.LowPart!=layout.adapter.LowPart||luid.HighPart!=layout.adapter.HighPart) throw std::runtime_error("Shared stream adapter LUID mismatch");
        if(InterlockedCompareExchange(&view.value->consumer_pid,LONG(GetCurrentProcessId()),0)!=0) throw std::runtime_error("Stream already has a consumer");
        claim.header=view.value;
        if(!StreamValid()) throw std::runtime_error("Stream changed while claiming");
        for(UINT n=0;n<3;++n) {
            Open(name+L"_buffer"+std::to_wstring(n),sources[n]);auto desc=sources[n]->GetDesc();
            D3D12_HEAP_PROPERTIES hp{};D3D12_HEAP_FLAGS flags{};Check(sources[n]->GetHeapProperties(&hp,&flags),"Shared buffer heap");
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||desc.Width!=layout.buffer_bytes||desc.Height!=1||desc.DepthOrArraySize!=1||desc.MipLevels!=1||desc.SampleDesc.Count!=1||desc.Layout!=D3D12_TEXTURE_LAYOUT_ROW_MAJOR||
               desc.Format!=DXGI_FORMAT_UNKNOWN||(desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||hp.Type!=D3D12_HEAP_TYPE_DEFAULT||!(flags&D3D12_HEAP_FLAG_SHARED))
                throw std::runtime_error("Named source is not the declared shared DEFAULT buffer");
        }
        Open(name+L"_ready",ready);Open(name+L"_released",released);
        releasedFloor=released->GetCompletedValue();
        if(ready->GetCompletedValue()==UINT64_MAX||releasedFloor==UINT64_MAX) throw std::runtime_error("Shared fence device removed");
        // After a prior consumer exits, already-released slots belong to the
        // producer again. Wait for a newer publication, never reread them.
        Check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&completion)),"Create completion fence");
        event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event.value) Check(HRESULT_FROM_WIN32(GetLastError()),"CreateEvent");
        Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Create allocator");
        Check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Create command list");Check(list->Close(),"Close initial list");
        D3D12_ROOT_PARAMETER params[6]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,sizeof(Params)/4};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[1].Descriptor.ShaderRegister=0;
        for(UINT n=2;n<6;++n) { params[n].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[n].Descriptor.ShaderRegister=n-2; }
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=6;rs.pParameters=params;
        ComPtr<ID3DBlob> serialized,errors;Check(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"Serialize root signature");
        Check(d->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)),"Create root signature");
        const char* entries[]={"Clear","DepthPass","IndexPass","Resolve"};
        for(UINT n=0;n<4;++n) { auto code=Compile(entries[n],calibration.eyeProjections.has_value());D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};Check(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&passes[n])),"Create compute pipeline"); }
        closest=Buffer(eyeBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
        winner=Buffer(eyeBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
        for(UINT eye=0;eye<2;++eye) {
            rgba[eye]=Buffer(eyeBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_SOURCE,true);
            bgra[eye]=Buffer(eyeBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_SOURCE,true);
        }
    }
};
LiveStereo::LiveStereo(ID3D12Device* d,ID3D12CommandQueue* q,const std::wstring& name,float baseline,UINT eyeWidth,UINT eyeHeight,reprojection::ProjectionCalibration calibration):impl_(std::make_unique<Impl>(d,q,name,baseline,eyeWidth,eyeHeight,calibration)) {}
LiveStereo::~LiveStereo()=default;
UINT64 LiveStereo::FrameId() const noexcept { return impl_->last; }
UINT64 LiveStereo::CaptureTick() const noexcept { return impl_->tick; }
UINT LiveStereo::Width() const noexcept { return impl_->layout.width; }
UINT LiveStereo::Height() const noexcept { return impl_->layout.height; }
UINT LiveStereo::EyeWidth() const noexcept { return impl_->eyeWidth; }
UINT LiveStereo::EyeHeight() const noexcept { return impl_->eyeHeight; }
const reprojection::ProjectionCalibration& LiveStereo::Calibration() const noexcept { return impl_->calibration; }
bool LiveStereo::Update() {
    auto& i=*impl_;i.Drain();
    if(!i.StreamValid()) return false;
    const int slot=live_pair::Newest(*i.view.value,i.last>i.releasedFloor?i.last:i.releasedFloor);if(slot<0) return false;
    const auto id=static_cast<UINT64>(live_pair::Read64(i.view.value->slots[slot].sequence));
    const auto tick=static_cast<UINT64>(live_pair::Read64(i.view.value->slots[slot].tick));
    if(id<=i.last||id<=i.releasedFloor||id>UINT64(LLONG_MAX)||id!=UINT64(live_pair::Read64(i.view.value->slots[slot].sequence))) return false;
    if(i.ready->GetCompletedValue()==UINT64_MAX||i.released->GetCompletedValue()==UINT64_MAX) i.InFlightFailure("shared fence device removed");
    if(!live_pair::Fresh(GetTickCount64(),tick)) {
        // Retire stale publications without binding or reading their source.
        // Otherwise a full aged ring can never publish a fresh frame again.
        // Waiting for ready still matters: released must not outrun GPU writes.
        i.Begin();i.Submit(id);i.releasedFloor=id;return false;
    }
    i.Begin();i.Barrier(i.sources[slot].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    i.list->SetComputeRootSignature(i.root.Get());i.list->SetComputeRootShaderResourceView(1,i.sources[slot]->GetGPUVirtualAddress());
    i.list->SetComputeRootUnorderedAccessView(2,i.closest->GetGPUVirtualAddress());i.list->SetComputeRootUnorderedAccessView(3,i.winner->GetGPUVirtualAddress());
    for(UINT eye=0;eye<2;++eye) {
        i.Barrier(i.rgba[eye].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        i.Barrier(i.bgra[eye].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto& model=i.calibration.model;
        const auto& map=i.imageTransforms[eye];
        Params p{i.layout.width,i.layout.height,UINT(i.layout.depth_offset),i.layout.depth_pitch,UINT(i.layout.color_offset),i.layout.color_pitch,i.layout.color_format,(eye?0.5f:-0.5f)*i.baseline,i.eyeWidth,i.eyeHeight,model.fx,model.a,model.b,model.wsign,map.scaleX,map.scaleY,map.offsetX,map.offsetY};
        i.list->SetComputeRoot32BitConstants(0,sizeof(Params)/4,&p,0);i.list->SetComputeRootUnorderedAccessView(4,i.rgba[eye]->GetGPUVirtualAddress());i.list->SetComputeRootUnorderedAccessView(5,i.bgra[eye]->GetGPUVirtualAddress());
        for(UINT pass=0;pass<4;++pass) {
            i.list->SetPipelineState(i.passes[pass].Get());const UINT count=pass==0||pass==3?i.eyePixels:p.sw*p.sh;i.list->Dispatch((count+255)/256,1,1);
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;i.list->ResourceBarrier(1,&barrier);
        }
        i.Barrier(i.rgba[eye].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        i.Barrier(i.bgra[eye].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    i.Barrier(i.sources[slot].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    i.Submit(id);i.last=id;i.tick=tick;return true;
}
void LiveStereo::Blit(ID3D12Resource* target,DXGI_FORMAT format,unsigned eye) {
    if(!TryBlit(target,format,eye)) throw std::runtime_error("Blit has no fresh live frame");
}
bool LiveStereo::TryBlit(ID3D12Resource* target,DXGI_FORMAT format,unsigned eye) {
    auto& i=*impl_;
    const bool bgra=format==DXGI_FORMAT_B8G8R8A8_UNORM||format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if(eye>1||(!bgra&&format!=DXGI_FORMAT_R8G8B8A8_UNORM&&format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)||!target) throw std::invalid_argument("Blit requires eye 0/1 and RGBA/BGRA UNORM/sRGB target");
    if(!i.last||!i.StreamValid()||!live_pair::Fresh(GetTickCount64(),i.tick)) return false;
    ComPtr<ID3D12Device> owner;Check(target->GetDevice(IID_PPV_ARGS(&owner)),"target GetDevice");auto d=target->GetDesc();
    const auto typeless=bgra?DXGI_FORMAT_B8G8R8A8_TYPELESS:DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if(owner.Get()!=i.device.Get()||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Width!=i.eyeWidth||d.Height!=i.eyeHeight||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)||(d.Format!=format&&d.Format!=typeless))
        throw std::invalid_argument("Blit needs same-device RT texture matching eye resolution, one mip/slice/sample, compatible format");
    ComPtr<ID3D12Resource> retainTarget=target;
    i.Begin();i.Barrier(target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=bgra?i.bgra[eye].Get():i.rgba[eye].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint={format,i.eyeWidth,i.eyeHeight,1,i.eyeWidth*4};
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=target;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    i.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);i.Barrier(target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);i.Submit();
    return true;
}
void LiveStereo::SavePreview(const std::wstring& prefix) {
    auto& i=*impl_;if(!i.last) throw std::logic_error("SavePreview before first frame");
    std::array<std::filesystem::path,2> paths{prefix+L".left.rgba.raw",prefix+L".right.rgba.raw"};
    for(const auto& path:paths) if(std::filesystem::exists(path)) throw std::runtime_error("Preview file exists; choose a new prefix");
    auto readback=i.Buffer(i.eyeBytes*2,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    i.Begin();for(UINT eye=0;eye<2;++eye) i.list->CopyBufferRegion(readback.Get(),eye*i.eyeBytes,i.rgba[eye].Get(),0,i.eyeBytes);i.Submit();
    void* mapped=nullptr;D3D12_RANGE range{0,SIZE_T(i.eyeBytes*2)};Check(readback->Map(0,&range,&mapped),"Map preview only");
    // GPU is drained; diagnostic write failures can safely unwind.
    try {
        for(UINT eye=0;eye<2;++eye) { std::ofstream out(paths[eye],std::ios::binary);out.write(static_cast<const char*>(mapped)+eye*i.eyeBytes,i.eyeBytes);if(!out) throw std::runtime_error("Write preview failed"); }
    } catch(...) { D3D12_RANGE empty{0,0};readback->Unmap(0,&empty);throw; }
    D3D12_RANGE empty{0,0};readback->Unmap(0,&empty);
}
