#pragma once
#include "../render_probe/dummy_device.hpp"
#include "pair_ledger.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <share.h>
#include <cmath>
#include <cstring>
namespace frame_pair {
using ExecuteFn=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
using BarrierFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
using ResetFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
class Capture {
public:
    enum Stage { Collecting,Preparing,Armed,Capturing,Recorded,Submitted,Saving,Complete,Failed };
    std::atomic<Stage> stage{Collecting};
    std::atomic<bool> failed{false};
    std::atomic<unsigned> missed{0};
    std::atomic<unsigned> depth_transitions{0},color_transitions{0},local_pairs{0};
    std::mutex mutex;
    ComPtr<ID3D12Resource> depth;
    // Borrow only identities. Holding GetBuffer references would block ResizeBuffers.
    std::array<Id,3> colors{};
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> copy_allocator;
    std::array<ComPtr<ID3D12GraphicsCommandList>,3> copy_lists;
    std::atomic<ID3D12CommandList*> recorded_list{nullptr};
    std::atomic<unsigned> active_executes{0};
    std::atomic<bool> retired{false};
    std::atomic<UINT64> signal_value{0};
    Id execution_queue=0;
    bool multiple_queues=false,executed_depth_known=false;
    UINT executed_depth_state=0;
    struct Plane { ComPtr<ID3D12Resource> readback; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes=0; UINT rows=0; };
    Plane d,c;
    D3D12_RESOURCE_DESC depth_desc{},color_desc{};
    std::unique_ptr<Ledger> ledger;
    UINT buffer_index=0,depth_state=0;
    bool synthetic_ok=false;
    void Fail() { failed.store(true); stage.store(Failed); }
    bool ReadEligible() const {
        // Retirement forbids any new matching executions. Once all entered
        // callbacks have exited, recheck terminal failure and publication LAST.
        if(!retired.load() || active_executes.load() || !signal_value.load()) return false;
        return !failed.load() && stage.load()==Submitted;
    }
    std::unique_lock<std::mutex> TryLock() {
        std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);
        return lock;
    }
    void Record(ID3D12GraphicsCommandList* cmd,UINT count,const D3D12_RESOURCE_BARRIER* barriers,BarrierFn original) {
        if(stage.load()!=Collecting && stage.load()!=Armed) return;
        std::unique_lock<std::mutex> lock;
        if(stage.load()==Collecting) { lock=TryLock(); if(!lock.owns_lock()) return; }
        for(UINT i=0;i<count;++i) {
            const auto& b=barriers[i];
            if(stage.load()==Collecting && b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource &&
               ((b.Transition.StateBefore|b.Transition.StateAfter)&(D3D12_RESOURCE_STATE_DEPTH_READ|D3D12_RESOURCE_STATE_DEPTH_WRITE))) {
                const auto desc=b.Transition.pResource->GetDesc();
                if(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS &&
                   desc.Width>=128 && desc.Height>=128 && desc.MipLevels==1 && desc.DepthOrArraySize==1 && desc.SampleDesc.Count==1 &&
                   (!depth || desc.Width*desc.Height>depth_desc.Width*depth_desc.Height)) { depth=b.Transition.pResource; depth_desc=desc; }
            } else if(stage.load()==Armed) {
                ledger->Record(reinterpret_cast<Id>(cmd),b);
                if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource==depth.Get()) ++depth_transitions;
            }
        }
        if(stage.load()!=Armed || missed.load() || cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        // Record identities only. Color and depth may belong to different lists.
        for(UINT i=0;i<count;++i) {
            const auto& b=barriers[i];
            if(b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_NONE ||
               b.Transition.StateBefore!=D3D12_RESOURCE_STATE_RENDER_TARGET || b.Transition.StateAfter!=D3D12_RESOURCE_STATE_PRESENT ||
               (b.Transition.Subresource!=0 && b.Transition.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) continue;
            for(UINT index=0;index<3;++index) if(colors[index]==reinterpret_cast<Id>(b.Transition.pResource)) {
                ++color_transitions;
            }
        }
    }
    void Reset(ID3D12GraphicsCommandList* cmd) {
        if(stage.load()!=Armed) return;
        if(stage.load()==Armed) ledger->Reset(reinterpret_cast<Id>(cmd));
    }
    void Close(ID3D12GraphicsCommandList* cmd) {
        if(stage.load()!=Armed) return;
        if(stage.load()==Armed) ledger->Seal(reinterpret_cast<Id>(cmd));
    }
    void Resize() {
        const auto s=stage.load();
        if(s==Collecting || s==Preparing || s==Armed) Fail();
    }
    void Execute(ID3D12CommandQueue* q,UINT count,ID3D12CommandList* const* lists,ExecuteFn original) {
        ++active_executes;
        struct Guard { std::atomic<unsigned>& n; ~Guard() { --n; } } guard{active_executes};
        // All callbacks respect this gate, including Capturing and failure.
        // Serialize CPU enqueue/state bookkeeping. No GPU or
        // file waits here. Appending to ONE ExecuteCommandLists call prevents
        // another submitting thread from slipping between rendering and copy.
        std::unique_lock lock(mutex);
        if(stage.load()!=Armed) { original(q,count,lists); return; }
        const auto qid=reinterpret_cast<Id>(q);
        if(execution_queue && execution_queue!=qid) multiple_queues=true;
        execution_queue=qid;
        UINT color_mask=0;
        for(UINT i=0;i<count;++i) {
            const auto s=ledger->Snapshot(reinterpret_cast<Id>(lists[i]));
            if(!s.trusted || s.aliasing) { executed_depth_known=false; color_mask=0; continue; }
            if(s.depth_touched) { executed_depth_known=s.depth_known; executed_depth_state=s.depth_state; }
            color_mask=(color_mask&~s.color_touched)|s.color_present;
        }
        const UINT supported_depth=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const bool eligible=!failed.load() && !ledger->unsafe.load() && !multiple_queues &&
            q->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT && executed_depth_known && executed_depth_state==supported_depth &&
            color_mask && !(color_mask&(color_mask-1)) && count<=256;
        if(!eligible) { original(q,count,lists); return; }
        UINT index=0; while(!(color_mask&(1u<<index))) ++index;
        ComPtr<ID3D12Resource> actual;
        if(FAILED(swap->GetBuffer(index,IID_PPV_ARGS(&actual))) || reinterpret_cast<Id>(actual.Get())!=colors[index] || !CompatibleColor(actual.Get())) {
            Fail(); original(q,count,lists); return;
        }
        stage.store(Capturing); buffer_index=index; depth_state=supported_depth; ++local_pairs;
        std::array<ID3D12CommandList*,257> batch{};
        for(UINT i=0;i<count;++i) batch[i]=lists[i];
        batch[count]=copy_lists[index].Get(); recorded_list.store(batch[count]);
        original(q,count+1,batch.data());
        if(FAILED(q->Signal(fence.Get(),1))) Fail();
        else { signal_value.store(1); retired.store(true); if(!failed.load()) stage.store(Submitted); }
    }
    void ObserveSwap(IDXGISwapChain* incoming) {
        if(swap) return;
        DXGI_SWAP_CHAIN_DESC desc{}; if(FAILED(incoming->GetDesc(&desc))) return;
        DWORD owner=0; GetWindowThreadProcessId(desc.OutputWindow,&owner);
        if(owner!=GetCurrentProcessId() || desc.BufferDesc.Width<128 || desc.BufferDesc.Height<128 || desc.BufferCount>3 || desc.BufferCount<2) return;
        if(desc.BufferDesc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM && desc.BufferDesc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.BufferDesc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM) return;
        if(FAILED(incoming->QueryInterface(IID_PPV_ARGS(&swap)))) return;
        for(UINT i=0;i<desc.BufferCount;++i) {
            ComPtr<ID3D12Resource> buffer; Check(swap->GetBuffer(i,IID_PPV_ARGS(&buffer)),"Pair GetBuffer");
            colors[i]=reinterpret_cast<Id>(buffer.Get()); color_desc=buffer->GetDesc();
        }
    }
    void Prepare() {
        std::unique_lock lock(mutex);
        if(stage.load()!=Collecting || !depth || !swap) return;
        if(depth_desc.Width!=color_desc.Width || depth_desc.Height!=color_desc.Height) throw std::runtime_error("Depth/backbuffer dimensions differ");
        stage.store(Preparing);
        lock.unlock(); // GPU resource preparation must not hold the enqueue gate
        Check(depth->GetDevice(IID_PPV_ARGS(&device)),"Pair depth device");
        ComPtr<ID3D12Device> color_device; Check(swap->GetDevice(IID_PPV_ARGS(&color_device)),"Pair color device");
        if(device.Get()!=color_device.Get()) throw std::runtime_error("Depth/backbuffer devices differ");
        auto prepare_plane=[&](Plane& plane,const D3D12_RESOURCE_DESC& desc) {
            device->GetCopyableFootprints(&desc,0,1,0,&plane.footprint,&plane.rows,nullptr,&plane.bytes);
            if(!plane.bytes || plane.rows!=desc.Height) throw std::runtime_error("Unsupported pair footprint");
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC b{}; b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; b.Width=plane.bytes;
            b.Height=b.DepthOrArraySize=b.MipLevels=1; b.SampleDesc.Count=1; b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&plane.readback)),"Pair readback");
        };
        prepare_plane(d,depth_desc); prepare_plane(c,color_desc);
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Pair fence");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&copy_allocator)),"Pair copy allocator");
        for(UINT i=0;i<3;++i) if(colors[i]) {
            ComPtr<ID3D12Resource> color; Check(swap->GetBuffer(i,IID_PPV_ARGS(&color)),"Prepare pair buffer");
            if(reinterpret_cast<Id>(color.Get())!=colors[i] || !CompatibleColor(color.Get())) throw std::runtime_error("Swapchain changed during pair preparation");
            Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,copy_allocator.Get(),nullptr,IID_PPV_ARGS(&copy_lists[i])),"Pair copy list");
            BuildCopy(copy_lists[i].Get(),color.Get()); Check(copy_lists[i]->Close(),"Close pair copy list");
        }
        ledger=std::make_unique<Ledger>(reinterpret_cast<Id>(depth.Get()),colors);
        lock.lock();
        if(!failed.load()) stage.store(Armed);
    }
    void Present(IDXGISwapChain* incoming,UINT flags) {
        if((flags&DXGI_PRESENT_TEST) || stage.load()!=Collecting) return;
        auto lock=TryLock(); if(!lock.owns_lock()) return;
        if(stage.load()==Collecting) ObserveSwap(incoming);
    }
    bool CompatibleColor(ID3D12Resource* color) const {
            const auto actual=color->GetDesc();
            return actual.Dimension==color_desc.Dimension && actual.Width==color_desc.Width && actual.Height==color_desc.Height &&
               actual.Format==color_desc.Format && actual.MipLevels==1 && actual.DepthOrArraySize==1 && actual.SampleDesc.Count==1;
    }
    void BuildCopy(ID3D12GraphicsCommandList* cmd,ID3D12Resource* color) {
            D3D12_RESOURCE_BARRIER transitions[2]{};
            for(auto& b:transitions) b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transitions[0].Transition={depth.Get(),0,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE};
            transitions[1].Transition={color,0,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE};
            cmd->ResourceBarrier(2,transitions);
            auto copy=[&](ID3D12Resource* from,Plane& plane) {
                D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=from; source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION dest{}; dest.pResource=plane.readback.Get(); dest.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dest.PlacedFootprint=plane.footprint;
                cmd->CopyTextureRegion(&dest,0,0,0,&source,nullptr);
            };
            copy(depth.Get(),d); copy(color,c);
            for(auto& b:transitions) std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
            cmd->ResourceBarrier(2,transitions);
    }
    bool Save(const std::wstring& prefix,bool synthetic) {
        std::unique_lock lock(mutex);
        if(!ReadEligible()) return false;
        const auto done=fence->GetCompletedValue();
        if(done==UINT64_MAX) throw std::runtime_error("Pair device removed");
        if(done<signal_value.load()) return false;
        stage.store(Saving); lock.unlock(); // never hold enqueue gate during IO
        void* dd=nullptr; void* cc=nullptr;
        D3D12_RANGE dr{0,static_cast<SIZE_T>(d.bytes)},cr{0,static_cast<SIZE_T>(c.bytes)};
        Check(d.readback->Map(0,&dr,&dd),"Pair map depth"); Check(c.readback->Map(0,&cr,&cc),"Pair map color");
        auto save=[&](const wchar_t* suffix,void* data,UINT64 size) {
            FILE* f=_wfsopen((prefix+suffix).c_str(),L"wbx",_SH_DENYNO); if(!f) throw std::runtime_error("Pair output exists or cannot be written");
            const bool ok=std::fwrite(data,1,static_cast<size_t>(size),f)==size; const int closed=std::fclose(f);
            if(!ok || closed) throw std::runtime_error("Pair output incomplete");
        };
        if(synthetic) {
            auto value=[&](UINT x,UINT y) { float f=0; std::memcpy(&f,static_cast<char*>(dd)+d.footprint.Offset+y*d.footprint.Footprint.RowPitch+x*4,4); return f; };
            auto color=[&](UINT x,UINT y) { UINT v=0; std::memcpy(&v,static_cast<char*>(cc)+c.footprint.Offset+y*c.footprint.Footprint.RowPitch+x*4,4); return v; };
            const UINT rgb=color(200,100);
            synthetic_ok=std::abs(value(200,100)-0.4f)<1e-6f && std::abs(value(20,100)-0.2f)<1e-6f &&
                std::abs(int(rgb&1023)-767)<=1 && std::abs(int((rgb>>10)&1023)-256)<=1 && std::abs(int((rgb>>20)&1023)-512)<=1;
        }
        save(L"-depth.raw",dd,d.bytes); save(L"-color.raw",cc,c.bytes);
        D3D12_RANGE no_write{0,0}; d.readback->Unmap(0,&no_write); c.readback->Unmap(0,&no_write);
        for(auto& cmd:copy_lists) cmd.Reset();
        copy_allocator.Reset();
        stage.store(Complete); return true;
    }
    void Log(FILE* file) {
        std::fprintf(file,"{\"phase\":\"pair_eligibility\",\"depth_transitions\":%u,\"color_transitions\":%u,\"matched_submissions\":%u,\"multiple_queues\":%s}\n",depth_transitions.load(),color_transitions.load(),local_pairs.load(),multiple_queues?"true":"false");
        std::fprintf(file,"{\"phase\":\"frame_pair\",\"written\":%s,\"stage\":%u,\"width\":%llu,\"height\":%u,\"color_format\":%u,\"depth_source_format\":%u,\"depth_footprint_format\":%u,\"depth_offset\":%llu,\"color_offset\":%llu,\"depth_pitch\":%u,\"color_pitch\":%u,\"depth_bytes\":%llu,\"color_bytes\":%llu,\"backbuffer_index\":%u,\"depth_state_restored\":%u,\"missed_observations\":%u,\"ledger_unsafe\":%s,\"copies_in_one_command_list\":true,\"capture_point\":\"end_of_graphics_submission\",\"recording_retired\":%s,\"executions\":%llu,\"synthetic_ok\":%s}\n",
            stage.load()==Complete?"true":"false",unsigned(stage.load()),depth_desc.Width,depth_desc.Height,color_desc.Format,depth_desc.Format,d.footprint.Footprint.Format,d.footprint.Offset,c.footprint.Offset,d.footprint.Footprint.RowPitch,c.footprint.Footprint.RowPitch,d.bytes,c.bytes,buffer_index,depth_state,missed.load(),ledger&&ledger->unsafe?"true":"false",retired.load()?"true":"false",signal_value.load(),synthetic_ok?"true":"false");
    }
};
}
