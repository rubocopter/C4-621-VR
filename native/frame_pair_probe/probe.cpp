// One bounded paired snapshot; no camera changes or waits on the game's thread.
#include "pair_capture.hpp"
#include "fixture.hpp"
#include <MinHook.h>
#include <d3d12sdklayers.h>
#include <array>
#include <cwchar>
#include <vector>
#include <filesystem>
using namespace frame_pair;
extern "C" size_t PairPresentSlot(void),PairExecuteSlot(void),PairBarrierSlot(void),PairResetSlot(void),PairCloseSlot(void),PairResizeSlot(void),PairResize1Slot(void);
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
// Retain on timeout/failure too: no COM teardown while game/GPU can use it.
static Capture* capture=new Capture;
static HMODULE module=nullptr;
static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* s,UINT sync,UINT flags) {
    try { capture->Present(s,flags); }
    catch(...) { capture->Fail(); }
    return original_present(s,sync,flags);
}
static void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* q,UINT count,ID3D12CommandList* const* lists) {
    capture->Execute(q,count,lists,original_execute);
}
static void STDMETHODCALLTYPE Barrier(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* items) {
    original_barrier(list,count,items);
    try { capture->Record(list,count,items,original_barrier); } catch(...) { capture->Fail(); }
}
static HRESULT STDMETHODCALLTYPE Reset(ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* state) {
    const HRESULT result=original_reset(list,allocator,state);
    if(SUCCEEDED(result)) capture->Reset(list);
    return result;
}
static HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* list) {
    const HRESULT result=original_close(list);
    if(SUCCEEDED(result)) capture->Close(list);
    return result;
}
static HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* swap,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags) {
    capture->Resize(); return original_resize(swap,count,width,height,format,flags);
}
static HRESULT STDMETHODCALLTYPE Resize1(IDXGISwapChain3* swap,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags,const UINT* masks,IUnknown* const* queues) {
    capture->Resize(); return original_resize1(swap,count,width,height,format,flags,masks,queues);
}
int RunPairProbe(bool self_test,bool resize_test=false) {
    if(!module) module=GetModuleHandleW(nullptr);
    wchar_t path[32768]; if(!GetModuleFileNameW(module,path,32768)) return 1;
    auto* name=std::wcsrchr(path,L'\\'); if(!name) return 1;
    const auto stem=self_test?std::wstring(L"frame_pair_selftest"):std::filesystem::path(path).stem().wstring().substr(4);
    swprintf_s(name+1,32768-(name+1-path),L"%s_%lu",stem.c_str(),GetCurrentProcessId());
    const std::wstring prefix=path;
    FILE* log=_wfsopen((prefix+L".jsonl").c_str(),L"wx",_SH_DENYNO); if(!log) return 1;
    std::array<void*,7> targets{}; std::array<std::array<unsigned char,16>,7> before{};
    bool activation_attempted=false;
    try {
        if(!self_test && (!GetModuleHandleW(L"armoredcore6.exe") || !GetModuleHandleW(L"ac6_coop.dll") ||
            GetModuleHandleW(L"EasyAntiCheat_EOS.dll") || GetModuleHandleW(L"EasyAntiCheat.dll"))) throw std::runtime_error("Host rejected");
        ComPtr<ID3D12Debug> debug;
        if(self_test && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        PairFixture fixture;
        ComPtr<ID3D12InfoQueue> validation;
        if(debug) Check(fixture.device.As(&validation),"Fixture debug queue");
        auto method=[](void* object,size_t slot) { return (*reinterpret_cast<void***>(object))[slot]; };
        ComPtr<IDXGISwapChain3> fixture_swap; Check(fixture.swapchain.As(&fixture_swap),"Fixture swapchain3");
        targets={method(fixture.swapchain.Get(),PairPresentSlot()),method(fixture.queue.Get(),PairExecuteSlot()),method(fixture.list.Get(),PairBarrierSlot()),method(fixture.list.Get(),PairResetSlot()),method(fixture.list.Get(),PairCloseSlot()),method(fixture.swapchain.Get(),PairResizeSlot()),method(fixture_swap.Get(),PairResize1Slot())};
        auto status=MH_Initialize(); if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status));
        const std::array<void*,7> detours={reinterpret_cast<void*>(&Present),reinterpret_cast<void*>(&Execute),reinterpret_cast<void*>(&Barrier),reinterpret_cast<void*>(&Reset),reinterpret_cast<void*>(&Close),reinterpret_cast<void*>(&Resize),reinterpret_cast<void*>(&Resize1)};
        const std::array<void**,7> originals={reinterpret_cast<void**>(&original_present),reinterpret_cast<void**>(&original_execute),reinterpret_cast<void**>(&original_barrier),reinterpret_cast<void**>(&original_reset),reinterpret_cast<void**>(&original_close),reinterpret_cast<void**>(&original_resize),reinterpret_cast<void**>(&original_resize1)};
        for(unsigned i=0;i<targets.size();++i) {
            std::memcpy(before[i].data(),targets[i],16); status=MH_CreateHook(targets[i],detours[i],originals[i]);
            if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status));
        }
        activation_attempted=true;
        status=MH_EnableHook(MH_ALL_HOOKS); if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status));
        std::fprintf(log,"{\"phase\":\"observing\",\"self_test\":%s,\"pid\":%lu,\"present_slot\":%zu,\"execute_slot\":%zu,\"barrier_slot\":%zu,\"reset_slot\":%zu}\n",self_test?"true":"false",GetCurrentProcessId(),PairPresentSlot(),PairExecuteSlot(),PairBarrierSlot(),PairResetSlot()); std::fflush(log);
        if(self_test && resize_test) {
            fixture.Frame(0); capture->Prepare();
            Check(fixture.swapchain->ResizeBuffers(2,640,360,DXGI_FORMAT_R10G10B10A2_UNORM,0),"Resize while armed");
            fixture.Frame(1);
            if(!capture->failed.load() || capture->recorded_list.load()) throw std::runtime_error("Resize did not cancel the prepared snapshot");
        } else if(self_test) {
            fixture.Frame(0); capture->Prepare(); fixture.Frame(1);
            if(capture->signal_value.load()!=1 || !capture->retired.load()) throw std::runtime_error("Private copy was not dispatched exactly once");
            fixture.Frame(2); capture->Save(prefix,true);
            Check(fixture.swapchain->ResizeBuffers(2,320,180,DXGI_FORMAT_R10G10B10A2_UNORM,0),"Resize after pair capture");
            if(validation) for(UINT64 i=0;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
                SIZE_T size=0; validation->GetMessage(i,nullptr,&size);
                std::vector<unsigned char> bytes(size); auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                Check(validation->GetMessage(i,message,&size),"Fixture debug message");
                if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
            }
            std::fprintf(log,"{\"phase\":\"fixture_validation\",\"debug_layer\":%s,\"third_frame_distinct\":true,\"resize_succeeded\":true,\"separate_submissions\":true,\"private_copy_once\":true}\n",validation?"true":"false");
        }
        else {
            const auto started=GetTickCount64();
            while(GetTickCount64()-started<8000 && !(GetAsyncKeyState(VK_F12)&0x8000)) {
                if(GetTickCount64()-started>=2000) { capture->Prepare(); capture->Save(prefix,false); }
                Sleep(10);
            }
        }
        status=MH_DisableHook(MH_ALL_HOOKS); if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status)); activation_attempted=false;
        bool restored=true; for(unsigned i=0;i<targets.size();++i) restored=restored && std::memcmp(before[i].data(),targets[i],16)==0;
        { std::lock_guard lock(capture->mutex); capture->Log(log); }
        const bool inactive=!capture->recorded_list.load() || capture->stage.load()==Capture::Complete;
        std::fprintf(log,"{\"phase\":\"%s\",\"hooks_restored\":%s,\"dll_retained_inactive\":%s}\n",inactive?"disabled":"hooks_disabled_pending_recording",restored?"true":"false",inactive?"true":"false"); std::fclose(log);
        if(self_test && resize_test) return restored && capture->failed.load() && !capture->recorded_list.load()?0:2;
        return restored && capture->stage.load()==Capture::Complete && (!self_test || capture->synthetic_ok)?0:2;
    } catch(const std::exception& error) {
        capture->Fail();
        if(activation_attempted) {
            const auto cleanup=MH_DisableHook(MH_ALL_HOOKS);
            std::fprintf(log,"{\"phase\":\"cleanup\",\"status\":\"%s\"}\n",MH_StatusToString(cleanup));
        }
        std::fprintf(log,"{\"phase\":\"failed\",\"message\":\"%s\"}\n",error.what()); std::fclose(log); return 1;
    }
}
#ifndef FRAME_PAIR_TEST_HOST
static DWORD WINAPI Worker(void*) { return RunPairProbe(false); }
BOOL APIENTRY DllMain(HMODULE current,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        module=current; HANDLE thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
        if(!thread) return FALSE; CloseHandle(thread);
    }
    return TRUE;
}
#endif
