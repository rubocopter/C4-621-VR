#include "pair_capture.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace frame_pair;
static void Require(bool v,const char* why) { if(!v) throw std::runtime_error(why); }
static std::atomic<unsigned> calls{0};
static void STDMETHODCALLTYPE Enqueue(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*) { ++calls; }
int main() try {
    Capture a;
    a.stage.store(Capture::Submitted); a.retired.store(true); a.signal_value.store(1);
    Require(a.ReadEligible(),"Retired, completed callback state should permit a fence check");
    a.Fail(); a.stage.store(Capture::Submitted); // stale publisher after failure
    Require(!a.ReadEligible(),"A stale Submitted store must not undo terminal failure");
    Capture b; b.stage.store(Capture::Submitted); b.signal_value.store(1);
    const bool stale_stage=b.stage.load()==Capture::Submitted;
    b.Fail(); b.retired.store(true); // failure between Save's old split checks
    Require(stale_stage && !b.ReadEligible(),"Map must recheck latched failure after retirement");
    Capture c; c.stage.store(Capture::Submitted); c.retired.store(true); c.signal_value.store(1); c.active_executes.store(1);
    Require(!c.ReadEligible(),"An entered callback must prevent Map until quiescent");
    Capture gate; gate.stage.store(Capture::Capturing);
    std::unique_lock lock(gate.mutex);
    std::thread other([&] { gate.Execute(nullptr,0,nullptr,Enqueue); });
    const auto deadline=GetTickCount64()+1000;
    while(!gate.active_executes.load() && !calls.load() && GetTickCount64()<deadline) Sleep(1);
    const bool blocked=gate.active_executes.load() && !calls.load();
    lock.unlock(); other.join();
    Require(blocked && calls.load()==1,"A concurrent submission must respect the enqueue gate during Capturing");
    std::cout<<"Terminal failure and readback eligibility tests passed\n"; return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
