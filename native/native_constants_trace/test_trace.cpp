#include "trace.hpp"
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace trace;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while(0)
static unsigned calls=0;
static U expected[4]{},dest=0;
static bool args_ok=true;
static U __fastcall fake_upload(U a,U b,U c,U d) {
    ++calls; args_ok &= a==expected[0] && b==expected[1] && c==expected[2] && d==expected[3];
    std::memcpy(reinterpret_cast<void*>(dest),reinterpret_cast<void*>(c),d);
    std::memset(reinterpret_cast<void*>(c),0x99,d); SetLastError(431); return dest;
}
static U __fastcall fake_resolve(U a,U b) { ++calls; args_ok &= a==expected[0] && b==expected[1]; return 0x123456789abcdef0ull; }
static U __fastcall fake_table(U a,unsigned b,U c) { ++calls; args_ok &= a==expected[0] && b==expected[1] && c==expected[2]; return 0x8877665544332211ull; }
static U __fastcall fake_draw(U a,unsigned b,unsigned c) { ++calls; args_ok &= a==expected[0] && b==expected[1] && c==expected[2]; if(!draw_scope) args_ok=false; return 0xdeadbeef12345678ull; }
static U __fastcall throws_draw(U,unsigned,unsigned) { throw std::runtime_error("original exception"); }
static U fifth=0;
static bool require_descriptor_scope=true;
static U __fastcall nested_resolve(U,U) { args_ok &= !require_descriptor_scope || descriptor_scope!=0; return 1234; }
static U __fastcall fake_descriptors(U a,unsigned b,unsigned c,U d,U e) { ++calls; args_ok &= a==expected[0] && b==expected[1] && c==expected[2] && d==expected[3] && e==fifth && (require_descriptor_scope?descriptor_scope!=0:descriptor_scope==0); resolve_hook(0,0); return 0x1234567887654321ull; }
static U __fastcall throws_descriptors(U,unsigned,unsigned,U,U) { throw std::runtime_error("descriptor original"); }
static HANDLE admitted=nullptr,resume_call=nullptr;
static U __fastcall blocked_resolve(U,U) { SetEvent(admitted); WaitForSingleObject(resume_call,2000); return 991; }
static DWORD WINAPI resolving_thread(void*) { return resolve_hook(0,0)==991?0:1; }
__declspec(noinline) static U __fastcall host_target(U a,U b) { return (a*0x79+b)^0x987654321ull; }
static ResolveFn host_original=nullptr;
static U __fastcall host_detour(U a,U b) { return host_original(a,b); }
int main() {
    unsigned char src[80],dst[80],buffer[64]{},context[0x2600]{}; U ref=reinterpret_cast<U>(buffer);
    std::memset(src,0x35,80); dest=reinterpret_cast<U>(dst);
    unsigned cap=256; std::memcpy(buffer+0x28,&cap,4);
    expected[0]=reinterpret_cast<U>(&ref); expected[1]=reinterpret_cast<U>(context); expected[2]=reinterpret_cast<U>(src); expected[3]=80;
    upload_original=fake_upload; resolve_original=fake_resolve; table_original=fake_table; draw_original=fake_draw;
    observing=true;
    CHECK(upload_hook(expected[0],expected[1],expected[2],80)==dest); CHECK(calls==1 && args_ok && GetLastError()==431);
    CHECK(uploads[0].ready && uploads[0].valid==7 && uploads[0].before[0]==0x35 && uploads[0].after[0]==0x35 && src[0]==0x99 && uploads[0].capacity==256);
    // A recycled CPU destination yields distinct, immutable upload records.
    std::memset(src,0x47,80); CHECK(upload_hook(expected[0],expected[1],expected[2],80)==dest);
    CHECK(uploads[0].before[0]==0x35 && uploads[1].before[0]==0x47 && uploads[0].end<uploads[1].begin);
    expected[0]=0; expected[1]=99; CHECK(resolve_hook(0,99)==0x123456789abcdef0ull);
    expected[0]=0; expected[1]=7; expected[2]=0x123456789abcdef0ull; CHECK(table_hook(0,7,expected[2])==0x8877665544332211ull);
    expected[0]=reinterpret_cast<U>(context); expected[1]=17; expected[2]=91; draw_scope=555;
    CHECK(draw_hook(expected[0],17,91)==0xdeadbeef12345678ull && draw_scope==555 && args_ok);
    draw_original=throws_draw; bool caught=false; try { draw_hook(expected[0],17,91); } catch(const std::runtime_error&) { caught=true; }
    CHECK(caught && draw_scope==555 && active==0);
    descriptors_original=fake_descriptors; resolve_original=nested_resolve;
    U heap[8]{0x123400,0x456700,32},native_list[]={ref},mapped_list[]={dest};
    expected[0]=reinterpret_cast<U>(heap); expected[1]=3; expected[2]=1; expected[3]=reinterpret_cast<U>(native_list); fifth=reinterpret_cast<U>(mapped_list);
    descriptor_scope=777; CHECK(descriptors_hook(expected[0],3,1,expected[3],fifth)==0x1234567887654321ull && descriptor_scope==777 && args_ok && active==0);
    CHECK(events[event_count.load()-1].a[7]!=0 && events[event_count.load()-1].kind==1);
    descriptors_original=throws_descriptors; caught=false;
    try { descriptors_hook(expected[0],3,1,expected[3],fifth); } catch(const std::runtime_error&) { caught=true; }
    CHECK(caught && descriptor_scope==777 && active==0);
    admitted=CreateEventW(nullptr,TRUE,FALSE,nullptr); resume_call=CreateEventW(nullptr,TRUE,FALSE,nullptr); CHECK(admitted && resume_call);
    resolve_original=blocked_resolve; HANDLE thread=CreateThread(nullptr,0,resolving_thread,nullptr,0,nullptr); CHECK(thread);
    CHECK(WaitForSingleObject(admitted,2000)==WAIT_OBJECT_0 && active==1);
    observing=false; CHECK(active==1); SetEvent(resume_call); CHECK(WaitForSingleObject(thread,2000)==WAIT_OBJECT_0 && active==0);
    CloseHandle(thread); CloseHandle(admitted); CloseHandle(resume_call); resolve_original=fake_resolve;
    void* pages=VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); CHECK(pages);
    DWORD old=0; CHECK(VirtualProtect(static_cast<char*>(pages)+4096,4096,PAGE_NOACCESS,&old));
    unsigned char scratch[80]{}; CHECK(!read(reinterpret_cast<U>(pages)+4090,scratch,80)); CHECK(!read(1,scratch,80)); CHECK(!read(0,scratch,80));
    CHECK(VirtualProtect(static_cast<char*>(pages)+4096,4096,PAGE_READWRITE|PAGE_GUARD,&old));
    CHECK(!read(reinterpret_cast<U>(pages)+4096,scratch,1));
    MEMORY_BASIC_INFORMATION region{}; CHECK(VirtualQuery(static_cast<char*>(pages)+4096,&region,sizeof(region)) && (region.Protect&PAGE_GUARD));
    CHECK(VirtualFree(pages,0,MEM_RELEASE));
    observing=false; expected[0]=0; expected[1]=99; auto n=event_count.load(); CHECK(resolve_hook(0,99)==0x123456789abcdef0ull && event_count==n);
    observing=true; event_count=event_limit; CHECK(resolve_hook(0,99)==0x123456789abcdef0ull && event_count==event_limit+1);
    CHECK(!observing && first_loss_sequence!=0);
    observing=true;
    expected[0]=reinterpret_cast<U>(heap); expected[1]=3; expected[2]=1; expected[3]=reinterpret_cast<U>(native_list);
    descriptors_original=fake_descriptors; resolve_original=nested_resolve; require_descriptor_scope=false;
    CHECK(descriptors_hook(expected[0],3,1,expected[3],fifth)==0x1234567887654321ull && descriptor_scope==777 && active==0 && args_ok);
    observing=true; event_count=1024; upload_count=256; stop_after_draw=true; draw_original=fake_draw;
    expected[0]=reinterpret_cast<U>(context); expected[1]=17; expected[2]=91;
    CHECK(draw_hook(expected[0],17,91)==0xdeadbeef12345678ull && !observing && active==0);
    unsigned char original[16]; std::memcpy(original,reinterpret_cast<void*>(&host_target),16);
    ResolveFn volatile invoke=host_target; auto baseline=invoke(14,88);
    CHECK(MH_Initialize()==MH_OK); CHECK(MH_CreateHook(reinterpret_cast<void*>(&host_target),reinterpret_cast<void*>(&host_detour),reinterpret_cast<void**>(&host_original))==MH_OK);
    CHECK(MH_EnableHook(reinterpret_cast<void*>(&host_target))==MH_OK); CHECK(invoke(14,88)==baseline);
    CHECK(MH_DisableHook(reinterpret_cast<void*>(&host_target))==MH_OK); CHECK(invoke(14,88)==baseline && !std::memcmp(original,reinterpret_cast<void*>(&host_target),16));
    std::puts("PASS: forwarding, immutable reuse, bounded invalid reads, exception/scope restoration, capacity, MinHook restoration");
}
