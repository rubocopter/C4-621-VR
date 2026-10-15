// Bounded D3D12 API observer: calls originals unchanged, no texture copies.
// Disabled detours/trampolines remain mapped until the game process exits.
#include "dummy_device.hpp"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <share.h>
#ifdef AC6_DEPTH_CAPTURE
#include "depth_capture.hpp"
#endif

extern "C" std::size_t Ac6PresentSlot(void),Ac6ExecuteSlot(void),Ac6BarrierSlot(void);
using PresentFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using ExecuteFn=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
using BarrierFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
static PresentFn original_present=nullptr;
static ExecuteFn original_execute=nullptr;
static BarrierFn original_barrier=nullptr;
static HMODULE probe_module=nullptr;
static std::atomic<unsigned long long> presents{0},executes{0},barrier_calls{0},barriers{0},dropped{0};
static std::mutex inventory_mutex;
struct DepthRecord { ID3D12Resource* resource=nullptr; D3D12_RESOURCE_DESC desc{}; UINT before=0,after=0; std::uint64_t count=0; };
struct QueueRecord { ID3D12CommandQueue* queue=nullptr; D3D12_COMMAND_LIST_TYPE type{}; LUID luid{}; std::uint64_t count=0; };
static std::array<DepthRecord,64> depths;
static std::array<QueueRecord,16> queues;
static DXGI_SWAP_CHAIN_DESC observed_swap{};
#ifdef AC6_DEPTH_CAPTURE
static DepthCapture depth_capture;
bool DepthCaptureSucceeded() { return depth_capture.stage.load()==DepthCapture::Complete && depth_capture.synthetic_ok; }
static void PrepareSyntheticCapture(ID3D12Resource* resource) { depth_capture.Discover(resource); depth_capture.Prepare(); }
#endif

static HRESULT STDMETHODCALLTYPE ObservePresent(IDXGISwapChain* swap,UINT sync,UINT flags) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if(SUCCEEDED(swap->GetDesc(&desc))) {
        DWORD owner=0; GetWindowThreadProcessId(desc.OutputWindow,&owner);
        if(owner==GetCurrentProcessId()) {
            ++presents;
            std::unique_lock lock(inventory_mutex,std::try_to_lock);
            if(lock.owns_lock()) observed_swap=desc; else ++dropped;
        }
    }
    return original_present(swap,sync,flags);
}
static void STDMETHODCALLTYPE ObserveExecute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) {
    ++executes;
    {
        std::unique_lock lock(inventory_mutex,std::try_to_lock);
        if(lock.owns_lock()) {
            for(auto& entry:queues) if(!entry.queue || entry.queue==queue) {
                if(!entry.queue) {
                    entry.queue=queue; entry.type=queue->GetDesc().Type;
                    ComPtr<ID3D12Device> device;
                    if(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))) entry.luid=device->GetAdapterLuid();
                }
                ++entry.count; break;
            }
        } else ++dropped;
    }
    original_execute(queue,count,lists);
#ifdef AC6_DEPTH_CAPTURE
    depth_capture.AfterExecute(queue,count,lists);
