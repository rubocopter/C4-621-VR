#include "trace.hpp"
#include "report.hpp"
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace factory_trace;
static void Check(bool ok,const char* why) {if(!ok) throw std::runtime_error(why);}
static constexpr U factory_id=0x123456789abcdef0ull,opaque_result=0xfedcba9876543210ull;
static constexpr DWORD incoming=0x1357,outgoing=0x2468;
static unsigned calls=0;
static U desc_arg=0,aux_arg=0,result_arg=opaque_result;
static unsigned char descriptor[0x70],auxiliary[0x18],view[0x140],targets[0xa0];
static void Word(unsigned char* p,unsigned offset,U value) {std::memcpy(p+offset,&value,8);}
static void Inputs() {
    std::memset(descriptor,0xab,sizeof descriptor);std::memset(auxiliary,0xcd,sizeof auxiliary);
    std::memset(targets,0xef,sizeof targets);
    Word(view,0,0x1111);Word(view,0x38,reinterpret_cast<U>(targets));Word(view,0x40,0x3333);
    Word(view,0x48,0x4444);Word(view,0x130,0x5555);Word(view,0x138,0x6666);
    desc_arg=reinterpret_cast<U>(descriptor);aux_arg=reinterpret_cast<U>(auxiliary);
    result_arg=reinterpret_cast<U>(view);
}
static U __fastcall Forward(U a,U b,U c) {
    ++calls;Check(a==factory_id&&b==desc_arg&&c==aux_arg,"All three arguments");
    Check(GetLastError()==incoming,"Original incoming LastError");
    Check(active.load()>0,"Active coverage of original");
    SetLastError(outgoing);return result_arg;
}
static U Invoke() {
    SetLastError(incoming);const U result=factory_hook(factory_id,desc_arg,aux_arg);
    Check(GetLastError()==outgoing,"Outgoing LastError");Check(result==result_arg,"Opaque RAX");return result;
}
static U __fastcall Mutate(U a,U b,U c) {
    const U result=Forward(a,b,c);std::memset(descriptor,0,sizeof descriptor);std::memset(auxiliary,0,sizeof auxiliary);
    SetLastError(outgoing);return result;
}
static void Forwarding() {
    original=Forward;Invoke();Check(calls==1&&active==0&&attempts==0,"Inactive forwarding");
    Inputs();Check(open(),"Open");original=Mutate;Invoke();
    Check(calls==2&&active==0&&event.ready&&attempts==1&&completed==1,"Active forwarding");
    Check(event.valid==15&&event.descriptor[0]==0xab&&event.auxiliary[0]==0xcd,"Capture BEFORE original");
    Check(event.target_params[0]==0xef&&event.target_params.back()==0xef,"Target params extent");
    Check(event.result==result_arg&&event.view_vtable==0x1111&&event.targets==reinterpret_cast<U>(targets)&&
        event.scene==0x3333&&event.camera==0x4444&&event.pipeline_owner==0x5555&&event.pipeline_state==0x6666,"View fields");
    Check(event.factory==factory_id&&event.descriptor_address==desc_arg&&event.auxiliary_address==aux_arg&&
        event.thread==GetCurrentThreadId()&&event.seq==1,"Input identities/thread/seq");
    close_window();original=Forward;Invoke();Check(calls==3&&attempts==1&&active==0,"Closed forwarding");
}
static U __fastcall Throw(U a,U b,U c) {Forward(a,b,c);SetLastError(outgoing);throw std::runtime_error("original");}
static void Exceptions() {
    original=Throw;Check(open(),"Open");bool caught=false;SetLastError(incoming);
    try {factory_hook(factory_id,0,0);} catch(const std::runtime_error& e) {caught=std::string(e.what())=="original";}
    Check(caught&&GetLastError()==outgoing&&active==0&&calls==1,"C++ exception propagation/finally/error");
    Check(!event.ready&&completed==0&&attempts==1,"Exception published event");close_window();
    caught=false;SetLastError(incoming);try {factory_hook(factory_id,0,0);} catch(const std::runtime_error&) {caught=true;}
    Check(caught&&active==0&&calls==2&&!event.ready,"Inactive exception propagation");
}
static U __fastcall SehOriginal(U,U,U) {++calls;SetLastError(outgoing);RaiseException(0xe0421234,0,0,nullptr);return 0;}
static bool CatchSeh() {
    __try {factory_hook(factory_id,0,0);} __except(GetExceptionCode()==0xe0421234?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
static void Seh() {
    original=SehOriginal;Check(open(),"Open");Check(CatchSeh(),"SEH propagated");
    Check(active==0&&calls==1&&!event.ready&&completed==0&&GetLastError()==outgoing,"SEH finally/no event/error");
}
static void Partial() {
    auto* pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));Check(pages!=nullptr,"Pages");
    DWORD old=0;Check(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old)!=0,"Noaccess");
    original=Forward;desc_arg=reinterpret_cast<U>(pages+4096-0x60);aux_arg=reinterpret_cast<U>(pages+4096-0x18);
    Check(open(),"Open");Invoke();Check(event.ready&&event.valid==2,"Partial descriptor must fail whole extent, aux exact boundary");
    unsigned char scratch[0x140]{};
    Check(!read(UINTPTR_MAX-3,scratch,8)&&!read(0,scratch,8)&&!read(1,scratch,8)&&!read(1,scratch,4097),"Invalid bounds");
    VirtualFree(pages,0,MEM_RELEASE);
}
static void Guard() {
    auto* pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));Check(pages!=nullptr,"Pages");
    DWORD old=0;Check(VirtualProtect(pages+4096,4096,PAGE_READWRITE|PAGE_GUARD,&old)!=0,"Guard");
    original=Forward;desc_arg=reinterpret_cast<U>(pages+4096-0x60);aux_arg=reinterpret_cast<U>(pages+4096);
    result_arg=reinterpret_cast<U>(pages+4096);Check(open(),"Open");Invoke();
    Check(event.ready&&event.valid==0,"Guard input/result accepted");
    MEMORY_BASIC_INFORMATION region{};Check(VirtualQuery(pages+4096,&region,sizeof region)&&region.Protect&PAGE_GUARD,"Guard consumed");
    VirtualFree(pages,0,MEM_RELEASE);
}
static void Once() {
    original=Forward;Check(open(),"Open");Invoke();Invoke();close_window();Invoke();
    Check(calls==3&&attempts==1&&completed==1&&active==0&&event.ready,"Single admission");Check(!open(),"Reopened single slot");
}
static std::atomic<unsigned> concurrent_calls{0};
static U __fastcall ConcurrentOriginal(U a,U b,U c) {
    if(a!=factory_id||b||c||GetLastError()!=incoming) return 0;
    concurrent_calls.fetch_add(1);SetLastError(outgoing);return opaque_result;
}
static void Concurrent() {
    original=ConcurrentOriginal;Check(open(),"Open");std::atomic<bool> valid{true};std::vector<std::thread> threads;
    for(unsigned i=0;i<24;++i) threads.emplace_back([&] {SetLastError(incoming);if(factory_hook(factory_id,0,0)!=opaque_result||GetLastError()!=outgoing) valid=false;});
    for(auto& t:threads) t.join();Check(valid&&concurrent_calls==24&&attempts==1&&completed==1&&active==0&&event.ready,"Concurrent admission/forwarding");
}
__declspec(noinline) static U __fastcall Local(U a,U b,U c) {return Forward(a,b,c);}
static void Minhook() {
    void* target=reinterpret_cast<void*>(&Local);unsigned char before[16]{};std::memcpy(before,target,16);
    Check(MH_Initialize()==MH_OK,"MH init");
    Check(MH_CreateHook(target,reinterpret_cast<void*>(&factory_hook),reinterpret_cast<void**>(&original))==MH_OK,"MH create");
    Check(open(),"Open");Check(MH_QueueEnableHook(target)==MH_OK&&MH_ApplyQueued()==MH_OK,"MH enable");
    FactoryFn volatile invoke=Local;
    SetLastError(incoming);Check(invoke(factory_id,0,0)==opaque_result&&GetLastError()==outgoing,"Hooked forwarding");
    close_window();SetLastError(incoming);Check(invoke(factory_id,0,0)==opaque_result&&GetLastError()==outgoing,"Hooked inactive forwarding");
    Check(MH_QueueDisableHook(target)==MH_OK&&MH_ApplyQueued()==MH_OK,"MH disable");
    Check(!std::memcmp(before,target,16)&&active==0&&calls==2&&attempts==1,"16-byte restoration/exactly once");
    // Trampoline intentionally remains mapped until this host exits.
}
static Report Metadata(unsigned pending=0) {
    Report r;r.image_base=0x140000000ull;r.pid=GetCurrentProcessId();r.process_creation=133700001234567890ull;
    r.closed_tick_ms=100;
    r.status="MH_OK";r.restored=true;r.pending_at_shutdown=pending;return r;
}
static std::string Text(const Report& r) {
    FILE* f=nullptr;Check(tmpfile_s(&f)==0&&f,"Temp file");const bool written=write_report(f,r);std::rewind(f);
    std::string text;char chunk[1024];while(const auto n=std::fread(chunk,1,sizeof chunk,f)) text.append(chunk,n);
    std::fclose(f);Check(written,"Report write");return text;
}
static HANDLE entered=nullptr,resume=nullptr;
static U __fastcall Blocked(U a,U b,U c) {
    Forward(a,b,c);SetEvent(entered);WaitForSingleObject(resume,5000);SetLastError(outgoing);return opaque_result;
}
static DWORD WINAPI BlockedThread(void*) {SetLastError(incoming);return factory_hook(factory_id,0,0)==opaque_result?0:1;}
static void Pending() {
    original=Blocked;Check(open(),"Open");entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);resume=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Check(entered&&resume,"Gates");HANDLE thread=CreateThread(nullptr,0,BlockedThread,nullptr,0,nullptr);Check(thread!=nullptr,"Thread");
    Check(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0&&active==1,"Entered");close_window();
    const auto before=Text(Metadata(active.load()));SetEvent(resume);Check(WaitForSingleObject(thread,2000)==WAIT_OBJECT_0,"Drain");
    CloseHandle(thread);CloseHandle(entered);CloseHandle(resume);
    Check(before.find("\"phase\":\"factory_event\"")==std::string::npos&&before.find("\"pending_at_shutdown\":1")!=std::string::npos,"Pending published");
    Check(active==0&&!event.ready&&completed==1,"Late completion published");
    Check(Text(Metadata()).find("\"phase\":\"factory_event\"")==std::string::npos,"Cutoff straddler published later");
}
static void Json() {
    Inputs();original=Forward;Check(open(),"Open");Invoke();close_window();const auto text=Text(Metadata());
    Check(text.find("\"schema\":\"native_view_factory_trace_v1\"")!=std::string::npos&&
        text.find("\"process_creation\":133700001234567890")!=std::string::npos&&
        text.find("\"closed_tick_ms\":100}")!=std::string::npos&&
        text.find("\"phase\":\"factory_event\"")!=std::string::npos&&text.find("\"valid\":15")!=std::string::npos&&
        text.find("\"attempts\":1")!=std::string::npos&&text.find("\"completed\":1")!=std::string::npos&&
        text.find("\"dll_retained_inactive\":true")!=std::string::npos&&text.find("\"frame_associated\":true")==std::string::npos,"JSON contract");
    auto failed=Metadata();failed.restored=false;Check(Text(failed).find("\"phase\":\"factory_event\"")==std::string::npos,"Failed restoration published");
    std::fputs(text.c_str(),stdout);
}
int main(int argc,char** argv) try {
    Check(argc==2,"Select case");const std::pair<const char*,void(*)()> cases[]{
        {"forwarding",Forwarding},{"exceptions",Exceptions},{"seh",Seh},{"partial",Partial},{"guard",Guard},
        {"once",Once},{"concurrent",Concurrent},{"minhook",Minhook},{"pending",Pending},{"json",Json}};
    for(const auto& c:cases) if(argv[1]==std::string(c.first)) {c.second();if(std::strcmp(c.first,"json")) std::printf("PASS %s\n",c.first);return 0;}
    throw std::runtime_error("Unknown case");
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
