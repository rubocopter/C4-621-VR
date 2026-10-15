#pragma once
#include "xr_api.hpp"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>

using Microsoft::WRL::ComPtr;
struct GpuUpload {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> upload;
    HANDLE completed=nullptr;
    UINT64 tick=0;
    GpuUpload(const XrGraphicsRequirementsD3D12KHR& requirements) {
        ComPtr<IDXGIFactory4> factory;
        HCheck(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
        HCheck(factory->EnumAdapterByLuid(requirements.adapterLuid,IID_PPV_ARGS(&adapter)),"EnumAdapterByLuid");
        HCheck(D3D12CreateDevice(adapter.Get(),requirements.minFeatureLevel,IID_PPV_ARGS(&device)),"D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        HCheck(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"CreateCommandQueue");
        HCheck(device->CreateCommandAllocator(desc.Type,IID_PPV_ARGS(&allocator)),"CreateCommandAllocator");
        HCheck(device->CreateCommandList(0,desc.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"CreateCommandList");
        HCheck(commands->Close(),"Close command list");
        HCheck(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"CreateFence");
        completed=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!completed) throw std::runtime_error("CreateEvent failed");
    }
    ~GpuUpload() { if(completed) CloseHandle(completed); }
    [[noreturn]] void FailedInFlight(const char* message) {
        // A timeout does not cancel GPU work. Do not unwind COM resources still
        // referenced by the queue. Device removal cancels that work; otherwise
        // end only this standalone probe and let the OS tear down its context.
        const auto removed=device->GetDeviceRemovedReason();
        if(FAILED(removed)) HCheck(removed,message);
        std::fprintf(stderr,"%s; pending GPU resources retained until probe process teardown\n",message);
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(),3);
        ExitProcess(3);
    }
    void Put(ID3D12Resource* texture,const std::vector<std::uint8_t>& rgba,DXGI_FORMAT format) {
        auto desc=texture->GetDesc(); desc.Format=format;
        if(rgba.size()!=desc.Width*desc.Height*4) throw std::runtime_error("Unexpected upload dimensions");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT rows=0; UINT64 row_bytes=0,total=0;
        device->GetCopyableFootprints(&desc,0,1,0,&layout,&rows,&row_bytes,&total);
        if(!upload || upload->GetDesc().Width<total) {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width=total; buffer.Height=1; buffer.DepthOrArraySize=1; buffer.MipLevels=1;
            buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            upload.Reset();
            HCheck(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"Create upload buffer");
        }
        void* mapped=nullptr; const D3D12_RANGE no_reads{0,0};
        HCheck(upload->Map(0,&no_reads,&mapped),"Map upload");
        auto* dest=static_cast<std::uint8_t*>(mapped)+layout.Offset;
        for(UINT y=0;y<rows;++y) {
            std::memcpy(dest+y*layout.Footprint.RowPitch,rgba.data()+y*row_bytes,static_cast<std::size_t>(row_bytes));
            if(format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format==DXGI_FORMAT_B8G8R8A8_UNORM)
                for(UINT x=0;x<desc.Width;++x) std::swap(dest[y*layout.Footprint.RowPitch+x*4],dest[y*layout.Footprint.RowPitch+x*4+2]);
        }
        upload->Unmap(0,nullptr);
        HCheck(allocator->Reset(),"Reset allocator");
        HCheck(commands->Reset(allocator.Get(),nullptr),"Reset command list");
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=texture; barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        commands->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=layout;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=texture; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        commands->ResourceBarrier(1,&barrier);
        HCheck(commands->Close(),"Close upload list");
        ID3D12CommandList* lists[]={commands.Get()}; queue->ExecuteCommandLists(1,lists);
        if(FAILED(queue->Signal(fence.Get(),++tick))) FailedInFlight("Signal upload fence failed");
        if(fence->GetCompletedValue()<tick) {
            if(FAILED(fence->SetEventOnCompletion(tick,completed))) FailedInFlight("SetEventOnCompletion failed");
            if(WaitForSingleObject(completed,5000)!=WAIT_OBJECT_0) FailedInFlight("GPU upload fence timed out");
        }
    }
};
