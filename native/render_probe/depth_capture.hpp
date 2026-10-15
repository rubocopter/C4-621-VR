#pragma once
#include "dummy_device.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>

using CaptureBarrierFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
class DepthCapture {
public:
    enum Stage { Empty,Selecting,Selected,Preparing,Armed,Recording,Recorded,Queued,Complete,Failed };
    std::atomic<Stage> stage{Empty};
    ComPtr<ID3D12Resource> source,readback;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    ID3D12CommandList* recorded_list=nullptr;
    D3D12_RESOURCE_DESC desc{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows=0; UINT64 row_bytes=0,total=0;
    bool synthetic_ok=false;
    UINT recorded_batch_count=0;
    void Discover(ID3D12Resource* resource) {
        if(stage.load()!=Empty) return;
        const auto d=resource->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=2560 || d.Height!=1440 ||
           d.Format!=DXGI_FORMAT_R32G8X24_TYPELESS || d.SampleDesc.Count!=1 || d.DepthOrArraySize!=1 || d.MipLevels!=1) return;
        Stage expected=Empty; if(!stage.compare_exchange_strong(expected,Selecting)) return;
        source=resource; desc=d; stage.store(Selected);
    }
    void Prepare() {
        Stage expected=Selected; if(!stage.compare_exchange_strong(expected,Preparing)) return;
        try {
            Check(source->GetDevice(IID_PPV_ARGS(&device)),"Depth source GetDevice");
            device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&row_bytes,&total);
            if(!total || rows!=desc.Height || row_bytes<desc.Width*4) throw std::runtime_error("Unexpected depth footprint");
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width=total; buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;
            buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"Create depth readback");
            Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Create depth fence");
            stage.store(Armed);
        } catch(...) { stage.store(Failed); throw; }
    }
    void AfterBarrier(ID3D12GraphicsCommandList* list,const D3D12_RESOURCE_BARRIER& transition,CaptureBarrierFn barrier,UINT batch_count) {
        if(stage.load()!=Armed || transition.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || transition.Flags!=D3D12_RESOURCE_BARRIER_FLAG_NONE) return;
        const auto& t=transition.Transition;
        if(t.pResource!=source.Get() || t.StateBefore!=D3D12_RESOURCE_STATE_DEPTH_WRITE ||
           !(t.StateAfter&(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_DEPTH_READ)) ||
           (t.Subresource!=0 && t.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) || list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        Stage expected=Armed; if(!stage.compare_exchange_strong(expected,Recording)) return;
        recorded_batch_count=batch_count;
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource=source.Get(); b.Transition.Subresource=0;
        b.Transition.StateBefore=t.StateAfter; b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier(list,1,&b);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource=source.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource=readback.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint=footprint;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter); barrier(list,1,&b);
        recorded_list=static_cast<ID3D12CommandList*>(list); stage.store(Recorded);
    }
    void AfterExecute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) {
        if(stage.load()!=Recorded) return;
        for(UINT i=0;i<count;++i) if(lists[i]==recorded_list) {
            Stage expected=Recorded; if(!stage.compare_exchange_strong(expected,Queued)) return;
            if(FAILED(queue->Signal(fence.Get(),1))) stage.store(Failed);
            return;
        }
    }
    bool Save(const wchar_t* path,bool synthetic) {
        if(stage.load()!=Queued || fence->GetCompletedValue()<1) return false;
        if(fence->GetCompletedValue()==UINT64_MAX) { stage.store(Failed); return false; }
        void* data=nullptr; const D3D12_RANGE range{0,static_cast<SIZE_T>(total)};
        Check(readback->Map(0,&range,&data),"Map completed depth readback");
        FILE* output=_wfsopen(path,L"wbx",_SH_DENYNO);
        if(!output) { readback->Unmap(0,nullptr); throw std::runtime_error("Depth output already exists or cannot be opened"); }
        bool ok=std::fwrite(data,1,static_cast<std::size_t>(total),output)==total;
        if(synthetic) {
            const auto* bytes=static_cast<const unsigned char*>(data)+footprint.Offset;
            synthetic_ok=true;
            for(UINT y:{0u,desc.Height/2,desc.Height-1}) for(UINT x:{0u,UINT(desc.Width/2),UINT(desc.Width-1)}) {
                float depth=0;
                std::memcpy(&depth,bytes+static_cast<std::size_t>(y)*footprint.Footprint.RowPitch+x*(row_bytes/desc.Width),4);
                synthetic_ok=synthetic_ok && std::isfinite(depth) && std::abs(depth-0.25f)<1e-6f;
            }
        }
        std::fclose(output); const D3D12_RANGE no_writes{0,0}; readback->Unmap(0,&no_writes);
        if(!ok) throw std::runtime_error("Incomplete depth output");
        stage.store(Complete); return true;
    }
    void Log(FILE* file) {
        std::fprintf(file,"{\"phase\":\"depth_capture\",\"stage\":%u,\"written\":%s,\"width\":%llu,\"height\":%u,\"source_format\":%u,\"footprint_format\":%u,\"offset\":%llu,\"row_pitch\":%u,\"row_bytes\":%llu,\"rows\":%u,\"total_bytes\":%llu,\"batch_count\":%u,\"synthetic_ok\":%s}\n",
            unsigned(stage.load()),stage.load()==Complete?"true":"false",desc.Width,desc.Height,desc.Format,footprint.Footprint.Format,footprint.Offset,footprint.Footprint.RowPitch,row_bytes,rows,total,recorded_batch_count,synthetic_ok?"true":"false");
    }
};
