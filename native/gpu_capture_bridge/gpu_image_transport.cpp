#include "gpu_image_transport.hpp"
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <stdexcept>
#include <cstdio>
#include <algorithm>
#include <array>
using Microsoft::WRL::ComPtr;
namespace gpu_capture {
namespace {
void Check(HRESULT hr,const char* op) {
    if(FAILED(hr)) { char s[256]; std::snprintf(s,sizeof(s),"%s failed (HRESULT 0x%08lX)",op,static_cast<unsigned long>(hr)); throw std::runtime_error(s); }
}
struct Handle { HANDLE value=nullptr; ~Handle() { if(value) CloseHandle(value); } };
constexpr DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
int FormatIndex(DXGI_FORMAT f) { for(int n=0;n<4;++n) if(formats[n]==f) return n; throw std::invalid_argument("Blit requires RGBA8/BGRA8 UNORM or UNORM_SRGB"); }
constexpr char shader[]=R"(
Texture2D<float4> image : register(t0);
SamplerState linearClamp : register(s0);
cbuffer Options : register(b0) { uint encodeSrgb; };
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V v; v.uv=float2((id<<1)&2,id&2);
    v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1); return v;
}
float3 encode(float3 c) {
    return float3(c.r<=0.0031308?12.92*c.r:1.055*pow(c.r,1.0/2.4)-0.055,
                  c.g<=0.0031308?12.92*c.g:1.055*pow(c.g,1.0/2.4)-0.055,
                  c.b<=0.0031308?12.92*c.b:1.055*pow(c.b,1.0/2.4)-0.055);
}
float4 ps(V v) : SV_Target {
    float3 c=saturate(image.Sample(linearClamp,v.uv).rgb);
    return float4(encodeSrgb?encode(c):c,1);
}
)";
ComPtr<ID3DBlob> Compile(const char* entry,const char* profile) {
    ComPtr<ID3DBlob> blob,errors;
    HRESULT hr=D3DCompile(shader,sizeof(shader)-1,"gpu_bridge",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&blob,&errors);
    if(FAILED(hr)&&errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
    Check(hr,"D3DCompile"); return blob;
}
D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v{}; v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; return v;
}
}
struct GpuImageTransport::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D11Texture2D> shared11;
    ComPtr<ID3D12Resource> shared12;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> srvHeap,rtvHeap;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,4> pso;
    Handle event;
    UINT width,height;
    UINT64 timeline=0,producer=0,consumer=0;
    // Timeout cannot cancel GPU work. Do not unwind a WGC frame or caller target
    // still in flight: retain them until this standalone process is torn down.
    [[noreturn]] void InFlightFailure(const char* why) const noexcept {
        std::fprintf(stderr,"GpuImageTransport: %s; D3D12 removal=0x%08lX, D3D11 removal=0x%08lX; terminating standalone process with GPU resources retained\n",why,
            static_cast<unsigned long>(device->GetDeviceRemovedReason()),static_cast<unsigned long>(device11->GetDeviceRemovedReason()));
        std::fflush(stderr); TerminateProcess(GetCurrentProcess(),3); ExitProcess(3);
    }
    UINT64 Completed() const {
        UINT64 v=fence->GetCompletedValue(); if(v==UINT64_MAX) InFlightFailure("device removed"); return v;
    }
    void Wait(UINT64 v) const {
        if(!v||Completed()>=v) return;
        if(FAILED(fence->SetEventOnCompletion(v,event.value))) InFlightFailure("SetEventOnCompletion");
        if(WaitForSingleObject(event.value,5000)!=WAIT_OBJECT_0||Completed()<v) InFlightFailure("GPU completion timeout/failure (5000ms)");
    }
    Impl(ID3D12Device* d,ID3D12CommandQueue* q,UINT w,UINT h):device(d),queue(q),width(w),height(h) {
        if(!d||!q||!w||!h||w>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION||h>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            throw std::invalid_argument("Invalid device, queue, or dimensions");
        ComPtr<ID3D12Device> owner; Check(q->GetDevice(IID_PPV_ARGS(&owner)),"queue GetDevice");
        if(owner.Get()!=d||q->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT||q->GetDesc().NodeMask>1||d->GetNodeCount()!=1)
            throw std::invalid_argument("Bridge requires caller device's DIRECT queue on single-node adapter");
        ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> adapter; Check(factory->EnumAdapterByLuid(d->GetAdapterLuid(),IID_PPV_ARGS(&adapter)),"EnumAdapterByLuid");
        ComPtr<ID3D11DeviceContext> immediate;
        UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags|=D3D11_CREATE_DEVICE_DEBUG;
#endif
        HRESULT hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,flags,nullptr,0,D3D11_SDK_VERSION,&device11,nullptr,&immediate);
        if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING) hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device11,nullptr,&immediate);
        Check(hr,"D3D11CreateDevice on D3D12 adapter"); Check(immediate.As(&context),"ID3D11DeviceContext4");
        ComPtr<ID3D11Device5> device5; Check(device11.As(&device5),"ID3D11Device5");
        Check(d->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"Create shared fence");
        Handle fh; Check(d->CreateSharedHandle(fence.Get(),nullptr,GENERIC_ALL,nullptr,&fh.value),"Create fence NT handle");
        Check(device5->OpenSharedFence(fh.value,IID_PPV_ARGS(&fence11)),"OpenSharedFence");
        D3D11_TEXTURE2D_DESC td{}; td.Width=w; td.Height=h; td.MipLevels=1; td.ArraySize=1;
        td.Format=DXGI_FORMAT_B8G8R8A8_TYPELESS; td.SampleDesc.Count=1;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        Check(device11->CreateTexture2D(&td,nullptr,&shared11),"Create shared BGRA texture");
        ComPtr<IDXGIResource1> resource; Check(shared11.As(&resource),"IDXGIResource1");
        Handle th; Check(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&th.value),"Create texture NT handle");
        Check(d->OpenSharedHandle(th.value,IID_PPV_ARGS(&shared12)),"Open shared texture on D3D12");
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors=1; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(d->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&srvHeap)),"Create SRV heap");
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Check(d->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&rtvHeap)),"Create RTV heap");
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels=1;
        d->CreateShaderResourceView(shared12.Get(),&srv,srvHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_DESCRIPTOR_RANGE range{}; range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors=1;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable={1,&range}; params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[1].Constants={0,0,1}; params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs{}; rs.NumParameters=2; rs.pParameters=params; rs.NumStaticSamplers=1; rs.pStaticSamplers=&sampler;
        ComPtr<ID3DBlob> serialized,errors; Check(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"Serialize root signature");
        Check(d->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)),"Create root signature");
        auto vs=Compile("vs","vs_5_0"),ps=Compile("ps","ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get(); pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        auto& blend=pd.BlendState.RenderTarget[0]; blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;
        blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD; blend.LogicOp=D3D12_LOGIC_OP_NOOP;
        pd.SampleMask=UINT_MAX; pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable=TRUE;
        pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets=1; pd.SampleDesc.Count=1;
        for(int n=0;n<4;++n) { pd.RTVFormats[0]=formats[n]; Check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso[n])),"Create blit PSO"); }
        Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Create allocator");
        Check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Create list");
        Check(list->Close(),"Close initial list");
        event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!event.value) Check(HRESULT_FROM_WIN32(GetLastError()),"CreateEvent");
    }
};
GpuImageTransport::GpuImageTransport(ID3D12Device* d,ID3D12CommandQueue* q,UINT w,UINT h):impl_(std::make_unique<Impl>(d,q,w,h)) {}
GpuImageTransport::~GpuImageTransport() { if(impl_) impl_->Wait(impl_->timeline); }
ID3D11Device* GpuImageTransport::Device11() const noexcept { return impl_->device11.Get(); }
bool GpuImageTransport::ProducerComplete() const { return impl_->Completed()>=impl_->producer; }
void GpuImageTransport::Drain() { impl_->Wait(impl_->timeline); }
void GpuImageTransport::Submit(ID3D11Texture2D* source) {
    auto& i=*impl_; if(!source) throw std::invalid_argument("Submit source is null");
    D3D11_TEXTURE2D_DESC d{}; source->GetDesc(&d); ComPtr<ID3D11Device> owner; source->GetDevice(&owner);
    if(owner.Get()!=i.device11.Get()||d.Width!=i.width||d.Height!=i.height||d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||d.ArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1)
        throw std::invalid_argument("Submit requires same-device exact-size single-subresource BGRA8 UNORM texture");
    if(i.consumer&&FAILED(i.context->Wait(i.fence11.Get(),i.consumer))) i.InFlightFailure("D3D11 wait for consumer");
    i.context->CopyResource(i.shared11.Get(),source); i.producer=++i.timeline;
    if(FAILED(i.context->Signal(i.fence11.Get(),i.producer))) i.InFlightFailure("D3D11 producer signal");
    i.context->Flush(); // publish both copy and fence even without subsequent D3D11 work
}
void GpuImageTransport::Blit(ID3D12Resource* target,DXGI_FORMAT format) {
    auto& i=*impl_; const int index=FormatIndex(format);
    if(!i.producer) throw std::logic_error("Blit before first frame");
    if(!target) throw std::invalid_argument("Blit target is null");
    ComPtr<ID3D12Device> owner; Check(target->GetDevice(IID_PPV_ARGS(&owner)),"target GetDevice"); auto d=target->GetDesc();
    const auto typeless=index<2?DXGI_FORMAT_R8G8B8A8_TYPELESS:DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if(owner.Get()!=i.device.Get()||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)||(d.Format!=format&&d.Format!=typeless))
        throw std::invalid_argument("Blit target requires same-device compatible RT-capable 2D texture, one mip/slice/sample");
    i.Wait(i.consumer); // safe allocator/descriptor reuse
    Check(i.allocator->Reset(),"Reset allocator"); Check(i.list->Reset(i.allocator.Get(),i.pso[index].Get()),"Reset list");
    D3D12_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format=format; rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
    auto handle=i.rtvHeap->GetCPUDescriptorHandleForHeapStart(); i.device->CreateRenderTargetView(target,&rtv,handle);
    auto barrier=Barrier(i.shared12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); i.list->ResourceBarrier(1,&barrier);
    i.list->SetGraphicsRootSignature(i.root.Get()); ID3D12DescriptorHeap* heaps[]={i.srvHeap.Get()}; i.list->SetDescriptorHeaps(1,heaps);
    i.list->SetGraphicsRootDescriptorTable(0,i.srvHeap->GetGPUDescriptorHandleForHeapStart());
    const UINT encode=(index%2)==0; i.list->SetGraphicsRoot32BitConstant(1,encode,0);
    i.list->OMSetRenderTargets(1,&handle,FALSE,nullptr); const float black[4]={0,0,0,1}; i.list->ClearRenderTargetView(handle,black,0,nullptr);
    const double scale=std::min(static_cast<double>(d.Width)/i.width,static_cast<double>(d.Height)/i.height);
    const float w=static_cast<float>(i.width*scale),h=static_cast<float>(i.height*scale);
    D3D12_VIEWPORT viewport{(static_cast<float>(d.Width)-w)*0.5f,(static_cast<float>(d.Height)-h)*0.5f,w,h,0,1};
    D3D12_RECT scissor{0,0,static_cast<LONG>(d.Width),static_cast<LONG>(d.Height)};
    i.list->RSSetViewports(1,&viewport); i.list->RSSetScissorRects(1,&scissor);
    i.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); i.list->DrawInstanced(3,1,0,0);
    barrier=Barrier(i.shared12.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON); i.list->ResourceBarrier(1,&barrier);
    Check(i.list->Close(),"Close blit list");
    if(FAILED(i.queue->Wait(i.fence.Get(),i.producer))) i.InFlightFailure("D3D12 wait for producer");
    ID3D12CommandList* lists[]={i.list.Get()}; i.queue->ExecuteCommandLists(1,lists); i.consumer=++i.timeline;
    if(FAILED(i.queue->Signal(i.fence.Get(),i.consumer))) i.InFlightFailure("D3D12 consumer signal"); i.Wait(i.consumer);
}
}
