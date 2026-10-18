#include "stream_capture.hpp"
#include "json_diagnostic.hpp"
#include "../frame_pair_probe/fixture.hpp"
#include <MinHook.h>
#include <filesystem>
#include <cwchar>
#ifdef AC6_FRAME_TRACE
#include "projection_observer.hpp"
#ifdef AC6_RENDER_CONTEXT_TRACE
static auto* projection_observer=new frame_trace::ProjectionObserver(false,true);
static auto* render_context_observer=new frame_trace::RenderContextObserver(false);
#else
static auto* projection_observer=new frame_trace::ProjectionObserver(false);
#endif
#endif
extern "C" size_t PairPresentSlot(void),PairExecuteSlot(void),PairBarrierSlot(void),PairResetSlot(void),PairCloseSlot(void),PairResizeSlot(void),PairResize1Slot(void);
using namespace live_pair;
using namespace frame_pair;
using PresentFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using CloseFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using ResizeFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
using Resize1Fn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*);
static PresentFn original_present=nullptr;
static ExecuteFn original_execute=nullptr;
static BarrierFn original_barrier=nullptr;
static ResetFn original_reset=nullptr;
static CloseFn original_close=nullptr;
static ResizeFn original_resize=nullptr;
static Resize1Fn original_resize1=nullptr;
static auto* capture=new StreamCapture; // retained, including on failure/in-flight work
#ifdef AC6_FRAME_TRACE
static void VerifyTraceFixture() {
    const auto& t=capture->trace;
    if(t.pair_count!=3||t.overflow||!t.window_closed||t.omitted_pairs!=1)
        throw std::runtime_error("GPU fixture must retain three pairs and report the resumed capture as omitted");
    const auto& a=t.pairs[0];const auto& repeated=t.pairs[1];const auto& changed=t.pairs[2];
    for(unsigned i=0;i<t.pair_count;++i) {
        const auto& p=t.pairs[i];
        if(!p.depth.valid||!p.color.valid||p.sequence!=i+1||p.depth.submit>=p.copy_submit||p.color.submit!=p.copy_submit||
           p.depth_resource!=reinterpret_cast<frame_pair::Id>(capture->depth.Get())||p.color_resource!=capture->colors[p.backbuffer])
            throw std::runtime_error("Trace must identify separate executed depth/color sources of the GPU-verified pair");
    }
    if(a.depth.recording!=repeated.depth.recording||a.depth.list!=repeated.depth.list||a.depth.submit==repeated.depth.submit||
       a.depth.recording==changed.depth.recording) throw std::runtime_error("Repeated execution and a new recording must remain distinct");
}
#endif
static HMODULE module=nullptr;
static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* s,UINT sync,UINT flags) {
    try { capture->Present(s,flags); } catch(...) { capture->Fail(); }
    return original_present(s,sync,flags);
}
static void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* q,UINT n,ID3D12CommandList* const* lists) { capture->Execute(q,n,lists,original_execute); }
static void STDMETHODCALLTYPE Barrier(ID3D12GraphicsCommandList* c,UINT n,const D3D12_RESOURCE_BARRIER* b) {
    original_barrier(c,n,b); try { capture->Record(c,n,b,original_barrier); } catch(...) { capture->Fail(); }
}
static HRESULT STDMETHODCALLTYPE Reset(ID3D12GraphicsCommandList* c,ID3D12CommandAllocator* a,ID3D12PipelineState* p) { auto hr=original_reset(c,a,p); if(SUCCEEDED(hr)) capture->Reset(c); return hr; }
static HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* c) { auto hr=original_close(c); if(SUCCEEDED(hr)) capture->Close(c); return hr; }
static HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* s,UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags) { capture->Resize(); return original_resize(s,n,w,h,f,flags); }
static HRESULT STDMETHODCALLTYPE Resize1(IDXGISwapChain3* s,UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags,const UINT* masks,IUnknown* const* qs) { capture->Resize(); return original_resize1(s,n,w,h,f,flags,masks,qs); }
static UINT fake_calls=0,fake_count=0;
static ID3D12CommandList* fake_last=nullptr;
static void STDMETHODCALLTYPE ObserveBatch(ID3D12CommandQueue*,UINT n,ID3D12CommandList* const* lists) { ++fake_calls;fake_count=n;fake_last=lists[n-1]; }
static void GateFixture(PairFixture& fixture) {
    ComPtr<ID3D12Fence> gate;Check(fixture.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"Fixture queue gate");
    struct ReleaseGate { ID3D12Fence* fence;~ReleaseGate() { fence->Signal(1); } } release_gate{gate.Get()};
    Check(fixture.queue->Wait(gate.Get(),1),"Hold producer GPU timeline");
    auto submit=[&](bool fresh_depth,bool injected) {
        capture->Reset(fixture.list.Get());
        D3D12_RESOURCE_BARRIER b[2]{};for(auto& x:b) x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition={capture->depth.Get(),0,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        b[1].Transition={reinterpret_cast<ID3D12Resource*>(capture->colors[0]),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT};
        capture->Record(fixture.list.Get(),fresh_depth?2:1,fresh_depth?b:b+1,nullptr);capture->Close(fixture.list.Get());
        ID3D12CommandList* list=fixture.list.Get();const auto calls=fake_calls;capture->Execute(fixture.queue.Get(),1,&list,&ObserveBatch);
        if(fake_calls!=calls+1||fake_count!=(injected?2u:1u)) throw std::runtime_error("Private copy must be appended to one original Execute call");
        if(injected) { bool found=false;for(auto& slot:capture->slots) for(auto& copy:slot.copies) found=found||copy.Get()==fake_last;if(!found) throw std::runtime_error("Private batch tail was not a prepared copy"); }
    };
    submit(true,true);submit(true,true);submit(true,true);
    if(capture->frames!=3||capture->fence->GetCompletedValue()!=0) throw std::runtime_error("Producer gate did not keep readiness pending");
    Check(capture->released->Signal(3),"Advance release before producer completion");submit(true,false);
    if(capture->frames!=3) throw std::runtime_error("Slot reused before BOTH fences completed");
#ifdef AC6_FRAME_TRACE
    // Real v17 saturated its diagnostic list store AFTER the three captured
    // pairs. Reproduce the no-consumer tail without running a game.
    for(unsigned i=0;i<frame_trace::Trace::ListCapacity+1;++i) submit(true,false);
    if(capture->trace.overflow) throw std::runtime_error("Post-backpressure tail overflowed the captured trace window");
#endif
    Check(gate->Signal(1),"Open producer gate");
    const auto until=GetTickCount64()+5000;while(capture->fence->GetCompletedValue()<3&&GetTickCount64()<until) Sleep(1);
    if(capture->fence->GetCompletedValue()<3||capture->fence->GetCompletedValue()==UINT64_MAX) TerminateProcess(GetCurrentProcess(),3);
    submit(false,false); // Drop must retire the old depth transition too.
    submit(true,true);submit(false,false);
    if(capture->frames!=4) throw std::runtime_error("Color-only submission reused stale depth");
    capture->ledger->unsafe.store(true);submit(true,false);
    if(!capture->failed.load()||Read32(capture->shared->status)!=live_pair::Failed) throw std::runtime_error("Ledger exhaustion must publish terminal failure");
}
int RunLivePairProbe(bool self_test,bool gate_test=false) {
    if(!module) module=GetModuleHandleW(nullptr);
    wchar_t file[32768]; if(!GetModuleFileNameW(module,file,32768)) return 1;
    auto directory=std::filesystem::path(file).parent_path();
    auto stem=self_test?std::wstring(L"live_pair_selftest"):std::filesystem::path(file).stem().wstring().substr(4);
    auto path=directory/(stem+L"_"+std::to_wstring(GetCurrentProcessId())+L".jsonl");
    FILE* log=_wfsopen(path.c_str(),L"wx",_SH_DENYNO); if(!log) return 1;
    const auto name=L"Local\\AC6PairStream_"+stem+L"_"+std::to_wstring(GetCurrentProcessId());
    std::array<void*,7> targets{}; std::array<std::array<unsigned char,16>,7> before{};
    bool activation_attempted=false;
    bool ever_activation_attempted=false,hook_baselines_captured=false;
    try {
        if(!self_test&&(!GetModuleHandleW(L"armoredcore6.exe")||!GetModuleHandleW(L"ac6_coop.dll")||GetModuleHandleW(L"EasyAntiCheat_EOS.dll")||GetModuleHandleW(L"EasyAntiCheat.dll"))) throw std::runtime_error("Host rejected");
        PairFixture fixture;
        ComPtr<IDXGISwapChain3> swap; Check(fixture.swapchain.As(&swap),"Fixture swap3");
        auto method=[](void* p,size_t slot) { return (*reinterpret_cast<void***>(p))[slot]; };
        targets={method(swap.Get(),PairPresentSlot()),method(fixture.queue.Get(),PairExecuteSlot()),method(fixture.list.Get(),PairBarrierSlot()),method(fixture.list.Get(),PairResetSlot()),method(fixture.list.Get(),PairCloseSlot()),method(swap.Get(),PairResizeSlot()),method(swap.Get(),PairResize1Slot())};
        auto status=MH_Initialize(); if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status));
#ifdef AC6_FRAME_TRACE
        if(!self_test&&!projection_observer->CreateHook()) throw std::runtime_error("Projection helper fingerprint/hook rejected");
#ifdef AC6_RENDER_CONTEXT_TRACE
        if(!self_test&&!render_context_observer->CreateHook()) throw std::runtime_error("Render context fingerprint/hook rejected");
#endif
#endif
        const std::array<void*,7> detours={reinterpret_cast<void*>(&Present),reinterpret_cast<void*>(&Execute),reinterpret_cast<void*>(&Barrier),reinterpret_cast<void*>(&Reset),reinterpret_cast<void*>(&Close),reinterpret_cast<void*>(&Resize),reinterpret_cast<void*>(&Resize1)};
        const std::array<void**,7> originals={reinterpret_cast<void**>(&original_present),reinterpret_cast<void**>(&original_execute),reinterpret_cast<void**>(&original_barrier),reinterpret_cast<void**>(&original_reset),reinterpret_cast<void**>(&original_close),reinterpret_cast<void**>(&original_resize),reinterpret_cast<void**>(&original_resize1)};
        for(unsigned i=0;i<7;++i) std::memcpy(before[i].data(),targets[i],16);
        hook_baselines_captured=true;
        for(unsigned i=0;i<7;++i) { status=MH_CreateHook(targets[i],detours[i],originals[i]); if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status)); }
        activation_attempted=true; ever_activation_attempted=true; status=MH_EnableHook(MH_ALL_HOOKS); if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status));
        constexpr unsigned bounded_seconds=
