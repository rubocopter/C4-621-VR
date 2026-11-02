// Worker only: no file I/O, allocation, or MinHook control in native callbacks.
#include "trace.hpp"
#include "report.hpp"
#include <MinHook.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <share.h>
using namespace view_trace;
static HMODULE module=nullptr;

static bool host_allowed(U base) noexcept {
    if(!base||reinterpret_cast<U>(GetModuleHandleW(nullptr))!=base||
        !GetModuleHandleW(L"ac6_coop.dll")||GetModuleHandleW(L"EasyAntiCheat_EOS.dll")||
        GetModuleHandleW(L"EasyAntiCheat.dll")||GetModuleHandleW(L"EasyAntiCheat_x64.dll")||
        GetModuleHandleW(L"ac6_render_probe.dll")!=module) return false;
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!read(base,&dos,sizeof dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||dos.e_lfanew>0x100000||
        base>UINTPTR_MAX-0x6c2fc00||!read(base+dos.e_lfanew,&nt,sizeof nt)) return false;
    return nt.Signature==IMAGE_NT_SIGNATURE&&nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64&&
        nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC&&nt.FileHeader.TimeDateStamp==0x6822405f&&
        nt.OptionalHeader.SizeOfImage==0x6c2fc00;
}
static bool prologue(U target,const std::array<unsigned char,16>& expected) noexcept {
    unsigned char actual[16]{};return read(target,actual,sizeof actual)&&!std::memcmp(actual,expected.data(),sizeof actual);
}
static bool identity_allowed(U base,Identity& i) noexcept {
    U vtable=0;
    return read_identity(base+manager_rva,i)&&read(i.main_view,&vtable,sizeof vtable)&&vtable==base+0x3e3e7e0;
}
static DWORD WINAPI worker(void*) {
    const ULONGLONG start=GetTickCount64();
    const U base=reinterpret_cast<U>(GetModuleHandleW(L"armoredcore6.exe"));
    Report report;report.image_base=base;report.pid=GetCurrentProcessId();
    report.status="host_rejected";report.disable_status="MH_OK";
    void* targets[]{base?reinterpret_cast<void*>(base+render_all_rva):nullptr,base?reinterpret_cast<void*>(base+view_render_rva):nullptr};
    void* detours[]{reinterpret_cast<void*>(&render_all_hook),reinterpret_cast<void*>(&view_render_hook)};
    void** originals[]{reinterpret_cast<void**>(&render_all_original),reinterpret_cast<void**>(&view_render_original)};
    unsigned created=0;
    MH_STATUS status=MH_UNKNOWN;
    if(host_allowed(base)) {
        report.status="identity_rejected";
        if(identity_allowed(base,report.identity)) {
            report.status="prologue_rejected";
            if(prologue(reinterpret_cast<U>(targets[0]),build_signatures[0])&&prologue(reinterpret_cast<U>(targets[1]),build_signatures[1])) {
                status=MH_Initialize();
                for(unsigned i=0;i<2&&status==MH_OK;++i) {
                    status=MH_CreateHook(targets[i],detours[i],originals[i]);if(status==MH_OK) ++created;
                }
                for(unsigned i=0;i<created&&status==MH_OK;++i) status=MH_QueueEnableHook(targets[i]);
                // Confirm the sampled root still names the same identities just
                // before opening. Equal samples cannot rule out ABA/lifetime races.
                Identity again{};
                if(status==MH_OK) {
                    if(!identity_allowed(base,again)||again.manager!=report.identity.manager||again.list!=report.identity.list||
                        again.view_manager!=report.identity.view_manager||again.main_view!=report.identity.main_view)
                        report.status="identity_changed";
                    else if(!open(report.identity.list)) report.status="admission_rejected";
                    else {status=MH_ApplyQueued();report.status=MH_StatusToString(status);}
                } else report.status=MH_StatusToString(status);
            }
        }
    }
    // Reserve one second of the eight-second admission/drain budget for cleanup.
    // Synchronous hook management and final file I/O are not hard-real-time.
    if(status==MH_OK&&observing.load()) {
        while(observing.load(std::memory_order_acquire)&&GetTickCount64()-start<7000&&!(GetAsyncKeyState(VK_F12)&0x8000)) Sleep(5);
    }
    close_window();
    MH_STATUS disable=MH_OK;
    for(unsigned i=0;i<created;++i) {
        const auto s=MH_QueueDisableHook(targets[i]);if(s!=MH_OK&&disable==MH_OK) disable=s;
    }
    if(created) {
        const auto s=MH_ApplyQueued();if(s!=MH_OK&&disable==MH_OK) disable=s;
        if(disable!=MH_OK) {
            // Best effort only; preserve the original failure in the summary.
            for(unsigned i=0;i<created;++i) MH_DisableHook(targets[i]);
        }
    }
    report.disable_status=MH_StatusToString(disable);
    while(active.load(std::memory_order_acquire)&&GetTickCount64()-start<8000) Sleep(1);
    report.pending_at_shutdown=active.load(std::memory_order_acquire);
    for(unsigned i=0;i<2;++i) report.restored[i]=targets[i]&&prologue(reinterpret_cast<U>(targets[i]),build_signatures[i]);
    report.allow_events=disable==MH_OK&&report.restored[0]&&report.restored[1]&&!report.pending_at_shutdown;

    // No log exists before admission closes and hook-disable/drain is attempted.
    // On failed drain/restoration write metadata/summary only; storage stays mapped.
    wchar_t path[32768]{};const DWORD n=GetModuleFileNameW(module,path,32768);
    if(!n||n>=32768) return 2;
    auto* name=std::wcsrchr(path,L'\\');if(!name) return 2;
    if(swprintf_s(name+1,32768-(name+1-path),L"render_probe_%lu.jsonl",report.pid)<0) return 2;
    FILE* log=_wfsopen(path,L"wx",_SH_DENYNO);if(!log) return 2;
    const bool written=write_report(log,report);const bool closed=std::fclose(log)==0;
    // Never remove hooks/trampolines, uninitialize MinHook, or unload this DLL.
    return written&&closed&&!std::strcmp(report.status,"MH_OK")&&report.allow_events?0:2;
}
BOOL APIENTRY DllMain(HMODULE current,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        module=current;HANDLE thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);
        if(!thread) return FALSE;CloseHandle(thread);
    }
    return TRUE;
}
