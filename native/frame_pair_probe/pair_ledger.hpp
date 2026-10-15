#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <atomic>
#include <mutex>
namespace frame_pair {
using Id=std::uintptr_t;
class Ledger {
public:
    struct Summary { bool trusted=false,aliasing=false,depth_touched=false,depth_known=false; UINT depth_state=0,color_touched=0,color_present=0; };
    Ledger(Id depth,std::array<Id,3> colors):depth_(depth),colors_(colors) {}
    void Reset(Id list) {
        for(auto& e:lists_) if(e.list.load()==list) { std::lock_guard lock(e.mutex); if(e.list.load()!=list) continue; e.updates={}; e.aliasing=false; e.open=true; e.closed=false; return; }
        for(auto& e:lists_) { Id empty=0; if(e.list.compare_exchange_strong(empty,list)) { std::lock_guard lock(e.mutex); e.updates={}; e.aliasing=false; e.open=true; e.closed=false; return; } }
        unsafe.store(true);
    }
    void Forget(Id list) { for(auto& e:lists_) if(e.list.load()==list) { std::lock_guard lock(e.mutex); if(e.list.load()!=list) continue; e.updates={}; e.aliasing=false; e.open=e.closed=false; e.list.store(0); return; } }
    void Seal(Id list) {
        for(auto& e:lists_) if(e.list.load()==list) {
            std::lock_guard lock(e.mutex); if(e.list.load()!=list) continue;
            if(e.open) { e.open=false; e.closed=true; }
            else { e.closed=false; e.updates={}; } // new initially-open object at a reused address
            return;
        }
    }
    Summary Snapshot(Id list) const {
        for(const auto& e:lists_) if(e.list.load()==list) {
            std::lock_guard lock(e.mutex); if(e.list.load()!=list || !e.closed) return {};
            Summary s; s.trusted=true; s.aliasing=e.aliasing;
            s.depth_touched=e.updates[0].touched; s.depth_known=e.updates[0].known; s.depth_state=e.updates[0].value;
            for(unsigned i=0;i<3;++i) if(e.updates[i+1].touched) { s.color_touched|=1u<<i; if(e.updates[i+1].known && !e.updates[i+1].value) s.color_present|=1u<<i; }
            return s;
        }
        return {};
    }
    void Record(Id list,const D3D12_RESOURCE_BARRIER& barrier) {
        unsigned index=4;
        const bool alias=barrier.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        if(!alias) {
            if(barrier.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) return;
            const auto& t=barrier.Transition;
            if(t.Subresource!=0 && t.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) return;
            const auto resource=reinterpret_cast<Id>(t.pResource);
            if(resource==depth_) index=0;
            else for(unsigned i=0;i<3;++i) if(colors_[i] && resource==colors_[i]) index=i+1;
            if(index==4) return;
        }
        Entry* found=nullptr;
        for(auto& e:lists_) if(e.list.load()==list) { found=&e; break; }
        // Require a witnessed Reset, including when COM addresses are reused.
        if(!found) return;
        std::lock_guard lock(found->mutex);
        if(found->list.load()!=list || !found->open) return;
        if(alias) { found->aliasing=true; return; }
        const auto& t=barrier.Transition;
        auto& update=found->updates[index]; update.touched=true;
        update.known=barrier.Flags!=D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
        update.value=t.StateAfter;
    }
    bool LocalReady(Id list,unsigned backbuffer) const {
        if(backbuffer>=3 || unsafe.load()) return false;
        for(const auto& e:lists_) if(e.list.load()==list) {
            std::lock_guard lock(e.mutex); if(e.list.load()!=list || !e.open) return false;
            const auto& d=e.updates[0]; const auto& c=e.updates[backbuffer+1];
            return !e.aliasing && d.touched && d.known && c.touched && c.known && c.value==D3D12_RESOURCE_STATE_PRESENT;
        }
        return false;
    }
    UINT LocalDepth(Id list) const { for(const auto& e:lists_) if(e.list.load()==list) { std::lock_guard lock(e.mutex); if(e.list.load()==list) return e.updates[0].value; } return 0; }
    std::atomic<bool> unsafe{false};
private:
    Id depth_; std::array<Id,3> colors_;
    struct Update { bool touched=false,known=false; UINT value=0; };
    // D3D12 requires the caller to serialize recording methods on each list.
    // Independent lists use separate entries; only slot identity is shared.
    struct Entry { std::atomic<Id> list{0}; mutable std::mutex mutex; std::array<Update,4> updates{}; bool aliasing=false,open=false,closed=false; };
    std::array<Entry,2048> lists_{};
};
}