#ifdef AC6_FRAME_TRACE
            8;
        std::fprintf(log,"{\"phase\":\"frame_trace_enabled\",\"game_base\":\"0x%llx\",\"projection_helper_rva\":\"0x23a7790\",\"projection_to_gpu_binding_observed\":false}\n",reinterpret_cast<UINT64>(GetModuleHandleW(L"armoredcore6.exe")));
#else
            60;
#endif
        std::fprintf(log,"{\"phase\":\"observing\",\"pid\":%lu,\"bounded_seconds\":%u}\n",GetCurrentProcessId(),bounded_seconds); std::fflush(log);
        if(self_test&&gate_test) {
            fixture.Frame(0);capture->PrepareStream(name);GateFixture(fixture);
            std::fprintf(log,"{\"phase\":\"gate_fixture\",\"pending_ready_prevents_reuse\":true,\"one_execute\":true,\"new_depth_required\":true,\"ledger_failure_published\":true}\n");
        } else if(self_test) {
            fixture.Frame(0); capture->PrepareStream(name); fixture.Frame(1); fixture.Frame(2); fixture.Frame(2);
            if(capture->frames!=3||!capture->dropped) throw std::runtime_error("Ring must fill then drop without consumer");
            const UINT second=767|(256<<10)|(512<<20);
            if(!capture->VerifyFixtureSlot(0,0.4f,second)||!capture->VerifyFixtureSlot(1,0.4f,second)) throw std::runtime_error("Repeated pair did not preserve frame1 depth/color");
            if(!capture->VerifyFixtureSlot(2,0.9f,205|(512<<10)|(921<<20))) throw std::runtime_error("Third slot did not preserve changed frame2 depth/color");
            Check(capture->released->Signal(3),"Fixture release"); fixture.Frame(2);
            if(capture->frames!=4) throw std::runtime_error("Released ring did not resume");
#ifdef AC6_FRAME_TRACE
            VerifyTraceFixture();
#endif
            Check(swap->ResizeBuffers(2,640,360,DXGI_FORMAT_R10G10B10A2_UNORM,0),"Resize must not be blocked by retained backbuffer refs");
            if(!capture->failed.load()) throw std::runtime_error("Resize did not stop capture");
            std::fprintf(log,"{\"phase\":\"fixture_validation\",\"pairs_match\":true,\"backpressure_drop\":true,\"release_resumes\":true,\"resize_cancels\":true}\n");
        } else {
            const auto started=GetTickCount64(); bool published=false;
            while(GetTickCount64()-started<bounded_seconds*1000 && !(GetAsyncKeyState(VK_F12)&0x8000) && !capture->failed.load()) {
                if(capture->shared&&Read32(capture->shared->stop_requested)) break;
                if(GetTickCount64()-started>=2000 && capture->stage.load()==Capture::Collecting) capture->PrepareStream(name);
#ifdef AC6_FRAME_TRACE
#ifdef AC6_RENDER_CONTEXT_TRACE
                if(capture->stage.load()==Capture::Armed) render_context_observer->BeginWindow();
#endif
                if(capture->stage.load()==Capture::Armed) projection_observer->BeginWindow();
#endif
                if(capture->shared&&!published) { published=true; std::fprintf(log,"{\"phase\":\"stream_ready\",\"mapping_suffix\":\"%ls_%lu\",\"width\":%u,\"height\":%u}\n",stem.c_str(),GetCurrentProcessId(),capture->shared->width,capture->shared->height); std::fflush(log); }
                Sleep(5);
            }
        }
        capture->StopStream();
