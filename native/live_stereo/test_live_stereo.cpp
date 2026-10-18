// Hardware-only, two-process fixture: no windows, game access, or OpenXR.
#include "live_stereo.hpp"
#include "../live_pair_stream/protocol.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
#include <cstring>
#include <limits>
#include <functional>
using Microsoft::WRL::ComPtr;
namespace {
// Test-only models, independent of the consumer. 1..4 exercise a different
// focal scale, forward/reverse depth and both view-Z conventions.
UINT fixtureProfile=0;
struct FixtureProjection { double fx,a,b,sign; };
FixtureProjection Model() {
    if(!fixtureProfile) return {0.572404087,-2.02655792e-6,1.00000203,1};
    const double sign=fixtureProfile>2?-1:1;
    const bool reverse=fixtureProfile%2==1;
    return {1,sign*(reverse?-1:2),reverse?2.0:-2.0,sign};
}
void Require(bool b,const char* why) { if(!b) throw std::runtime_error(why); }
void Check(HRESULT hr,const char* why) { Require(SUCCEEDED(hr),why); }
struct Handle {
    HANDLE h=nullptr;
    ~Handle() { if(h&&h!=INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle()=default; Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
};
struct View { live_pair::Header* p=nullptr; ~View() { if(p) UnmapViewOfFile(p); } };
void Reject(const std::function<void()>& f,const char* why) { bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } Require(rejected,why); }
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v{}; v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; return v;
}
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    Handle event; UINT64 timeline=0;
    Gpu() {
        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
        ComPtr<IDXGIAdapter1> adapter;
        for(UINT n=0;factory->EnumAdapters1(n,&adapter)!=DXGI_ERROR_NOT_FOUND;++n) {
            DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
            if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) break;
            adapter.Reset();
        }
        Require(bool(device),"Hardware D3D12 adapter required (no WARP fallback)");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
        Check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"allocator");
        Check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list"); Check(list->Close(),"close");
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence"); event.h=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,bool shared=false) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=heap;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=bytes; d.Height=1; d.DepthOrArraySize=1; d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r; Check(device->CreateCommittedResource(&hp,shared?D3D12_HEAP_FLAG_SHARED:D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"buffer"); return r;
    }
    void Begin() { Check(allocator->Reset(),"reset allocator"); Check(list->Reset(allocator.Get(),nullptr),"reset list"); }
    void End() {
        Check(list->Close(),"close commands"); ID3D12CommandList* lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
        if(FAILED(queue->Signal(fence.Get(),++timeline))||FAILED(fence->SetEventOnCompletion(timeline,event.h))||WaitForSingleObject(event.h,5000)!=WAIT_OBJECT_0||fence->GetCompletedValue()==UINT64_MAX) {
            std::cerr<<"Fixture GPU failure; retaining resources until process exit\n"; TerminateProcess(GetCurrentProcess(),4); ExitProcess(4);
        }
    }
    void NoErrors() {
        ComPtr<ID3D12InfoQueue> info; if(FAILED(device.As(&info))) return;
        for(UINT64 n=0;n<info->GetNumStoredMessages();++n) {
            SIZE_T size=0; info->GetMessage(n,nullptr,&size); std::vector<char> bytes(size); auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); info->GetMessage(n,m,&size);
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(m->pDescription);
        }
    }
};
// Nontrivial source pitches/offsets and half-size horizontal output create
// collisions. Alpha intentionally varies: consumer must always output 255.
float Depth(UINT x,UINT y,UINT frame) {
    if(fixtureProfile&&frame==4&&x<7) {
        const float edge[]={std::numeric_limits<float>::infinity(),-0.1f,1.1f,-std::numeric_limits<float>::infinity(),-0.f,0.f,1.f};
        return edge[x];
    }
    if(frame==6) return x%2?0.0067077986896038055f:0.006707798223942518f;
    if(frame==4&&((x/37+y/19)%5==0)) return std::numeric_limits<float>::quiet_NaN();
    if(frame==4) return x%2?0.75001f:0.25f;
    return frame==5?0.4f:0.3f;
}
UINT Packed(UINT x,UINT y,UINT frame,UINT fmt) {
    UINT r=(x*13+frame*47)%256,g=(y*7+frame*23)%256,b=(x+y*3+frame*11)%256;
    if(fmt==24) return ((r*4+1)&1023)|(((g*4+2)&1023)<<10)|(((b*4+3)&1023)<<20);
    return fmt==87?b|(g<<8)|(r<<16):r|(g<<8)|(b<<16);
}
UINT Rgba(UINT x,UINT y,UINT frame,UINT fmt) {
    UINT c=Packed(x,y,frame,fmt);
    if(fmt==24) { UINT r=((c&1023)*255+511)/1023,g=(((c>>10)&1023)*255+511)/1023,b=(((c>>20)&1023)*255+511)/1023; return r|(g<<8)|(b<<16)|0xff000000; }
    if(fmt==87) c=(c&0xff00)|((c&255)<<16)|((c>>16)&255);
    return c|0xff000000;
}
std::vector<UINT> Oracle(const live_pair::Header& h,UINT frame,UINT eye,UINT eyeWidth=1280,UINT eyeHeight=720) {
    std::vector<UINT> out(size_t(eyeWidth)*eyeHeight,0xff000000),winner(out.size(),UINT_MAX);
    std::vector<double> nearest(out.size(),std::numeric_limits<double>::infinity());
    const auto p=Model();
    for(UINT y=0;y<h.height;++y) for(UINT x=0;x<h.width;++x) {
        float d=Depth(x,y,frame); if(!std::isfinite(d)||d<0||d>1) continue;
        double z=p.sign*p.b/(p.sign*double(d)-p.a);
        if(!std::isfinite(z)||z<=0) continue;
        double px=(x+0.5)*eyeWidth/h.width-p.fx*(eye?0.25:-0.25)*eyeWidth/(2*z),py=(y+0.5)*eyeHeight/h.height;
        if(px<0||px>=eyeWidth) continue;
        UINT t=UINT(std::floor(py))*eyeWidth+UINT(std::floor(px)),id=y*h.width+x;
        if(z<nearest[t]||(z==nearest[t]&&id<winner[t])) { nearest[t]=z;winner[t]=id;out[t]=Rgba(x,y,frame,h.color_format); }
    }
    return out;
}
void VerifyPreview(LiveStereo& stereo,const live_pair::Header& h,UINT frame,const std::wstring& prefix,UINT eyeWidth=1280,UINT eyeHeight=720) {
    stereo.SavePreview(prefix);
    std::array<std::vector<UINT>,2> eyes;
    for(UINT eye=0;eye<2;++eye) {
        const auto path=std::filesystem::path(prefix+(eye?L".right.rgba.raw":L".left.rgba.raw"));
        std::ifstream file(path,std::ios::binary); Require(bool(file),"SavePreview file missing");
        eyes[eye].resize(size_t(eyeWidth)*eyeHeight); file.read(reinterpret_cast<char*>(eyes[eye].data()),eyes[eye].size()*4);
        Require(bool(file)&&file.peek()==EOF,"SavePreview size incorrect");
        auto want=Oracle(h,frame,eye,eyeWidth,eyeHeight);
        if(eyes[eye]!=want) {
            size_t bad=0; while(bad<want.size()&&want[bad]==eyes[eye][bad]) ++bad;
            std::cerr<<"frame="<<frame<<" eye="<<eye<<" pixel="<<bad<<" got="<<std::hex<<eyes[eye][bad]<<" want="<<want[bad]<<std::dec<<'\n';
            throw std::runtime_error("GPU preview differs from independent numeric depth/color oracle");
        }
        Require(std::count(want.begin(),want.end(),0xff000000)>0,"Hole mask missing"); file.close();std::filesystem::remove(path);
    }
    Require(eyes[0]!=eyes[1],"Eye parallax missing");
}
void VerifyBlits(Gpu& gpu,LiveStereo& stereo,const live_pair::Header& h,UINT frame,UINT eyeWidth=1280,UINT eyeHeight=720) {
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) for(UINT eye=0;eye<2;++eye) {
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=eyeWidth;d.Height=eyeHeight;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> target; Check(gpu.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)),"target");
        stereo.Blit(target.Get(),format,eye);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes=0; gpu.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
        auto readback=gpu.Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.Begin(); auto barrier=Transition(target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE); gpu.list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=target.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        gpu.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr); barrier=Transition(target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);gpu.list->ResourceBarrier(1,&barrier);gpu.End();
        void* mapped=nullptr; D3D12_RANGE range{0,SIZE_T(bytes)};Check(readback->Map(0,&range,&mapped),"readback");auto want=Oracle(h,frame,eye,eyeWidth,eyeHeight);
        for(UINT y=0;y<eyeHeight;++y) for(UINT x=0;x<eyeWidth;++x) {
            UINT expected=want[y*eyeWidth+x];if(format==87||format==91) expected=(expected&0xff00ff00)|((expected&255)<<16)|((expected>>16)&255);
            UINT actual;std::memcpy(&actual,static_cast<char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch+x*4,4);Require(actual==expected,"Blit target pixels differ from RGBA/BGRA oracle");
        }
        D3D12_RANGE empty{0,0};readback->Unmap(0,&empty);
        Reject([&]{stereo.Blit(target.Get(),format,2);},"Invalid eye accepted");
    }
    // Reject each axis independently, and a complete different supported profile.
    for(auto size: {std::array<UINT,2>{eyeWidth==1280?1920u:1280u,eyeHeight},std::array<UINT,2>{eyeWidth,eyeHeight==720?1080u:720u},std::array<UINT,2>{eyeWidth==1280?1920u:1280u,eyeHeight==720?1080u:720u}}) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=size[0];d.Height=size[1];d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> target;
        Check(gpu.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)),"mismatched target");
        bool rejected=false;
        try { stereo.TryBlit(target.Get(),d.Format,0); } catch(const std::invalid_argument&) { rejected=true; }
        Require(rejected,"Mismatched Blit width/height accepted (or source expired before validation)");
    }
}
std::unique_ptr<LiveStereo> ConfiguredStereo(Gpu& gpu,const std::wstring& name,UINT width,UINT height) {
    if(fixtureProfile) {
        const auto p=Model();
        // Nonzero principal point offsets are intentionally omitted from the
        // reduced contract: they cancel for identical parallel projections.
        std::array<float,16> m{float(p.fx),0,0,0,0,1,0,0,0.125f,-0.25f,float(p.a),float(p.sign),0,0,float(p.b),0};
        reprojection::ProjectionCalibration calibration{reprojection::Perspective::FromRowMatrix(m),reprojection::CalibrationProvenance::ExplicitUnassociated};
        return std::make_unique<LiveStereo>(gpu.device.Get(),gpu.queue.Get(),name,0.5f,width,height,calibration);
    }
    // Keep exercising the original three-argument call for the default fixture.
    if(width==1280&&height==720) return std::make_unique<LiveStereo>(gpu.device.Get(),gpu.queue.Get(),name);
    return std::make_unique<LiveStereo>(gpu.device.Get(),gpu.queue.Get(),name,0.5f,width,height);
}
void VerifyResolutionValidation() {
    for(auto size:{std::array<UINT,2>{1280,1080},{1920,720},{2560,1080},{640,360},{0,720},{1280,0},{UINT_MAX,UINT_MAX}}) {
        bool rejected=false;
        try {
            LiveStereo invalid(nullptr,nullptr,L"",0.5f,size[0],size[1]);
        } catch(const std::invalid_argument& e) { rejected=std::string(e.what()).find("Unsupported eye resolution")!=std::string::npos; }
        Require(rejected,"Unsupported eye resolution must be rejected before device access/GPU allocation");
    }
}
void WaitEvent(HANDLE h) { Require(WaitForSingleObject(h,15000)==WAIT_OBJECT_0,"fixture event timeout"); }
void Worker(const std::wstring& name,const std::wstring& prefix,bool timeout,UINT eyeWidth=1280,UINT eyeHeight=720) {
    Gpu gpu; Handle map;map.h=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,name.c_str());Require(bool(map.h),"worker map");
    View view;view.p=static_cast<live_pair::Header*>(MapViewOfFile(map.h,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(live_pair::Header)));Require(bool(view.p),"worker view");auto& h=*view.p;
    Handle go,done;go.h=OpenEventW(SYNCHRONIZE,FALSE,(name+L"_go").c_str());done.h=OpenEventW(EVENT_MODIFY_STATE,FALSE,(name+L"_done").c_str());
    std::cout<<"consumer PID="<<GetCurrentProcessId()<<" producer PID="<<h.producer_pid<<" (separate D3D12 devices)\n";
    for(auto bad: {reprojection::ProjectionCalibration{{1,1,.5f,1,1},reprojection::CalibrationProvenance::ExplicitUnassociated},
                   reprojection::ProjectionCalibration{{1,1,-1,2,1},reprojection::CalibrationProvenance::LegacyAc6Observation}}) {
        Reject([&]{LiveStereo invalid(gpu.device.Get(),gpu.queue.Get(),name,0.5f,eyeWidth,eyeHeight,bad);},"Invalid projection reached stream consumer");
        Require(live_pair::Read32(h.consumer_pid)==0,"Invalid calibration claimed stream");
    }
    {
        auto ownedStereo=ConfiguredStereo(gpu,name,eyeWidth,eyeHeight);auto& stereo=*ownedStereo;
        Require(stereo.Width()==h.width&&stereo.Height()==h.height,"source dimensions");
        Require(stereo.EyeWidth()==eyeWidth&&stereo.EyeHeight()==eyeHeight,"configured eye dimensions");
        const auto& calibration=stereo.Calibration();calibration.Validate();
        std::cout<<"projection="<<calibration.Json()<<'\n';
        Require(calibration.provenance==(fixtureProfile?reprojection::CalibrationProvenance::ExplicitUnassociated:reprojection::CalibrationProvenance::LegacyAc6Observation),"Projection provenance lost");
        Require(live_pair::Read32(h.consumer_pid)==LONG(GetCurrentProcessId()),"consumer PID not claimed");
        Reject([&]{LiveStereo duplicate(gpu.device.Get(),gpu.queue.Get(),name);},"Second consumer accepted");
        Require(live_pair::Read32(h.consumer_pid)==LONG(GetCurrentProcessId()),"Failed duplicate cleared live claim");
        SetEvent(done.h);WaitEvent(go.h); // publish only after device/shader setup
        Require(stereo.Update()&&stereo.FrameId()==3,"Newest ready frame 3 not consumed");
        Require(stereo.CaptureTick()==UINT64(live_pair::Read64(h.slots[2].tick)),"capture tick mismatch");
        Require(!stereo.Update(),"Repeated frame consumed");
        VerifyPreview(stereo,h,3,prefix,eyeWidth,eyeHeight);VerifyBlits(gpu,stereo,h,3,eyeWidth,eyeHeight);SetEvent(done.h);
        WaitEvent(go.h);Require(stereo.Update()&&stereo.FrameId()==4,"Changing frame 4 not consumed");VerifyPreview(stereo,h,4,prefix,eyeWidth,eyeHeight);SetEvent(done.h);
        WaitEvent(go.h);Require(stereo.Update()&&stereo.FrameId()==5,"Stopped final frame not consumed");VerifyPreview(stereo,h,5,prefix,eyeWidth,eyeHeight);SetEvent(done.h);
        WaitEvent(go.h);SetEvent(done.h); // parent deliberately withholds ready=6
        Require(stereo.Update()&&stereo.FrameId()==6,"Pending-ready frame 6 not consumed");
        Require(!timeout,"Unsignaled ready fence was ignored");VerifyPreview(stereo,h,6,prefix,eyeWidth,eyeHeight);SetEvent(done.h);
        WaitEvent(go.h);Require(!stereo.Update()&&stereo.FrameId()==6,"Stale frame consumed");
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=eyeWidth;td.Height=eyeHeight;td.DepthOrArraySize=1;td.MipLevels=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> validTarget;
        Check(gpu.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&validTarget)),"valid stale target");
        Reject([&]{stereo.Blit(validTarget.Get(),td.Format,0);},"Stale blit accepted");
        Require(!stereo.TryBlit(validTarget.Get(),td.Format,0),"Expired source must return false without aborting the XR frame");
        const auto width=h.width;h.width=128;Require(!stereo.Update(),"Changed immutable header accepted");h.width=width;
        Require(live_pair::Read32(h.stop_requested)==0,"Consumer requested producer stop");SetEvent(done.h);
        WaitEvent(go.h);Require(stereo.Update()&&stereo.FrameId()==10,"Fresh publication did not resume after aged full ring");VerifyPreview(stereo,h,10,prefix,eyeWidth,eyeHeight);SetEvent(done.h);
    }
    Require(live_pair::Read32(h.consumer_pid)==0,"Consumer PID not released after drain");
    // A later consumer must not read a slot already released by its predecessor:
    // the producer may concurrently overwrite any such slot.
    const auto savedHeader=h;
    for(auto& slot:h.slots) { InterlockedExchange64(&slot.sequence,10);InterlockedExchange64(&slot.tick,LONG64(GetTickCount64())); }
    { LiveStereo reopened(gpu.device.Get(),gpu.queue.Get(),name);Require(!reopened.Update()&&reopened.FrameId()==0,"Reopened consumer reread released source"); }
    h=savedHeader;
    auto invalid=[&](const std::function<void()>& change,const char* reason) {
        live_pair::Header saved=h;change();Reject([&]{LiveStereo s(gpu.device.Get(),gpu.queue.Get(),name);},reason);h=saved;
        Require(live_pair::Read32(h.consumer_pid)==0,"Rejected constructor leaked claim");
    };
    invalid([&]{h.adapter.LowPart^=1;},"Adapter mismatch accepted");
    invalid([&]{h.version++;},"Version mismatch accepted");
    invalid([&]{h.status=live_pair::Preparing;},"Preparing accepted");
    invalid([&]{h.status=live_pair::Failed;},"Failed accepted");
    invalid([&]{h.color_offset=h.depth_offset;},"Overlapping planes accepted");
    invalid([&]{h.buffer_bytes=UINT64_MAX;},"Resource size mismatch accepted");
    invalid([&]{h.depth_pitch=1;},"Invalid pitch accepted");
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;ComPtr<ID3D12CommandQueue> compute;
    Check(gpu.device->CreateCommandQueue(&q,IID_PPV_ARGS(&compute)),"compute queue");Reject([&]{LiveStereo s(gpu.device.Get(),compute.Get(),name);},"Compute queue accepted");
    gpu.NoErrors();
}
void Producer(UINT fmt,bool timeout=false,UINT eyeWidth=1280,UINT eyeHeight=720) {
    Gpu gpu;
    const auto name=L"Local\\ac6_live_stereo_fixture_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(fmt);
    const auto prefix=(std::filesystem::temp_directory_path()/(L"ac6_live_stereo_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(fmt))).wstring();
    Handle map;map.h=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(live_pair::Header),name.c_str());Require(bool(map.h),"create mapping");
    View view;view.p=static_cast<live_pair::Header*>(MapViewOfFile(map.h,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(live_pair::Header)));Require(bool(view.p),"map view");
    new(view.p) live_pair::Header();auto& h=*view.p;h.producer_pid=GetCurrentProcessId();h.adapter=gpu.device->GetAdapterLuid();h.width=2560;h.height=128;h.color_format=fmt;
    h.depth_offset=512;h.depth_pitch=h.width*4+256;h.color_offset=(h.depth_offset+UINT64(h.depth_pitch)*h.height+511)&~511ull;h.color_pitch=h.width*4+512;h.buffer_bytes=h.color_offset+UINT64(h.color_pitch)*h.height;
    std::array<ComPtr<ID3D12Resource>,3> buffers;std::array<Handle,3> handles;
    for(UINT n=0;n<3;++n) { buffers[n]=gpu.Buffer(h.buffer_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,true);Check(gpu.device->CreateSharedHandle(buffers[n].Get(),nullptr,GENERIC_ALL,(name+L"_buffer"+std::to_wstring(n)).c_str(),&handles[n].h),"share buffer"); }
    ComPtr<ID3D12Fence> ready,released;Handle rh,lh;
    Check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&ready)),"ready");Check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&released)),"released");
    Check(gpu.device->CreateSharedHandle(ready.Get(),nullptr,GENERIC_ALL,(name+L"_ready").c_str(),&rh.h),"share ready");Check(gpu.device->CreateSharedHandle(released.Get(),nullptr,GENERIC_ALL,(name+L"_released").c_str(),&lh.h),"share released");
    auto upload=gpu.Buffer(h.buffer_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto publish=[&](UINT slot,UINT frame,bool signalReady=true) {
        Require(live_pair::Reusable(UINT64(live_pair::Read64(h.slots[slot].sequence)),ready->GetCompletedValue(),released->GetCompletedValue()),"Producer slot not released");
        void* mapped=nullptr;D3D12_RANGE empty{0,0};Check(upload->Map(0,&empty,&mapped),"upload map");std::memset(mapped,0xa5,SIZE_T(h.buffer_bytes));
        for(UINT y=0;y<h.height;++y) for(UINT x=0;x<h.width;++x) { float d=Depth(x,y,frame);UINT c=Packed(x,y,frame,fmt);std::memcpy(static_cast<char*>(mapped)+h.depth_offset+y*h.depth_pitch+x*4,&d,4);std::memcpy(static_cast<char*>(mapped)+h.color_offset+y*h.color_pitch+x*4,&c,4); }
        upload->Unmap(0,nullptr);gpu.Begin();auto barrier=Transition(buffers[slot].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);gpu.list->ResourceBarrier(1,&barrier);gpu.list->CopyBufferRegion(buffers[slot].Get(),0,upload.Get(),0,h.buffer_bytes);barrier=Transition(buffers[slot].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);gpu.list->ResourceBarrier(1,&barrier);gpu.End();
        if(signalReady) Check(gpu.queue->Signal(ready.Get(),frame),"publish ready");InterlockedExchange64(&h.slots[slot].tick,LONG64(GetTickCount64()));InterlockedExchange64(&h.slots[slot].sequence,frame);
    };
    Handle go,done;go.h=CreateEventW(nullptr,FALSE,FALSE,(name+L"_go").c_str());done.h=CreateEventW(nullptr,FALSE,FALSE,(name+L"_done").c_str());
    wchar_t exe[32768];Require(GetModuleFileNameW(nullptr,exe,32768)>0,"exe path");std::wstring cmd=L"\""+std::wstring(exe)+(timeout?L"\" --worker-timeout \"":L"\" --worker \"")+name+L"\" \""+prefix+L"\" "+std::to_wstring(eyeWidth)+L" "+std::to_wstring(eyeHeight);
    cmd+=L" "+std::to_wstring(fixtureProfile);
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle log;const auto logPath=prefix+L".worker.log";
    log.h=CreateFileW(logPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);Require(log.h!=INVALID_HANDLE_VALUE,"worker log");
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;si.wShowWindow=SW_HIDE;si.hStdOutput=si.hStdError=log.h;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);PROCESS_INFORMATION pi{};
    InterlockedExchange(&h.status,live_pair::Ready);
    Require(CreateProcessW(exe,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi),"launch consumer");Handle process,thread;process.h=pi.hProcess;thread.h=pi.hThread;
    auto printLog=[&] { std::ifstream input(logPath);std::cerr<<input.rdbuf(); };
    auto step=[&] { SetEvent(go.h);HANDLE waits[]={done.h,process.h};if(WaitForMultipleObjects(2,waits,FALSE,15000)!=WAIT_OBJECT_0) { printLog();throw std::runtime_error("Consumer failed or timed out"); } };
    { HANDLE waits[]={done.h,process.h};if(WaitForMultipleObjects(2,waits,FALSE,15000)!=WAIT_OBJECT_0) { printLog();throw std::runtime_error("Consumer initialization failed"); } }
    publish(0,1);publish(1,2);publish(2,3);InterlockedExchange(&h.status,live_pair::Ready);step();
    Require(released->GetCompletedValue()==3,"Newest frame release missing / skipped frames not released");publish(0,4);step();Require(released->GetCompletedValue()==4,"Frame 4 release missing");
    publish(1,5);InterlockedExchange(&h.status,live_pair::Stopped);step();Require(released->GetCompletedValue()==5,"Stopped frame release missing");
    publish(0,6,false);SetEvent(go.h);WaitEvent(done.h);Sleep(100);
    Require(released->GetCompletedValue()==5,"Released frame before shared ready fence completed");
    if(timeout) {
        Require(WaitForSingleObject(process.h,7000)==WAIT_OBJECT_0,"In-flight failure did not terminate within bound");DWORD exit=0;GetExitCodeProcess(process.h,&exit);printLog();
        Require(exit==3,"In-flight timeout did not terminate standalone process with expected code");
        Require(live_pair::Read32(h.consumer_pid)==LONG(pi.dwProcessId),"In-flight timeout unwound consumer claim");
        std::cout<<"post-termination ready="<<ready->GetCompletedValue()<<" released="<<released->GetCompletedValue()<<" producer-removal="<<gpu.device->GetDeviceRemovedReason()<<'\n';
        Require(!live_pair::Reusable(6,ready->GetCompletedValue(),released->GetCompletedValue()),"In-flight failure made pending source reusable");
        std::cout<<"PASS in-flight timeout: standalone process exits3, PID claim retained, source unreleased\n";CloseHandle(log.h);log.h=nullptr;std::filesystem::remove(logPath);return;
    }
    Check(gpu.queue->Signal(ready.Get(),6),"complete delayed ready");WaitEvent(done.h);Require(released->GetCompletedValue()==6,"Pending-ready frame release missing");
    Sleep(550);publish(0,7);publish(1,8);publish(2,9);
    for(auto& slot:h.slots) InterlockedExchange64(&slot.tick,LONG64(GetTickCount64()-600));
    step();Require(released->GetCompletedValue()==9,"Aged full ring was not retired without source reads");
    InterlockedExchange(&h.status,live_pair::Ready);publish(0,10);step();Require(released->GetCompletedValue()==10,"Resumed frame release missing");
    Require(WaitForSingleObject(process.h,15000)==WAIT_OBJECT_0,"Worker exit timeout");DWORD code=1;GetExitCodeProcess(process.h,&code);printLog();Require(code==0,"Consumer fixture failed");gpu.NoErrors();CloseHandle(log.h);log.h=nullptr;std::filesystem::remove(logPath);
    std::cout<<"PASS format="<<fmt<<" eye="<<eyeWidth<<'x'<<eyeHeight<<" profile="<<fixtureProfile<<" changing frames, masks, parallax, byte-exact previews and 8 target blits, skip/repeat/release/stale/Stopped/validation\n";
}
}
int wmain(int argc,wchar_t** argv) try {
    if(argc==2&&std::wstring(argv[1])==L"--invalid-resolution") VerifyResolutionValidation();
    else if(argc==7&&(std::wstring(argv[1])==L"--worker"||std::wstring(argv[1])==L"--worker-timeout")) { fixtureProfile=UINT(std::stoul(argv[6]));Worker(argv[2],argv[3],std::wstring(argv[1])==L"--worker-timeout",UINT(std::stoul(argv[4])),UINT(std::stoul(argv[5]))); }
    else if(argc==2&&std::wstring(argv[1])==L"--projection-contract") { for(fixtureProfile=1;fixtureProfile<=4;++fixtureProfile) Producer(28); }
    else if(argc==2&&std::wstring(argv[1])==L"--timeout") Producer(28,true);
    else if(argc==2&&std::wstring(argv[1])==L"--1080p") { Producer(24,false,1920,1080);Producer(28,false,1920,1080);Producer(87,false,1920,1080); }
    else if(argc==2&&std::wstring(argv[1])==L"--1440p") Producer(28,false,2560,1440);
    else { Producer(24);Producer(28);Producer(87); }
    return 0;
} catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
