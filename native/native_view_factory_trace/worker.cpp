// Observation only. All hook management, module gates and JSONL I/O run here.
#include "trace.hpp"
#include "report.hpp"
#include <MinHook.h>
#include <tlhelp32.h>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <share.h>
using namespace factory_trace;
static HMODULE module=nullptr;
static bool modules_allowed() noexcept {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
    if(snapshot==INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{};entry.dwSize=sizeof entry;bool coop=false,ok=Module32FirstW(snapshot,&entry)!=FALSE;
    if(ok) do {
        for(auto* p=entry.szModule;*p;++p) *p=static_cast<wchar_t>(std::towlower(*p));
        if(std::wcsstr(entry.szModule,L"easyanticheat")||!std::wcsncmp(entry.szModule,L"eac_",4)) {ok=false;break;}
        if(!std::wcscmp(entry.szModule,L"ac6_coop.dll")) coop=true;
    } while(Module32NextW(snapshot,&entry));
    if(ok&&GetLastError()!=ERROR_NO_MORE_FILES) ok=false;
    CloseHandle(snapshot);return ok&&coop;
}
static bool host_allowed(U base) noexcept {
    if(!base||reinterpret_cast<U>(GetModuleHandleW(nullptr))!=base||
        GetModuleHandleW(L"ac6_render_probe.dll")!=module||!modules_allowed()) return false;
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!read(base,&dos,sizeof dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||dos.e_lfanew>0x100000||
        base>UINTPTR_MAX-0x6c2fc00||!read(base+dos.e_lfanew,&nt,sizeof nt)) return false;
    return nt.Signature==IMAGE_NT_SIGNATURE&&nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64&&
        nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC&&nt.FileHeader.TimeDateStamp==0x6822405f&&
        nt.OptionalHeader.SizeOfImage==0x6c2fc00;
}
static bool prologue(U target) noexcept {
    unsigned char actual[16]{};
    return read(target,actual,sizeof actual)&&!std::memcmp(actual,build_prologue.data(),sizeof actual);
}
static DWORD WINAPI worker(void*) {
    const ULONGLONG start=GetTickCount64();
    const U base=reinterpret_cast<U>(GetModuleHandleW(L"armoredcore6.exe"));
    Report report;report.image_base=base;report.pid=GetCurrentProcessId();
    FILETIME creation{},exit{},kernel{},user{};
    const bool creation_ok=GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user)!=FALSE;
    report.process_creation=(static_cast<U>(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
    void* target=nullptr;bool created=false;MH_STATUS status=MH_UNKNOWN;
    if(creation_ok&&host_allowed(base)) {
        target=reinterpret_cast<void*>(base+factory_rva);report.status="prologue_rejected";
        if(prologue(reinterpret_cast<U>(target))) {
            status=MH_Initialize();
            if(status==MH_OK) {
                status=MH_CreateHook(target,reinterpret_cast<void*>(&factory_hook),reinterpret_cast<void**>(&original));
                created=status==MH_OK;
            }
            if(status==MH_OK) status=MH_QueueEnableHook(target);
            if(status==MH_OK) {
                if(!open()) report.status="admission_rejected";
                else {status=MH_ApplyQueued();report.status=MH_StatusToString(status);}
            } else report.status=MH_StatusToString(status);
        }
    }
    if(status==MH_OK) {
        while(window.load(std::memory_order_acquire)!=Window::complete&&
            window.load(std::memory_order_acquire)!=Window::closed&&
            GetTickCount64()-start<8000&&!(GetAsyncKeyState(VK_F12)&0x8000)) Sleep(1);
    }
    close_window();
    MH_STATUS disable=MH_OK;
    if(created) {
        disable=MH_QueueDisableHook(target);
        const auto applied=MH_ApplyQueued();if(disable==MH_OK) disable=applied;
        if(disable!=MH_OK) MH_DisableHook(target); // Best effort; preserve failure.
    }
    report.disable_status=MH_StatusToString(disable);
    report.restored=target&&prologue(reinterpret_cast<U>(target));
    const auto drain_start=GetTickCount64();
    while(active.load(std::memory_order_acquire)&&GetTickCount64()-drain_start<1000) Sleep(1);
    report.pending_at_shutdown=active.load(std::memory_order_acquire);
    report.closed_tick_ms=GetTickCount64();
    // Private, ignored JSONL only, after cutoff/disable/restore/drain. No event
    // payload is traversed if pending callbacks outlive the bounded drain.
    wchar_t path[32768]{};const DWORD n=GetModuleFileNameW(module,path,32768);
    if(!n||n>=32768) return 2;
    auto* name=std::wcsrchr(path,L'\\');if(!name) return 2;
    if(swprintf_s(name+1,32768-(name+1-path),L"render_probe_%lu.jsonl",report.pid)<0) return 2;
    FILE* log=_wfsopen(path,L"wx",_SH_DENYNO);if(!log) return 2;
    const bool written=write_report(log,report);const bool closed=std::fclose(log)==0;
    // Retain DLL, hook records and trampolines: no RemoveHook, Uninitialize,
    // FreeLibrary, factory retry, camera edits or GPU calls.
    return written&&closed&&!std::strcmp(report.status,"MH_OK")&&disable==MH_OK&&
        report.restored&&!report.pending_at_shutdown?0:2;
}
BOOL APIENTRY DllMain(HMODULE current,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        module=current;HANDLE thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);
        if(!thread) return FALSE;CloseHandle(thread);
    }
    return TRUE;
}