#endif
}
static void STDMETHODCALLTYPE ObserveBarrier(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* items) {
    ++barrier_calls; barriers+=count;
    for(UINT i=0;i<count;++i) {
        const auto& b=items[i];
        if(b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) continue;
        const UINT states=b.Transition.StateBefore|b.Transition.StateAfter;
        if(!(states&(D3D12_RESOURCE_STATE_DEPTH_WRITE|D3D12_RESOURCE_STATE_DEPTH_READ))) continue;
#ifdef AC6_DEPTH_CAPTURE
        depth_capture.Discover(b.Transition.pResource);
#endif
        std::unique_lock lock(inventory_mutex,std::try_to_lock);
        if(!lock.owns_lock()) { ++dropped; continue; }
        for(auto& entry:depths) if(!entry.resource || entry.resource==b.Transition.pResource) {
            if(!entry.resource) { entry.resource=b.Transition.pResource; entry.desc=entry.resource->GetDesc(); }
            entry.before=b.Transition.StateBefore; entry.after=b.Transition.StateAfter; ++entry.count; break;
        }
    }
    original_barrier(list,count,items);
#ifdef AC6_DEPTH_CAPTURE
    // Unrelated transitions may be batched with the depth transition. Copy only
    // when no other item can change or alias this resource in the same batch.
    if(depth_capture.stage.load()==DepthCapture::Armed) for(UINT i=0;i<count;++i) {
        if(items[i].Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || items[i].Transition.pResource!=depth_capture.source.Get()) continue;
        bool unique=true;
        for(UINT j=0;j<count;++j) if(j!=i) {
            if(items[j].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && items[j].Transition.pResource==items[i].Transition.pResource) unique=false;
            if(items[j].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING) unique=false;
        }
        if(unique) depth_capture.AfterBarrier(list,items[i],original_barrier,count);
    }
#endif
}
static void Phase(FILE* file,const char* phase,const char* status="MH_OK") {
    std::fprintf(file,"{\"phase\":\"%s\",\"tick_ms\":%llu,\"status\":\"%s\",\"presents\":%llu,\"executes\":%llu,\"barrier_calls\":%llu,\"barriers\":%llu,\"dropped\":%llu}\n",
        phase,GetTickCount64(),status,presents.load(),executes.load(),barrier_calls.load(),barriers.load(),dropped.load());
    std::fflush(file);
}
int RunRenderProbe(bool self_test) {
    wchar_t path[32768];
    if(!GetModuleFileNameW(probe_module,path,32768)) return 1;
    wchar_t* name=std::wcsrchr(path,L'\\'); if(!name) return 1; ++name;
#ifdef AC6_DEPTH_BATCH_CAPTURE
    swprintf_s(name,32768-(name-path),self_test?L"depth_batch_capture_probe_selftest_%lu.jsonl":L"depth_batch_capture_probe_%lu.jsonl",GetCurrentProcessId());
    wchar_t raw_path[32768]; wcscpy_s(raw_path,path); wchar_t* raw_name=std::wcsrchr(raw_path,L'\\')+1;
    swprintf_s(raw_name,32768-(raw_name-raw_path),self_test?L"depth_batch_capture_selftest_%lu.raw":L"depth_batch_capture_%lu.raw",GetCurrentProcessId());
#elif defined(AC6_DEPTH_CAPTURE)
    swprintf_s(name,32768-(name-path),self_test?L"depth_capture_probe_selftest_%lu.jsonl":L"depth_capture_probe_%lu.jsonl",GetCurrentProcessId());
    wchar_t raw_path[32768]; wcscpy_s(raw_path,path); wchar_t* raw_name=std::wcsrchr(raw_path,L'\\')+1;
    swprintf_s(raw_name,32768-(raw_name-raw_path),self_test?L"depth_capture_selftest_%lu.raw":L"depth_capture_%lu.raw",GetCurrentProcessId());
#else
    swprintf_s(name,32768-(name-path),self_test?L"render_probe_selftest_%lu.jsonl":L"render_probe_%lu.jsonl",GetCurrentProcessId());
#endif
    FILE* file=_wfsopen(path,L"wx",_SH_DENYNO); if(!file) return 1;
    if(!self_test && (!GetModuleHandleW(L"armoredcore6.exe") || !GetModuleHandleW(L"ac6_coop.dll") ||
        GetModuleHandleW(L"EasyAntiCheat_EOS.dll") || GetModuleHandleW(L"EasyAntiCheat.dll"))) {
        Phase(file,"host_rejected"); std::fclose(file); return 1;
    }
    std::array<void*,3> targets{};
    std::array<std::array<unsigned char,16>,3> before{};
    bool enabled=false;
    try {
        DummyDevice dummy;
        auto method=[](void* object,std::size_t slot) { return (*reinterpret_cast<void***>(object))[slot]; };
        targets={method(dummy.swapchain.Get(),Ac6PresentSlot()),method(dummy.queue.Get(),Ac6ExecuteSlot()),method(dummy.list.Get(),Ac6BarrierSlot())};
        std::fprintf(file,"{\"phase\":\"metadata\",\"self_test\":%s,\"pid\":%lu,\"present_slot\":%zu,\"execute_slot\":%zu,\"barrier_slot\":%zu,\"graphics_commands_modified\":%s}\n",
            self_test?"true":"false",GetCurrentProcessId(),Ac6PresentSlot(),Ac6ExecuteSlot(),Ac6BarrierSlot(),
#ifdef AC6_DEPTH_CAPTURE
            "true"
#else
            "false"
#endif
        );
        auto status=MH_Initialize(); if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status));
        const std::array<void*,3> detours={reinterpret_cast<void*>(&ObservePresent),reinterpret_cast<void*>(&ObserveExecute),reinterpret_cast<void*>(&ObserveBarrier)};
        const std::array<void**,3> originals={reinterpret_cast<void**>(&original_present),reinterpret_cast<void**>(&original_execute),reinterpret_cast<void**>(&original_barrier)};
        for(unsigned i=0;i<3;++i) {
            std::memcpy(before[i].data(),targets[i],16);
            status=MH_CreateHook(targets[i],detours[i],originals[i]);
            if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status));
        }
        status=MH_EnableHook(MH_ALL_HOOKS); if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status)); enabled=true;
        Phase(file,"observing");
        if(self_test) {
#ifdef AC6_DEPTH_CAPTURE
            dummy.ExerciseDepth(PrepareSyntheticCapture);
            depth_capture.Save(raw_path,true);
#else
            dummy.ExerciseDepth();
#endif
        }
        else {
            const auto deadline=GetTickCount64()+8000;
            while(GetTickCount64()<deadline && !(GetAsyncKeyState(VK_F12)&0x8000)) {
#ifdef AC6_DEPTH_CAPTURE
                if(GetTickCount64()+6000<deadline) { Sleep(10); continue; }
                depth_capture.Prepare(); depth_capture.Save(raw_path,false);
#endif
                Sleep(10);
            }
        }
        status=MH_DisableHook(MH_ALL_HOOKS);
        if(status!=MH_OK) throw std::runtime_error(MH_StatusToString(status)); enabled=false;
        bool restored=true;
        for(unsigned i=0;i<3;++i) restored=restored && std::memcmp(before[i].data(),targets[i],16)==0;
        unsigned depth_count=0,queue_count=0;
        {
            std::lock_guard lock(inventory_mutex);
            for(const auto& entry:depths) if(entry.resource) {
                ++depth_count; const auto& d=entry.desc;
                std::fprintf(file,"{\"phase\":\"depth_resource\",\"resource\":\"0x%llx\",\"width\":%llu,\"height\":%u,\"format\":%u,\"array_size\":%u,\"samples\":%u,\"mips\":%u,\"flags\":%u,\"last_before\":%u,\"last_after\":%u,\"transitions\":%llu}\n",
                    reinterpret_cast<unsigned long long>(entry.resource),d.Width,d.Height,d.Format,d.DepthOrArraySize,d.SampleDesc.Count,d.MipLevels,d.Flags,entry.before,entry.after,entry.count);
            }
            for(const auto& entry:queues) if(entry.queue) {
                ++queue_count;
                std::fprintf(file,"{\"phase\":\"queue\",\"address\":\"0x%llx\",\"type\":%u,\"adapter_luid_low\":%lu,\"adapter_luid_high\":%ld,\"submissions\":%llu}\n",
                    reinterpret_cast<unsigned long long>(entry.queue),entry.type,entry.luid.LowPart,entry.luid.HighPart,entry.count);
            }
            std::fprintf(file,"{\"phase\":\"swapchain\",\"width\":%u,\"height\":%u,\"format\":%u,\"buffer_count\":%u,\"window\":\"0x%llx\"}\n",observed_swap.BufferDesc.Width,observed_swap.BufferDesc.Height,observed_swap.BufferDesc.Format,observed_swap.BufferCount,reinterpret_cast<unsigned long long>(observed_swap.OutputWindow));
        }
        std::fprintf(file,"{\"phase\":\"restoration\",\"hooks_restored\":%s,\"depth_resources\":%u,\"queues\":%u}\n",restored?"true":"false",depth_count,queue_count);
#ifdef AC6_DEPTH_CAPTURE
        depth_capture.Log(file);
#endif
        Phase(file,"disabled"); std::fclose(file);
        return restored && depth_count>0 && queue_count>0 && presents>0
#ifdef AC6_DEPTH_CAPTURE
            && depth_capture.stage.load()==DepthCapture::Complete && (!self_test || depth_capture.synthetic_ok)
#endif
            ?0:2;
    } catch(const std::exception& error) {
        if(enabled) MH_DisableHook(MH_ALL_HOOKS);
        Phase(file,"probe_failed",error.what()); std::fclose(file); return 1;
    }
}
#ifdef AC6_RENDER_TEST_HOST
void SetRenderProbeModule() { probe_module=GetModuleHandleW(nullptr); }
#else
static DWORD WINAPI Worker(void*) { return RunRenderProbe(false); }
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        probe_module=module; HANDLE thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
        if(!thread) return FALSE; CloseHandle(thread);
    }
    return TRUE;
}
#endif
