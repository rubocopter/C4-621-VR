// Five-second read-only observation of a fingerprinted projection helper.
// The detour calls the original and never changes its input or returned matrix.
#include "snapshot.hpp"
#include <MinHook.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <cwchar>
#include <share.h>
using BuildProjection=void*(__fastcall*)(void*,void*,unsigned char);
static BuildProjection original=nullptr;
static HMODULE module=nullptr;
static std::atomic<bool> observing{false};
static std::atomic<unsigned> calls{0},matched{0},count{0};
static std::atomic<ULONGLONG> next_tick{0};
struct Entry { ProjectionSnapshot sample; ULONGLONG tick=0; bool jitter_enabled=false; std::atomic<bool> ready{false}; };
static std::array<Entry,160> entries;
static void* __fastcall Observe(void* view,void* output,unsigned char jitter) {
    void* result=original(view,output,jitter);
    ++calls;
    if(!observing.load()) return result;
    ProjectionSnapshot sample;
    if(!ReadProjectionSnapshot(view,output,sample)) return result;
    if(std::abs(sample.fov-1.55334306f)>0.01f || std::abs(sample.near_plane-1.f)>0.01f ||
       std::abs(sample.far_plane-500000.f)>1.f) return result;
    ++matched;
    const auto now=GetTickCount64(); auto next=next_tick.load();
    if(now<next || !next_tick.compare_exchange_strong(next,now+25)) return result;
    const auto index=count.fetch_add(1);
    if(index<entries.size()) { auto& e=entries[index]; e.sample=sample; e.tick=now; e.jitter_enabled=jitter!=0; e.ready.store(true); }
    return result;
}
static DWORD WINAPI Worker(void*) {
    wchar_t path[32768]; if(!GetModuleFileNameW(module,path,32768)) return 1;
    auto* name=std::wcsrchr(path,L'\\'); if(!name) return 1;
    swprintf_s(name+1,32768-(name+1-path),L"projection_probe_%lu.jsonl",GetCurrentProcessId());
    FILE* log=_wfsopen(path,L"wx",_SH_DENYNO); if(!log) return 1;
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"armoredcore6.exe"));
    if(!base || !GetModuleHandleW(L"ac6_coop.dll") || GetModuleHandleW(L"EasyAntiCheat_EOS.dll") || GetModuleHandleW(L"EasyAntiCheat.dll")) {
        std::fprintf(log,"{\"phase\":\"host_rejected\"}\n"); std::fclose(log); return 1;
    }
    auto* target=reinterpret_cast<void*>(base+0x23a7790);
    const unsigned char expected[16]={0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18,0x55};
    if(std::memcmp(target,expected,16)) { std::fprintf(log,"{\"phase\":\"prologue_rejected\"}\n"); std::fclose(log); return 1; }
    MH_STATUS status=MH_Initialize();
    if(status==MH_OK) status=MH_CreateHook(target,reinterpret_cast<void*>(&Observe),reinterpret_cast<void**>(&original));
    if(status==MH_OK) {
        observing.store(true); status=MH_EnableHook(target);
        if(status==MH_OK) {
            std::fprintf(log,"{\"phase\":\"observing\",\"pid\":%lu,\"target_rva\":\"0x23a7790\",\"writes_camera_or_matrix\":false}\n",GetCurrentProcessId()); std::fflush(log);
            const auto start=GetTickCount64();
            while(GetTickCount64()-start<5000 && !(GetAsyncKeyState(VK_F12)&0x8000)) Sleep(10);
        }
    }
    observing.store(false);
    const auto disabled=MH_DisableHook(target);
    const bool restored=std::memcmp(target,expected,16)==0;
    unsigned saved=0;
    for(const auto& e:entries) if(e.ready.load()) {
        const auto& s=e.sample;
        std::fprintf(log,"{\"phase\":\"projection_sample\",\"tick\":%llu,\"view\":\"0x%llx\",\"fov_y\":%.9g,\"near_plane\":%.9g,\"far_plane\":%.9g,\"aspect\":%.9g,\"width\":%u,\"height\":%u,\"jitter_enabled\":%s,\"jitter_x\":%.9g,\"jitter_y\":%.9g,\"shift_x\":%.9g,\"shift_y\":%.9g,\"matrix\":[",e.tick,static_cast<unsigned long long>(s.view),s.fov,s.near_plane,s.far_plane,s.aspect,s.width,s.height,e.jitter_enabled?"true":"false",s.jitter_x,s.jitter_y,s.shift_x,s.shift_y);
        for(unsigned i=0;i<16;++i) std::fprintf(log,i?",%.9g":"%.9g",s.matrix[i]);
        std::fprintf(log,"]}\n"); ++saved;
    }
    std::fprintf(log,"{\"phase\":\"disabled\",\"status\":\"%s\",\"disable_status\":\"%s\",\"calls\":%u,\"matched\":%u,\"samples\":%u,\"hooks_restored\":%s,\"dll_retained_inactive\":true}\n",MH_StatusToString(status),MH_StatusToString(disabled),calls.load(),matched.load(),saved,restored?"true":"false");
    std::fclose(log);
    // Trampoline and storage remain resident. In-flight callbacks retain access.
    return status==MH_OK && disabled==MH_OK && restored && saved?0:2;
}
BOOL APIENTRY DllMain(HMODULE current,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) { module=current; HANDLE worker=CreateThread(nullptr,0,Worker,nullptr,0,nullptr); if(!worker) return FALSE; CloseHandle(worker); }
    return TRUE;
}
