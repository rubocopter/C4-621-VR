#include "gpu_image_transport.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cmath>
using Microsoft::WRL::ComPtr;
using gpu_capture::GpuImageTransport;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("test HRESULT " + std::to_string(static_cast<unsigned long>(hr))); }
static void Require(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v{}; v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; return v;
}
struct TestGpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value=0;
    TestGpu() {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debug.As(&debug1))) debug1->SetEnableGPUBasedValidation(TRUE);
            std::cout << "D3D12 debug layer + GPU validation enabled\n";
        } else std::cout << "D3D12 debug layer unavailable\n";
        ComPtr<IDXGIFactory6> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        for (UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))==DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d{}; Check(adapter->GetDesc1(&d));
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) {
                std::wcout << L"Hardware adapter: " << d.Description << L"\n"; break;
            }
        }
        Require(device.Get()!=nullptr,"No hardware D3D12 adapter (WARP is deliberately excluded)");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        Check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
        Check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        Check(list->Close()); Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    }
    void Wait() {
        Check(queue->Signal(fence.Get(),++value));
        HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(e!=nullptr,"test event");
        Check(fence->SetEventOnCompletion(value,e)); DWORD result=WaitForSingleObject(e,10000); CloseHandle(e);
        // Do not unwind test GPU resources after a timeout either.
        if(result!=WAIT_OBJECT_0) { std::cerr << "test GPU wait failed\n"; TerminateProcess(GetCurrentProcess(),4); }
    }
    ComPtr<ID3D12Resource> Target(UINT w, UINT h, DXGI_FORMAT f, bool typeless=false) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h;
        d.DepthOrArraySize=1; d.MipLevels=1; d.SampleDesc.Count=1; d.Format=f;
        if(typeless) d.Format=(f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f==DXGI_FORMAT_R8G8B8A8_UNORM)?DXGI_FORMAT_R8G8B8A8_TYPELESS:DXGI_FORMAT_B8G8R8A8_TYPELESS;
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> r;
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&r))); return r;
    }
    std::vector<std::array<int,4>> Read(ID3D12Resource* target, DXGI_FORMAT f) {
        auto d=target->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total=0;
        device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&total);
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC b{}; b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; b.Width=total; b.Height=1;
        b.DepthOrArraySize=1; b.MipLevels=1; b.SampleDesc.Count=1; b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> read;
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)));
        Check(allocator->Reset()); Check(list->Reset(allocator.Get(),nullptr));
        auto barrier=Transition(target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=target; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=read.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        barrier=Transition(target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET); list->ResourceBarrier(1,&barrier);
        Check(list->Close()); ID3D12CommandList* lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists); Wait();
        std::vector<std::array<int,4>> pixels(static_cast<size_t>(d.Width)*d.Height);
        unsigned char* p=nullptr; D3D12_RANGE range{0,static_cast<SIZE_T>(total)}; Check(read->Map(0,&range,reinterpret_cast<void**>(&p)));
        const bool bgra=f==DXGI_FORMAT_B8G8R8A8_UNORM || f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for(UINT y=0;y<d.Height;++y) for(UINT x=0;x<d.Width;++x) {
            auto* v=p+fp.Offset+y*fp.Footprint.RowPitch+x*4;
            pixels[y*static_cast<size_t>(d.Width)+x]={v[bgra?2:0],v[1],v[bgra?0:2],v[3]};
        }
        D3D12_RANGE noWrite{0,0}; read->Unmap(0,&noWrite); return pixels;
    }
    void NoErrors() {
        ComPtr<ID3D12InfoQueue> info; if(FAILED(device.As(&info))) return;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
            SIZE_T n=0; Check(info->GetMessage(i,nullptr,&n)); std::vector<char> bytes(n);
            auto* msg=reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); Check(info->GetMessage(i,msg,&n));
            if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { std::cerr << msg->pDescription << '\n'; throw std::runtime_error("D3D12 validation error"); }
        }
    }
};
int main() try {
    TestGpu gpu;
    {
        GpuImageTransport bridge(gpu.device.Get(),gpu.queue.Get(),2,2);
        ComPtr<ID3D11DeviceContext> ctx; bridge.Device11()->GetImmediateContext(&ctx);
        ComPtr<ID3D11DeviceContext1> ctx1; Check(ctx.As(&ctx1));
        D3D11_TEXTURE2D_DESC sd{}; sd.Width=2; sd.Height=2; sd.MipLevels=1; sd.ArraySize=1;
        sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count=1; sd.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> source; Check(bridge.Device11()->CreateTexture2D(&sd,nullptr,&source));
        ComPtr<ID3D11RenderTargetView> rtv; Check(bridge.Device11()->CreateRenderTargetView(source.Get(),nullptr,&rtv));
        const float colors[4][4]={{1,0,0,1},{0,1,0,1},{0,0,1,1},{128.f/255,128.f/255,128.f/255,1}};
        auto paint=[&] { for(int y=0;y<2;++y) for(int x=0;x<2;++x) { D3D11_RECT r{x,y,x+1,y+1}; ctx1->ClearView(rtv.Get(),colors[y*2+x],&r,1); } };
        const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM};
        for(auto f:formats) for(bool typeless:{false,true}) {
            paint(); bridge.Submit(source.Get());
            auto target=gpu.Target(8,4,f,typeless); bridge.Blit(target.Get(),f);
            Require(bridge.ProducerComplete(),"producer not complete after Blit");
            auto p=gpu.Read(target.Get(),f);
            Require(p[0]==std::array<int,4>{0,0,0,255},"letterbox left or alpha");
            Require(p[7]==std::array<int,4>{0,0,0,255},"letterbox right");
            Require(p[2]==std::array<int,4>{255,0,0,255},"red/channel order/top orientation");
            Require(p[5]==std::array<int,4>{0,255,0,255},"green/channel order");
            Require(p[3*8+2]==std::array<int,4>{0,0,255,255},"blue/bottom orientation");
            Require(std::abs(p[3*8+5][0]-128)<=1,"sRGB midtone preservation");
            auto tall=gpu.Target(4,8,f); bridge.Blit(tall.Get(),f); auto t=gpu.Read(tall.Get(),f);
            Require(t[0]==std::array<int,4>{0,0,0,255} && t[7*4]==t[0],"top/bottom bars");
            // Black/white must blend in linear light, then encode to ~188.
            const float black[4]={0,0,0,1}, white[4]={1,1,1,1};
            ctx->ClearRenderTargetView(rtv.Get(),black); D3D11_RECT right{1,0,2,2}; ctx1->ClearView(rtv.Get(),white,&right,1);
            bridge.Submit(source.Get()); auto tiny=gpu.Target(1,1,f); bridge.Blit(tiny.Get(),f);
            auto g=gpu.Read(tiny.Get(),f); Require(std::abs(g[0][0]-188)<=2,"linear-light filtering / gamma");
        }
        for(int i=0;i<100;++i) {
            float c[4]={i%2?1.f:0.f,i%2?0.f:1.f,0,1};
            ctx->ClearRenderTargetView(rtv.Get(),c); bridge.Submit(source.Get());
            // Two producers before consumer: queued copies cannot corrupt fence order.
            bridge.Submit(source.Get());
            auto target=gpu.Target(13,9,formats[i%4]); bridge.Blit(target.Get(),formats[i%4]);
            auto p=gpu.Read(target.Get(),formats[i%4]); Require(p[4*13+6][0]==(i%2?255:0),"repeated producer/consumer red");
            Require(p[4*13+6][1]==(i%2?0:255),"repeated producer/consumer green");
        }
        auto large=gpu.Target(1280,720,formats[0]); bridge.Blit(large.Get(),formats[0]);
        auto p=gpu.Read(large.Get(),formats[0]); Require(p[360*1280+640][0]==255,"1280x720 blit");
        bool rejected=false; try { bridge.Blit(large.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT); } catch(const std::exception&) { rejected=true; }
        Require(rejected,"invalid target format must fail");
        bridge.Drain();
        // Check D3D11 debug errors as well when the runtime is available.
        ComPtr<ID3D11InfoQueue> info;
        if(SUCCEEDED(bridge.Device11()->QueryInterface(IID_PPV_ARGS(&info)))) {
            for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
                SIZE_T n=0; Check(info->GetMessage(i,nullptr,&n)); std::vector<char> bytes(n);
                auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); Check(info->GetMessage(i,m,&n));
                if(m->Severity<=D3D11_MESSAGE_SEVERITY_ERROR) { std::cerr << m->pDescription << '\n'; throw std::runtime_error("D3D11 validation error"); }
            }
        }
    }
    gpu.NoErrors(); std::cout << "PASS: hardware NT sharing, fence ordering, RGBA/BGRA typed+typeless targets, gamma, scale, letterbox, repeated frames, RT state\n";
    return 0;
} catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
