#include "frame_trace.hpp"
#include <iostream>
#include <stdexcept>
using namespace frame_trace;
static void Require(bool v,const char* message) { if(!v) throw std::runtime_error(message); }
static D3D12_RESOURCE_BARRIER Barrier(frame_pair::Id resource,UINT state) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={reinterpret_cast<ID3D12Resource*>(resource),0,D3D12_RESOURCE_STATE_COMMON,static_cast<D3D12_RESOURCE_STATES>(state)};return b;
}
int main() try {
    frame_pair::Ledger ledger(1,{2,3,4});
    ledger.Reset(10);ledger.Record(10,Barrier(1,192));ledger.Seal(10);
    auto first=ledger.Snapshot(10);
    Require(first.recording!=0,"Reset must allocate a recording identity");
    ledger.Reset(11);ledger.Record(11,Barrier(1,192));ledger.Seal(11); // next frame recorded early
    static Trace trace; // bounded trace exceeds the Windows default thread stack
    trace.BeginSubmission(100,1000);trace.Observe(10,0,first);
    const auto depth=trace.depth;
    trace.BeginSubmission(100,1001);
    ledger.Reset(12);ledger.Record(12,Barrier(2,0));ledger.Seal(12);trace.Observe(12,0,ledger.Snapshot(12));
    trace.Publish(1,0,0,1,2,1002);
    auto p=trace.Find(1,0,1002,live_pair::Ready,1);
    Require(p&&p->depth.submit==1&&p->color.submit==2,"Pair must retain the depth submission, not assign copy submission to depth");
    Require(p->depth.recording==first.recording&&p->depth.list==10,"Future recording must not leak into executed provenance");
    Require(!trace.Find(0,0,1002,live_pair::Ready,1)&&!trace.Find(2,0,1002,live_pair::Ready,1),"Previous/next pair cannot resolve");
    Require(!trace.Find(1,1,1002,live_pair::Ready,1),"Wrong slot must fail");
    Require(trace.Find(1,0,1502,live_pair::Ready,1)&&!trace.Find(1,0,1503,live_pair::Ready,1),"Freshness boundary must remain 500ms");
    Require(!trace.Find(1,0,1001,live_pair::Ready,1),"Future publication must fail");
    Require(!trace.Find(1,0,1002,live_pair::Failed,1)&&!trace.Find(1,0,1002,live_pair::Preparing,1),"Failed/lost producer must invalidate lookup");
    Require(trace.Find(1,0,1002,live_pair::Stopped,1),"Clean stopped producer retains only fresh diagnostics, like stream");
    Require(!trace.Find(1,0,1002,live_pair::Ready,0),"Incomplete sequence publication must fail");
    ledger.Reset(10);ledger.Record(10,Barrier(1,192));ledger.Seal(10);
    Require(ledger.Snapshot(10).recording!=first.recording,"Recycled command list must get a new recording identity");
    trace.BeginSubmission(100,1003);trace.Observe(10,0,ledger.Snapshot(10));
    ledger.Reset(12);ledger.Record(12,Barrier(2,0));ledger.Seal(12);trace.Observe(12,1,ledger.Snapshot(12));
    trace.Publish(2,0,0,1,2,1004);
    Require(!trace.Find(1,0,1004,live_pair::Ready,2),"Recycled slot must reject prior metadata");
    Require(trace.Find(2,0,1004,live_pair::Ready,2),"Recycled slot must resolve the new immutable record");
    trace.BeginSubmission(100,1005);trace.Observe(99,0,{});
    Require(!trace.depth.valid,"Untrusted list must break depth provenance");
    trace.depth=depth;trace.RetireDepth();Require(!trace.depth.valid,"Backpressure drop must retire depth provenance");
    ledger.Forget(10);ledger.Reset(10);ledger.Seal(10);
    Require(ledger.Snapshot(10).recording!=first.recording,"COM address reuse must not resurrect an old identity");
    trace.BeginSubmission(101,1006);Require(!trace.depth.valid,"Different queue cannot inherit depth provenance");
    for(unsigned i=0;i<Trace::PairCapacity+2;++i) trace.Publish(i+3,0,0,1,2,1010+i);
    Require(trace.overflow>0,"Bounded trace overflow must be explicit");
    Require(!trace.Find(Trace::PairCapacity+4,0,1020,live_pair::Ready,Trace::PairCapacity+4),"Truncated trace must not synthesize a match");
    static Trace closed;
    ledger.Reset(20);ledger.Record(20,Barrier(1,192));ledger.Record(20,Barrier(2,0));ledger.Seal(20);
    closed.BeginSubmission(100,2000);closed.Observe(20,0,ledger.Snapshot(20));closed.Publish(1,0,0,1,2,2001);
    closed.BeginSubmission(100,2002);closed.Observe(20,0,ledger.Snapshot(20));closed.CloseOnBackpressure();
    const auto retained=closed.list_count;
    for(unsigned i=0;i<Trace::ListCapacity+10;++i) { closed.BeginSubmission(100,2003+i);closed.Observe(20,0,ledger.Snapshot(20)); }
    closed.CloseOnBackpressure();
    Require(closed.window_closed&&closed.closed_submit==2&&closed.list_count==retained&&!closed.overflow,
            "First backpressure must freeze the captured prefix without tail overflow or reopening");
    closed.Publish(2,1,0,1,2,3000);
    Require(closed.pair_count==1&&closed.omitted_pairs==1&&!closed.Find(2,1,3000,live_pair::Ready,2),
            "A resumed stream must report the omitted diagnostic pair and never synthesize metadata");
    std::cout<<"Submission/recording provenance and diagnostic publication rejection passed\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
