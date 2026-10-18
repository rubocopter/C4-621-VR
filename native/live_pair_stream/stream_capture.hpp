#pragma once
#include "protocol.hpp"
#include "../frame_pair_probe/pair_capture.hpp"
#include <vector>
#include <new>
namespace live_pair {
struct StreamCapture : frame_pair::Capture {
    Header* shared=nullptr;
    std::atomic<Header*> published_header{nullptr};
    ComPtr<ID3D12Fence> released;
    UINT64 frames=0,dropped=0;
    std::wstring mapping_name;
    struct GpuSlot { ComPtr<ID3D12Resource> buffer; std::array<ComPtr<ID3D12GraphicsCommandList>,3> copies; };
    std::array<GpuSlot,3> slots;
    HANDLE mapping=nullptr;
    std::vector<HANDLE> handles;
    ComPtr<ID3D12CommandQueue> source_queue;
    bool depth_since_capture=false;
    struct QueueObservation {
        frame_pair::Id identity=0,device_identity=0;
        D3D12_COMMAND_LIST_TYPE type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        UINT submissions=0,lists=0,untrusted=0,depth_lists=0,color_lists=0,aliasing=0;
        bool same_device=false;
        std::array<void*,12> stack{};
        USHORT stack_count=0;
    };
    std::array<QueueObservation,8> queue_observations{};
    UINT queue_observation_count=0;
    void ObserveQueue(ID3D12CommandQueue* queue,UINT n,ID3D12CommandList* const* lists) {
        const auto id=reinterpret_cast<frame_pair::Id>(queue);
        QueueObservation* observation=nullptr;
        for(UINT i=0;i<queue_observation_count;++i) if(queue_observations[i].identity==id) observation=&queue_observations[i];
        if(!observation&&queue_observation_count<queue_observations.size()) {
            observation=&queue_observations[queue_observation_count++];
            observation->identity=id;observation->type=queue->GetDesc().Type;
            observation->stack_count=CaptureStackBackTrace(0,static_cast<DWORD>(observation->stack.size()),observation->stack.data(),nullptr);
            ComPtr<ID3D12Device> queue_device;
            if(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queue_device)))) {
                observation->device_identity=reinterpret_cast<frame_pair::Id>(queue_device.Get());
                observation->same_device=queue_device.Get()==device.Get();
            }
        }
        if(!observation) return;
        ++observation->submissions;observation->lists+=n;
        for(UINT i=0;i<n;++i) {
            const auto summary=ledger->Snapshot(reinterpret_cast<frame_pair::Id>(lists[i]));
            observation->untrusted+=!summary.trusted;observation->depth_lists+=summary.depth_touched;
            observation->color_lists+=summary.color_touched!=0;observation->aliasing+=summary.aliasing;
        }
    }
    void Fail() { frame_pair::Capture::Fail(); if(auto* h=published_header.load()) InterlockedExchange(&h->status,live_pair::Failed); }
    void Resize() { std::lock_guard lock(mutex); Fail(); }
    void PrepareStream(const std::wstring& name) {
        std::unique_lock lock(mutex);
        if(stage.load()!=Collecting||!depth||!swap) return;
        if(depth_desc.Width!=color_desc.Width||depth_desc.Height!=color_desc.Height) throw std::runtime_error("Source dimensions differ");
        stage.store(Preparing); lock.unlock();
        mapping_name=name;
        Check(depth->GetDevice(IID_PPV_ARGS(&device)),"Stream device");
        ComPtr<ID3D12Device> color_device; Check(swap->GetDevice(IID_PPV_ARGS(&color_device)),"Color device");
        if(device.Get()!=color_device.Get()||device->GetNodeCount()!=1) throw std::runtime_error("Stream requires same single-node device");
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Header),name.c_str());
        if(!mapping) throw std::runtime_error("Create stream mapping failed");
        if(GetLastError()==ERROR_ALREADY_EXISTS) { CloseHandle(mapping);mapping=nullptr;throw std::runtime_error("Stream mapping already exists"); }
        auto* memory=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Header));
        if(!memory) throw std::runtime_error("Map stream metadata failed");
        shared=::new(memory) Header;
        published_header.store(shared);
        shared->producer_pid=GetCurrentProcessId(); shared->adapter=device->GetAdapterLuid();
        shared->width=static_cast<UINT>(color_desc.Width); shared->height=color_desc.Height; shared->color_format=color_desc.Format;
        device->GetCopyableFootprints(&depth_desc,0,1,0,&d.footprint,&d.rows,nullptr,&d.bytes);
        UINT64 color_base=d.footprint.Offset+UINT64(d.footprint.Footprint.RowPitch)*d.rows;
        color_base=(color_base+511)&~UINT64(511);
        device->GetCopyableFootprints(&color_desc,0,1,color_base,&c.footprint,&c.rows,nullptr,&c.bytes);
        shared->depth_offset=d.footprint.Offset; shared->color_offset=c.footprint.Offset;
        shared->depth_pitch=d.footprint.Footprint.RowPitch;shared->color_pitch=c.footprint.Footprint.RowPitch;
        shared->buffer_bytes=c.footprint.Offset+UINT64(c.footprint.Footprint.RowPitch)*c.rows;
        if(d.rows!=shared->height||c.rows!=shared->height||d.footprint.Footprint.Format!=DXGI_FORMAT_R32_TYPELESS||!Valid(*shared)) throw std::runtime_error("Unsupported shared footprint");
        auto share=[&](ID3D12DeviceChild* object,const std::wstring& suffix) { HANDLE h=nullptr;Check(device->CreateSharedHandle(object,nullptr,GENERIC_ALL,(name+suffix).c_str(),&h),"Create shared GPU handle");handles.push_back(h); };
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"Shared ready fence");
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&released)),"Shared release fence");
        share(fence.Get(),L"_ready");share(released.Get(),L"_released");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&copy_allocator)),"Stream allocator");
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=shared->buffer_bytes;desc.Height=1;
        desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for(unsigned s=0;s<3;++s) {
            auto& slot=slots[s];Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_SHARED,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&slot.buffer)),"Shared pair buffer");
            share(slot.buffer.Get(),L"_buffer"+std::to_wstring(s));
            d.readback=slot.buffer;c.readback=slot.buffer;
            for(unsigned i=0;i<3;++i) if(colors[i]) {
                ComPtr<ID3D12Resource> color;Check(swap->GetBuffer(i,IID_PPV_ARGS(&color)),"Prepare stream color");
                if(reinterpret_cast<frame_pair::Id>(color.Get())!=colors[i]||!CompatibleColor(color.Get())) throw std::runtime_error("Swapchain changed during stream preparation");
                auto& cmd=slot.copies[i];Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,copy_allocator.Get(),nullptr,IID_PPV_ARGS(&cmd)),"Stream copy list");
                D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={slot.buffer.Get(),0,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST};
                cmd->ResourceBarrier(1,&b); BuildCopy(cmd.Get(),color.Get()); std::swap(b.Transition.StateBefore,b.Transition.StateAfter);cmd->ResourceBarrier(1,&b);
                Check(cmd->Close(),"Close stream list");
            }
        }
        ledger=std::make_unique<frame_pair::Ledger>(reinterpret_cast<frame_pair::Id>(depth.Get()),colors);
        lock.lock();if(!failed.load()) { InterlockedExchange(&shared->status,Ready);stage.store(Armed); }
    }
    void Execute(ID3D12CommandQueue* queue,UINT n,ID3D12CommandList* const* lists,frame_pair::ExecuteFn original) noexcept {
        bool forwarded=false;
        try {
            std::lock_guard lock(mutex);
            if(stage.load()!=Armed) { forwarded=true;original(queue,n,lists);return; }
            ObserveQueue(queue,n,lists);
            const auto id=reinterpret_cast<frame_pair::Id>(queue);
            if(execution_queue&&execution_queue!=id) { multiple_queues=true;Fail(); }
            execution_queue=id;
            UINT mask=0;
            for(UINT i=0;i<n;++i) {
                auto summary=ledger->Snapshot(reinterpret_cast<frame_pair::Id>(lists[i]));
                if(!summary.trusted||summary.aliasing) { executed_depth_known=false;depth_since_capture=false;mask=0;continue; }
                if(summary.depth_touched) { executed_depth_known=summary.depth_known;executed_depth_state=summary.depth_state;depth_since_capture=summary.depth_known; }
                mask=(mask&~summary.color_touched)|summary.color_present;
            }
            if(ledger->unsafe.load()) Fail();
            const UINT ds=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if(failed.load()||ledger->unsafe.load()||multiple_queues||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT||
               !executed_depth_known||!depth_since_capture||executed_depth_state!=ds||!mask||(mask&(mask-1))||n>256) { forwarded=true;original(queue,n,lists);return; }
            const auto ready=fence->GetCompletedValue(),done=released->GetCompletedValue();
            if(ready==UINT64_MAX||done==UINT64_MAX) { Fail();forwarded=true;original(queue,n,lists);return; }
            int selected=-1;
            for(unsigned s=0;s<3;++s) if(Reusable(Read64(shared->slots[s].sequence),ready,done)) { selected=s;break; }
            if(selected<0) { ++dropped;depth_since_capture=false;forwarded=true;original(queue,n,lists);return; }
            UINT index=0;while(!(mask&(1u<<index))) ++index;
            ComPtr<ID3D12Resource> color;
            if(FAILED(swap->GetBuffer(index,IID_PPV_ARGS(&color)))||reinterpret_cast<frame_pair::Id>(color.Get())!=colors[index]||!CompatibleColor(color.Get())) { Fail();forwarded=true;original(queue,n,lists);return; }
            if(!source_queue) source_queue=queue;
            std::array<ID3D12CommandList*,257> batch{};for(UINT i=0;i<n;++i) batch[i]=lists[i];batch[n]=slots[selected].copies[index].Get();
            forwarded=true;original(queue,n+1,batch.data());
            const UINT64 next=frames+1;
            if(FAILED(queue->Signal(fence.Get(),next))) { Fail();return; }
            frames=next;depth_since_capture=false;
            InterlockedExchange64(&shared->slots[selected].tick,GetTickCount64());
            InterlockedExchange64(&shared->slots[selected].sequence,next);
        } catch(...) { Fail();if(!forwarded) original(queue,n,lists); }
    }
    void StopStream() {
        std::lock_guard lock(mutex);
        if(!failed.load()) stage.store(Complete);
        if(shared) InterlockedExchange(&shared->status,failed.load()?live_pair::Failed:Stopped);
    }
    bool VerifyFixtureSlot(unsigned slot,float expected_depth,UINT expected_rgb) {
        if(slot>=3||!source_queue) return false;
        const auto bytes=shared->buffer_bytes;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        auto desc=slots[slot].buffer->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> read;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)),"Fixture readback");
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Fixture reader allocator");
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Fixture reader list");
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={slots[slot].buffer.Get(),0,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&b);list->CopyResource(read.Get(),slots[slot].buffer.Get());std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);Check(list->Close(),"Fixture reader close");
        ID3D12CommandList* batch[]={list.Get()};source_queue->ExecuteCommandLists(1,batch);
        ComPtr<ID3D12Fence> f;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)),"Fixture reader fence");Check(source_queue->Signal(f.Get(),1),"Fixture reader signal");
        const auto until=GetTickCount64()+5000;while(f->GetCompletedValue()<1&&GetTickCount64()<until) Sleep(1);
        if(f->GetCompletedValue()<1||f->GetCompletedValue()==UINT64_MAX) TerminateProcess(GetCurrentProcess(),3);
        void* memory=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};Check(read->Map(0,&range,&memory),"Fixture reader map");
        float value;UINT rgb;std::memcpy(&value,static_cast<char*>(memory)+shared->depth_offset+100*shared->depth_pitch+200*4,4);
        std::memcpy(&rgb,static_cast<char*>(memory)+shared->color_offset+100*shared->color_pitch+200*4,4);
        D3D12_RANGE noWrite{0,0};read->Unmap(0,&noWrite);
        bool ok=std::abs(value-expected_depth)<1e-6;
        for(unsigned shift:{0u,10u,20u}) ok=ok&&std::abs(int((rgb>>shift)&1023)-int((expected_rgb>>shift)&1023))<=1;
        return ok;
    }
};
}