#ifdef AC6_FRAME_TRACE
        projection_observer->Stop();
#ifdef AC6_RENDER_CONTEXT_TRACE
        render_context_observer->Stop();
#endif
#endif
        status=MH_DisableHook(MH_ALL_HOOKS); if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status)); activation_attempted=false;
        bool restored=true; for(unsigned i=0;i<7;++i) restored=restored&&std::memcmp(before[i].data(),targets[i],16)==0;
#ifdef AC6_FRAME_TRACE
        restored=restored&&(self_test||projection_observer->HookRestored());
#ifdef AC6_RENDER_CONTEXT_TRACE
        restored=restored&&(self_test||render_context_observer->HookRestored());
        if(!render_context_observer->Write(log)) throw std::runtime_error("Write render context diagnostics failed");
#endif
        capture->WriteFrameTrace(log);
        if(!projection_observer->Write(log)) throw std::runtime_error("Write projection diagnostics failed");
#endif
        for(UINT i=0;i<capture->queue_observation_count;++i) {
            const auto& q=capture->queue_observations[i];
            if(std::fprintf(log,"{\"phase\":\"queue_observation\",\"queue\":\"0x%llx\",\"device\":\"0x%llx\",\"type\":%u,\"same_device\":%s,\"submissions\":%u,\"lists\":%u,\"untrusted\":%u,\"depth_lists\":%u,\"color_lists\":%u,\"aliasing\":%u}\n",static_cast<unsigned long long>(q.identity),static_cast<unsigned long long>(q.device_identity),unsigned(q.type),q.same_device?"true":"false",q.submissions,q.lists,q.untrusted,q.depth_lists,q.color_lists,q.aliasing)<0) throw std::runtime_error("Write queue diagnostic failed");
            for(USHORT j=0;j<q.stack_count;++j) {
                HMODULE owner=nullptr;wchar_t owner_path[32768]{};
                if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(q.stack[j]),&owner)) GetModuleFileNameW(owner,owner_path,32768);
                const auto owner_name=JsonModuleName(std::filesystem::path(owner_path).filename().wstring());
                if(std::fprintf(log,"{\"phase\":\"queue_stack\",\"queue\":\"0x%llx\",\"index\":%u,\"module\":%s,\"offset\":\"0x%llx\"}\n",static_cast<unsigned long long>(q.identity),unsigned(j),owner_name.c_str(),static_cast<unsigned long long>(reinterpret_cast<frame_pair::Id>(q.stack[j])-reinterpret_cast<frame_pair::Id>(owner)))<0) throw std::runtime_error("Write queue stack diagnostic failed");
            }
        }
        if(std::fflush(log)!=0) throw std::runtime_error("Flush queue diagnostics failed");
        std::fprintf(log,"{\"phase\":\"stream_summary\",\"frames\":%llu,\"dropped\":%llu,\"multiple_queues\":%s,\"ledger_unsafe\":%s,\"failed\":%s,\"cpu_pixel_readback\":false}\n",capture->frames,capture->dropped,capture->multiple_queues?"true":"false",capture->ledger&&capture->ledger->unsafe?"true":"false",capture->failed.load()?"true":"false");
        std::fprintf(log,"{\"phase\":\"disabled\",\"hooks_restored\":%s,\"dll_retained_inactive\":true}\n",restored?"true":"false"); std::fclose(log);
        return restored&&capture->frames>0&&(self_test||!capture->failed.load())?0:2;
    } catch(const std::exception& error) {
        capture->Fail();
#ifdef AC6_FRAME_TRACE
        projection_observer->Stop();
#ifdef AC6_RENDER_CONTEXT_TRACE
        render_context_observer->Stop();
#endif
#endif
        if(activation_attempted) { const auto cleanup=MH_DisableHook(MH_ALL_HOOKS);std::fprintf(log,"{\"phase\":\"cleanup\",\"status\":\"%s\"}\n",MH_StatusToString(cleanup)); }
        bool restored=true;for(unsigned i=0;i<7;++i) if(targets[i]) restored=restored&&std::memcmp(before[i].data(),targets[i],16)==0;
#ifdef AC6_FRAME_TRACE
        restored=restored&&(self_test||projection_observer->HookRestored());
#ifdef AC6_RENDER_CONTEXT_TRACE
        restored=restored&&(self_test||render_context_observer->HookRestored());
#endif
#endif
        std::fprintf(log,"{\"phase\":\"failed\",\"message\":\"%s\",\"hook_activation_attempted\":%s,\"hook_baselines_captured\":%s}\n",error.what(),ever_activation_attempted?"true":"false",hook_baselines_captured?"true":"false");
        std::fprintf(log,"{\"phase\":\"disabled\",\"hooks_restored\":%s,\"dll_retained_inactive\":true}\n",restored?"true":"false");std::fclose(log);return 1;
    }
}
#ifndef LIVE_PAIR_TEST_HOST
static DWORD WINAPI Worker(void*) { return RunLivePairProbe(false); }
BOOL APIENTRY DllMain(HMODULE current,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) { module=current; HANDLE t=CreateThread(nullptr,0,Worker,nullptr,0,nullptr); if(!t) return FALSE; CloseHandle(t); }
    return TRUE;
}
#endif
