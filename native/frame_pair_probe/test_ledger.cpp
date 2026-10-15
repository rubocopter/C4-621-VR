#include "pair_ledger.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace frame_pair;
static void Require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
static D3D12_RESOURCE_BARRIER Transition(Id resource,UINT after,UINT sub=0,D3D12_RESOURCE_BARRIER_FLAGS flags=D3D12_RESOURCE_BARRIER_FLAG_NONE) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Flags=flags;
    b.Transition.pResource=reinterpret_cast<ID3D12Resource*>(resource); b.Transition.StateAfter=static_cast<D3D12_RESOURCE_STATES>(after); b.Transition.Subresource=sub; return b;
}
int main() try {
    Ledger a(1,{2,3,4});
    a.Record(10,Transition(1,D3D12_RESOURCE_STATE_DEPTH_READ)); a.Record(10,Transition(2,0));
    Require(!a.LocalReady(10,0),"A recording whose Reset was not observed must be ignored");
    a.Reset(10);
    a.Record(10,Transition(1,D3D12_RESOURCE_STATE_DEPTH_READ)); a.Record(10,Transition(2,0));
    Require(a.LocalReady(10,0),"Both explicit states in the same list can be copied inline");
    a.Reset(11); a.Record(11,Transition(1,D3D12_RESOURCE_STATE_DEPTH_WRITE)); // future frame recorded early
    Require(a.LocalReady(10,0),"One list containing both explicit states can capture inline");
    Require(!a.LocalReady(11,0),"A list without its own color state must not capture inline");
    Require(a.LocalDepth(10)==D3D12_RESOURCE_STATE_DEPTH_READ,"Another list's future recording must not overwrite local depth state");
    a.Reset(10); Require(!a.LocalReady(10,0),"Reset must discard the list's old transitions");
    a.Record(10,Transition(1,D3D12_RESOURCE_STATE_DEPTH_WRITE,1)); a.Record(10,Transition(3,0));
    Require(!a.LocalReady(10,1),"Stencil transition does not establish the depth plane state");
    a.Record(11,Transition(3,0)); Require(a.LocalReady(11,1),"Explicit depth and selected backbuffer in one list must match");
    a.Record(11,Transition(1,D3D12_RESOURCE_STATE_DEPTH_READ,0,D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY));
    Require(!a.LocalReady(11,1),"Pending split barrier must prevent copying");
    D3D12_RESOURCE_BARRIER alias{}; alias.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING; a.Record(11,alias);
    Require(!a.LocalReady(11,1),"Aliasing must invalidate that recording");
    a.Reset(11); a.Record(11,Transition(1,16)); a.Record(11,Transition(3,0));
    Require(a.LocalReady(11,1),"Reset discards the previous generation's aliasing");
    a.Forget(11); a.Record(11,Transition(3,0));
    Require(!a.LocalReady(11,1),"A new open object reusing a closed list's address must not inherit depth state");
    a.Reset(11); a.Record(11,Transition(1,192)); a.Record(11,Transition(3,0));
    Require(!a.Snapshot(11).trusted,"An open list must not supply an execution summary");
    a.Seal(11); const auto summary=a.Snapshot(11);
    Require(summary.trusted && summary.depth_known && summary.depth_state==192 && summary.color_present==2,"Closed generation summary must preserve final resource states");
    a.Seal(11); Require(!a.Snapshot(11).trusted,"Another successful Close without Reset must invalidate a reused address");
    Ledger parallel(1,{2,3,4});
    std::atomic<unsigned> failures{0}; std::vector<std::thread> workers;
    for(Id id=100;id<108;++id) workers.emplace_back([&,id] {
        for(unsigned n=0;n<1000;++n) {
            parallel.Reset(id); parallel.Record(id,Transition(1,192)); parallel.Record(id,Transition(2,0));
            if(!parallel.LocalReady(id,0) || parallel.LocalDepth(id)!=192) ++failures;
            parallel.Forget(id);
        }
    });
    for(auto& worker:workers) worker.join();
    Require(!failures.load() && !parallel.unsafe.load(),"Independent recording threads must not invalidate each other");
    std::cout<<"Pair list isolation, reset, plane, split-barrier and aliasing tests passed\n"; return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
