// Standalone offline GPU diagnostic. No game handles, hooks or OpenXR session.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "shader.hpp"
#include "../reprojection/projection.hpp"
using Microsoft::WRL::ComPtr;
struct Params {
    UINT sw,sh,tw,th;
    float fx,fy,a,b,wsign,eyeX;
    UINT padding0=0,padding1=0;
};
static_assert(sizeof(Params)==48);
struct Result { std::vector<UINT> indices,color; double gpu_ms=0; };
static void Require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
static void Check(HRESULT hr,const char* why) { if(FAILED(hr)) throw std::runtime_error(std::string(why)+" HRESULT "+std::to_string(static_cast<unsigned long>(hr))); }
static UINT Count(UINT w,UINT h) { Require(w&&h&&w<=16384&&h<=16384&&static_cast<UINT64>(w)*h<=65535ull*256,"Invalid dimensions (limit 65535 groups of 256 pixels)"); return w*h; }
static void Validate(const Params& p) {
    Count(p.sw,p.sh); Count(p.tw,p.th);
    reprojection::Perspective{p.fx,p.fy,p.a,p.b,p.wsign}.Validate();
    Require(std::isfinite(p.eyeX),"Eye offset must be finite");
}
static std::vector<UINT> Oracle(const Params& p,const std::vector<float>& depth) {
    std::vector<UINT> ids(Count(p.tw,p.th),UINT_MAX);
    std::vector<double> closest(ids.size(),std::numeric_limits<double>::infinity());
    for(UINT i=0;i<depth.size();++i) {
        double d=depth[i]; if(!std::isfinite(d)||d<0||d>1) continue;
        double z=static_cast<double>(p.wsign)*p.b/(p.wsign*d-p.a);
        if(!std::isfinite(z)||z<=0) continue;
        double x=(i%p.sw+0.5)*p.tw/p.sw-static_cast<double>(p.fx)*p.eyeX*p.tw/(2*z);
        double y=(i/p.sw+0.5)*p.th/p.sh;
        if(!std::isfinite(x)||x<0||x>=p.tw||y<0||y>=p.th) continue;
        UINT target=static_cast<UINT>(std::floor(y))*p.tw+static_cast<UINT>(std::floor(x));
        if(z<closest[target]) { closest[target]=z; ids[target]=i; }
    }
    return ids;
}
class Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,4> pipelines;
    UINT64 timeline=0;
    HANDLE event=nullptr;
    [[noreturn]] void InFlightFailure() {
        std::cerr<<"GPU wait/signal failed; terminating standalone process with resources retained\n";
        TerminateProcess(GetCurrentProcess(),4); ExitProcess(4);
    }
    void Wait() {
        if(FAILED(queue->Signal(fence.Get(),++timeline)) || FAILED(fence->SetEventOnCompletion(timeline,event))) InFlightFailure();
        if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0 || fence->GetCompletedValue()==UINT64_MAX || fence->GetCompletedValue()<timeline) InFlightFailure();
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,bool uav=false) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=heap;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=bytes; d.Height=1;
        d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(uav) d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> resource;
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&resource)),"Create buffer"); return resource;
    }
    void Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
    }
    void UavBarrier() { D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; list->ResourceBarrier(1,&b); }
