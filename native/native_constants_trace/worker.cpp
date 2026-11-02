// Bounded observation only. No GPU work, view edits, or VR presentation.
#include "trace.hpp"
#include <MinHook.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <share.h>
#include <algorithm>
using namespace trace;
static HMODULE module=nullptr;
static const U rvas[]={0x2462320,0x2906be0,0x2378fd0,0x28eda40,0x28f8a50};
static constexpr unsigned hooks=5;
static const unsigned char expected[hooks][16]={
 {0x48,0x89,0x74,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x49,0x8b,0xf1,0x49,0x8b,0xf8},
 {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x59,0x20,0x48,0x8b},
 {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20,0x33},
 {0x44,0x0f,0xb6,0x49,0x78,0x41,0x83,0xe9,0x01,0x74,0x13,0x41,0x83,0xf9,0x01,0x75},
 {0x40,0x53,0x55,0x41,0x55,0x41,0x56,0x48,0x83,0xec,0x78,0x48,0x8b,0x05,0xee,0x74}
};
static void hex(FILE* log,const unsigned char* bytes,U size) {
    for(U i=0;i<size;++i) std::fprintf(log,"%02x",bytes[i]);
}
static DWORD WINAPI worker(void*) {
    wchar_t path[32768]; if(!GetModuleFileNameW(module,path,32768)) return 1;
    auto* name=std::wcsrchr(path,L'\\'); if(!name) return 1;
    swprintf_s(name+1,32768-(name+1-path),L"render_probe_%lu.jsonl",GetCurrentProcessId());
    FILE* log=_wfsopen(path,L"wx",_SH_DENYNO); if(!log) return 1;
    auto base=reinterpret_cast<U>(GetModuleHandleW(L"armoredcore6.exe"));
    if(!base || !GetModuleHandleW(L"ac6_coop.dll") || GetModuleHandleW(L"EasyAntiCheat_EOS.dll") || GetModuleHandleW(L"EasyAntiCheat.dll")) {
        std::fputs("{\"phase\":\"host_rejected\"}\n",log); std::fclose(log); return 1;
    }
    void* targets[hooks]{}; void* detours[]={reinterpret_cast<void*>(&upload_hook),reinterpret_cast<void*>(&resolve_hook),reinterpret_cast<void*>(&draw_hook),reinterpret_cast<void*>(&table_hook),reinterpret_cast<void*>(&descriptors_hook)};
    void** originals[]={reinterpret_cast<void**>(&upload_original),reinterpret_cast<void**>(&resolve_original),reinterpret_cast<void**>(&draw_original),reinterpret_cast<void**>(&table_original),reinterpret_cast<void**>(&descriptors_original)};
    for(unsigned i=0;i<hooks;++i) {
        targets[i]=reinterpret_cast<void*>(base+rvas[i]); unsigned char bytes[16];
        if(!read(base+rvas[i],bytes,16) || std::memcmp(bytes,expected[i],16)) {
            std::fprintf(log,"{\"phase\":\"prologue_rejected\",\"rva\":%llu}\n",rvas[i]); std::fclose(log); return 1;
        }
    }
    MH_STATUS status=MH_Initialize(); unsigned created=0;
    for(unsigned i=0;i<hooks && status==MH_OK;++i) { status=MH_CreateHook(targets[i],detours[i],originals[i]); if(status==MH_OK) ++created; }
    for(unsigned i=0;i<hooks && status==MH_OK;++i) status=MH_QueueEnableHook(targets[i]);
    if(status==MH_OK) { stop_after_draw=true; observing.store(true,std::memory_order_release); status=MH_ApplyQueued(); }
    if(status==MH_OK) {
        std::fprintf(log,"{\"phase\":\"observing\",\"schema\":\"native_constants_trace_v1\",\"pid\":%lu,\"image_base\":%llu,\"duration_ms\":8000,\"upload_limit\":%u,\"event_limit\":%u,\"writes_matrices\":false,\"gpu_commands_added\":false,\"frame_associated\":false}\n",GetCurrentProcessId(),base,upload_limit,event_limit); std::fflush(log);
        auto start=GetTickCount64(); while(observing.load() && GetTickCount64()-start<8000 && !(GetAsyncKeyState(VK_F12)&0x8000)) Sleep(10);
    }
    close_window();
    MH_STATUS disable=MH_OK;
    for(unsigned i=0;i<created;++i) { auto s=MH_QueueDisableHook(targets[i]); if(s!=MH_OK) disable=s; }
    if(created) { auto s=MH_ApplyQueued(); if(s!=MH_OK) disable=s; }
    auto wait_start=GetTickCount64(); while(active.load() && GetTickCount64()-wait_start<1000) Sleep(1);
    const auto pending=active.load();
    bool restored=true;
    for(unsigned i=0;i<hooks;++i) { unsigned char bytes[16]; bool same=read(base+rvas[i],bytes,16) && !std::memcmp(bytes,expected[i],16); restored &=same;
        std::fprintf(log,"{\"phase\":\"restoration\",\"rva\":%llu,\"restored\":%s}\n",rvas[i],same?"true":"false"); }
    unsigned saved_uploads=0,saved_events=0;
    for(const auto& u:uploads) if(u.ready.load(std::memory_order_acquire)) {
        ++saved_uploads;
        std::fprintf(log,"{\"phase\":\"upload\",\"begin\":%llu,\"end\":%llu,\"thread\":%u,\"scope\":%llu,\"caller\":%llu,\"ref\":%llu,\"buffer\":%llu,\"context\":%llu,\"source\":%llu,\"size\":%llu,\"destination\":%llu,\"capacity\":%u,\"flags\":%u,\"valid\":%u,\"before\":\"",u.begin,u.end,u.thread,u.scope,u.caller,u.ref,u.buffer,u.context,u.source,u.size,u.destination,u.capacity,u.flags,u.valid);
        if(u.valid&1) hex(log,u.before,u.size); std::fputs("\",\"after\":\"",log); if(u.valid&2) hex(log,u.after,u.size); std::fputs("\"}\n",log);
    }
    for(const auto& e:events) if(e.ready.load(std::memory_order_acquire)) {
        ++saved_events;
        std::fprintf(log,"{\"phase\":\"event\",\"kind\":%u,\"begin\":%llu,\"end\":%llu,\"thread\":%u,\"scope\":%llu,\"caller\":%llu,\"valid\":%u,\"a\":[",e.kind,e.begin,e.end,e.thread,e.scope,e.caller,e.valid);
        for(unsigned i=0;i<16;++i) std::fprintf(log,i?",%llu":"%llu",e.a[i]); std::fputs("]}\n",log);
    }
    std::fprintf(log,"{\"phase\":\"disabled\",\"status\":\"%s\",\"disable_status\":\"%s\",\"hooks_restored\":%s,\"dll_retained_inactive\":true,\"active_callbacks_at_flush\":%u,\"upload_calls\":%llu,\"resolve_calls\":%llu,\"draw_calls\":%llu,\"table_calls\":%llu,\"upload_attempts\":%u,\"event_attempts\":%u,\"saved_uploads\":%u,\"saved_events\":%u,\"upload_overflow\":%u,\"event_overflow\":%u,\"frame_associated\":false}\n",MH_StatusToString(status),MH_StatusToString(disable),restored?"true":"false",active.load(),upload_calls.load(),resolve_calls.load(),draw_calls.load(),table_calls.load(),upload_count.load(),event_count.load(),saved_uploads,saved_events,upload_count.load()>upload_limit?upload_count.load()-upload_limit:0,event_count.load()>event_limit?event_count.load()-event_limit:0);
    std::fprintf(log,"{\"phase\":\"descriptor_totals\",\"calls\":%llu,\"pending_at_shutdown\":%u,\"first_loss_sequence\":%llu,\"recording_cutoff\":%llu,\"stop_after_draw\":true}\n",descriptor_calls.load(),pending,first_loss_sequence.load(),recording_cutoff.load());
    std::fclose(log);
    // Deliberately retain DLL, trampoline and all callback storage, including on failure.
    return status==MH_OK && disable==MH_OK && restored && pending==0?0:2;
}
BOOL APIENTRY DllMain(HMODULE current,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) { module=current; HANDLE t=CreateThread(nullptr,0,worker,nullptr,0,nullptr); if(!t) return FALSE; CloseHandle(t); }
    return TRUE;
}