public:
    bool debug_layer=false;
    Gpu() {
        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); debug_layer=true; }
        ComPtr<IDXGIFactory6> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
        for(UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> adapter;
            const auto hr=factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));
            if(hr==DXGI_ERROR_NOT_FOUND) break; Check(hr,"Enumerate hardware adapter");
            DXGI_ADAPTER_DESC1 d{}; Check(adapter->GetDesc1(&d),"Adapter description");
            if(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) {
                std::wcerr<<L"Hardware adapter: "<<d.Description<<L"\n"; break;
            }
        }
        Require(device.Get()!=nullptr,"Hardware D3D12 adapter required");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");
        Check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Allocator");
        Check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"List"); Check(list->Close(),"Close initial list");
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(event!=nullptr,"Fence event");
        D3D12_ROOT_PARAMETER params[6]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants={0,0,12};
        for(UINT i=1;i<6;++i) { params[i].ParameterType=i<3?D3D12_ROOT_PARAMETER_TYPE_SRV:D3D12_ROOT_PARAMETER_TYPE_UAV; params[i].Descriptor.ShaderRegister=i<3?i-1:i-3; }
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=6; desc.pParameters=params;
        ComPtr<ID3DBlob> bytes,errors;
        Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&errors),"Serialize root signature");
        Check(device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&root)),"Root signature");
        const char* names[]={"Clear","DepthPass","IndexPass","Resolve"};
        for(UINT i=0;i<4;++i) {
            bytes.Reset(); errors.Reset();
            HRESULT hr=D3DCompile(shader_source,sizeof(shader_source)-1,"reprojection.hlsl",nullptr,nullptr,names[i],"cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&bytes,&errors);
            if(FAILED(hr)&&errors) std::cerr<<static_cast<const char*>(errors->GetBufferPointer());
            Check(hr,"Compile reprojection shader");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get(); pd.CS={bytes->GetBufferPointer(),bytes->GetBufferSize()};
            Check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipelines[i])),"Compute pipeline");
        }
    }
    ~Gpu() { if(event) CloseHandle(event); }
    Result Run(const Params& p,const std::vector<float>& depth,const std::vector<UINT>& color) {
        Validate(p); const UINT input=Count(p.sw,p.sh),output=Count(p.tw,p.th);
        Require(depth.size()==input&&color.size()==input,"Input buffer length mismatch");
        auto upload=Buffer(8ull*input,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        auto source=Buffer(8ull*input,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        auto closest=Buffer(4ull*output,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
        auto winner=Buffer(4ull*output,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
        auto image=Buffer(4ull*output,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
        auto readback=Buffer(8ull*output,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto times=Buffer(16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_QUERY_HEAP_DESC qd{}; qd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count=2;
        ComPtr<ID3D12QueryHeap> queries; Check(device->CreateQueryHeap(&qd,IID_PPV_ARGS(&queries)),"Timestamp heap");
        void* mapped=nullptr; D3D12_RANGE noRead{0,0}; Check(upload->Map(0,&noRead,&mapped),"Map upload");
        std::memcpy(mapped,depth.data(),4ull*input); std::memcpy(static_cast<char*>(mapped)+4ull*input,color.data(),4ull*input);
        upload->Unmap(0,nullptr);
        Check(allocator->Reset(),"Reset allocator"); Check(list->Reset(allocator.Get(),nullptr),"Reset list");
        list->CopyBufferRegion(source.Get(),0,upload.Get(),0,8ull*input);
        Transition(source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->SetComputeRootSignature(root.Get()); list->SetComputeRoot32BitConstants(0,12,&p,0);
        list->SetComputeRootShaderResourceView(1,source->GetGPUVirtualAddress()); list->SetComputeRootShaderResourceView(2,source->GetGPUVirtualAddress()+4ull*input);
        list->SetComputeRootUnorderedAccessView(3,closest->GetGPUVirtualAddress()); list->SetComputeRootUnorderedAccessView(4,winner->GetGPUVirtualAddress()); list->SetComputeRootUnorderedAccessView(5,image->GetGPUVirtualAddress());
        list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        for(UINT pass=0;pass<4;++pass) {
            list->SetPipelineState(pipelines[pass].Get()); const auto count=pass==0||pass==3?output:input;
            list->Dispatch((count+255)/256,1,1); UavBarrier();
        }
        list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1); list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,times.Get(),0);
        Transition(winner.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(image.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(),0,winner.Get(),0,4ull*output); list->CopyBufferRegion(readback.Get(),4ull*output,image.Get(),0,4ull*output);
        Check(list->Close(),"Close submission"); ID3D12CommandList* lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists); Wait();
        Result result; result.indices.resize(output); result.color.resize(output);
        D3D12_RANGE readRange{0,static_cast<SIZE_T>(8ull*output)}; Check(readback->Map(0,&readRange,&mapped),"Map result");
        std::memcpy(result.indices.data(),mapped,4ull*output); std::memcpy(result.color.data(),static_cast<char*>(mapped)+4ull*output,4ull*output); readback->Unmap(0,&noRead);
        D3D12_RANGE timeRange{0,16}; Check(times->Map(0,&timeRange,&mapped),"Map timestamps");
        UINT64 ticks[2]; std::memcpy(ticks,mapped,16); times->Unmap(0,&noRead);
        UINT64 frequency=0; Check(queue->GetTimestampFrequency(&frequency),"Timestamp frequency"); Require(frequency&&ticks[1]>=ticks[0],"Invalid GPU timestamps");
        result.gpu_ms=1000.0*(ticks[1]-ticks[0])/frequency;
        NoErrors(); return result;
    }
    void NoErrors() {
        ComPtr<ID3D12InfoQueue> info; if(FAILED(device.As(&info))) return;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
            SIZE_T bytes=0; Check(info->GetMessage(i,nullptr,&bytes),"Debug message size"); std::vector<char> storage(bytes);
            auto* msg=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); Check(info->GetMessage(i,msg,&bytes),"Debug message");
            if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(msg->pDescription);
        }
    }
};
static void SelfTest(Gpu& gpu) {
    for(auto coefficients:{std::array<float,2>{0.5f,1},std::array<float,2>{2,1},std::array<float,2>{-1,-1}}) {
        Params invalid{8,6,8,6,1,1,coefficients[0],coefficients[1],1,0};
        bool rejected=false;try { Validate(invalid); } catch(const std::exception&) { rejected=true; }
        Require(rejected,"Projection with interior pole or behind-camera depth range accepted");
    }
    unsigned cases=0;
    auto test=[&](Params p,std::vector<float> depth) {
        std::vector<UINT> color(depth.size()); for(UINT i=0;i<color.size();++i) color[i]=0xff000000|i*12345;
        auto expected=Oracle(p,depth); auto actual=gpu.Run(p,depth,color);
        Require(actual.indices==expected,"GPU winner differs from double-precision fixture oracle");
        for(UINT i=0;i<expected.size();++i) Require(actual.color[i]==(expected[i]==UINT_MAX?0:color[expected[i]]),"GPU color/coverage mismatch"); ++cases;
    };
    const float nan=std::numeric_limits<float>::quiet_NaN();
    for(float sign:{1.f,-1.f}) {
        Params p{8,6,8,6,1,1,-sign/9,10.f/9,sign,0};
        std::vector<float> depth(48); for(UINT i=0;i<48;++i) depth[i]=0.1f+0.8f*i/47;
        test(p,depth); p.eyeX=-0.5f; test(p,std::vector<float>(48,0.25f));
        p.eyeX=0.5f; test(p,std::vector<float>(48,0.25f));
        p.tw=4; p.th=3; test(p,depth);
        p.eyeX=0; p.tw=16; p.th=12; test(p,std::vector<float>(48,0.3f));
        p.tw=8; p.th=6; p.a=sign*10.f/9; p.b=-10.f/9; p.eyeX=0.2f; test(p,depth);
    }
    Params p{5,1,5,1,1,1,-1.f/9,10.f/9,1,0.4f};
    test(p,{nan,1.f/6,1,nan,nan}); // Background and foreground collide; foreground wins.
    p.eyeX=-10; test(p,{nan,-0.1f,1.1f,std::numeric_limits<float>::infinity(),1});
    // Adjacent device depths can collapse after float32 view-Z conversion.
    Params precision{2,1,1,1,0.572404087f,1.01760733f,-2.02655792e-6f,1.00000203f,1,0};
    test(precision,{0.006707798223942518f,0.0067077986896038055f});
    precision.a=0; precision.b=1; test(precision,{0,0.25f}); // Infinite far pole rejected.
    precision.a=-1.f/9; precision.b=10.f/9; test(precision,{-0.f,0.f});
    for(float sign:{1.f,-1.f}) for(bool reverse:{false,true}) {
        Params edge{8,1,4,1,1,1,sign*(reverse?0.f:1.f),reverse?1.f:-1.f,sign,0};
        test(edge,{nan,std::numeric_limits<float>::infinity(),-0.1f,1.1f,-0.f,0.f,1.f,0.5f});
        // At zero eye offset both samples collide. Original depth must choose
        // the nearer, including forward-Z where LOWER device depth wins.
        edge.a=sign*(reverse?-1.f:2.f);edge.b=reverse?2.f:-2.f;
        test(edge,{0.25f,std::nextafter(0.25f,1.f),0.5f,0.5f,1,0,0.75f,0.875f});
    }
    std::cout<<"{\"fixture_cases\":"<<cases<<",\"exact_indices_and_color\":true,\"debug_layer\":"<<(gpu.debug_layer?"true":"false")<<"}\n";
}
static void Write(const std::filesystem::path& path,const void* bytes,size_t count) {
    Require(!std::filesystem::exists(path),"Output exists; choose a new prefix");
    std::ofstream out(path,std::ios::binary); Require(bool(out),"Open output failed"); out.write(static_cast<const char*>(bytes),count); Require(bool(out),"Write failed");
}
int wmain(int argc,wchar_t** argv) try {
    if(argc==2&&std::wstring(argv[1])==L"--self-test") { Gpu gpu; SelfTest(gpu); return 0; }
    Require(argc==3,"Usage: ac6_offline_stereo input.bin NEW-output-prefix, or --self-test");
    std::ifstream input(argv[1],std::ios::binary); Require(bool(input),"Open input failed");
    char magic[8]; Params p{}; input.read(magic,8); input.read(reinterpret_cast<char*>(&p),sizeof(p));
    Require(bool(input)&&std::memcmp(magic,"AC6RPRJ1",8)==0,"Invalid input header"); Validate(p);
    const UINT count=Count(p.sw,p.sh); std::vector<float> depth(count); std::vector<UINT> color(count);
    input.read(reinterpret_cast<char*>(depth.data()),4ull*count); input.read(reinterpret_cast<char*>(color.data()),4ull*count);
    Require(bool(input)&&input.peek()==std::char_traits<char>::eof(),"Truncated or unexpected input size");
    std::filesystem::path prefix(argv[2]); auto rgba=prefix; rgba+=L".rgba.raw"; auto indices=prefix; indices+=L".indices.raw";
    Require(!std::filesystem::exists(rgba)&&!std::filesystem::exists(indices),"Output exists");
    Gpu gpu; auto result=gpu.Run(p,depth,color);
    Write(rgba,result.color.data(),4ull*result.color.size()); Write(indices,result.indices.data(),4ull*result.indices.size());
    const auto covered=std::count_if(result.indices.begin(),result.indices.end(),[](UINT i){return i!=UINT_MAX;});
    std::cout<<"{\"gpu_ms\":"<<result.gpu_ms<<",\"covered_pixels\":"<<covered<<",\"debug_layer\":"<<(gpu.debug_layer?"true":"false")<<",\"upload_and_readback_included_in_gpu_ms\":false}\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
