#include "trace.hpp"
#include "report.hpp"
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
using namespace view_trace;
static void Check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
static constexpr U list_id=0x123456789abcdef0ull,ctx_id=0x23456789abcdef01ull,frame_id=0xfedcba9876543210ull;
static constexpr U render_result=0xfedc9876543210abull,view_result=0xabcd9876543210efull;
static constexpr DWORD incoming_error=0x1379,outgoing_error=0x2468;
static unsigned render_calls=0,view_calls=0;
static U expected_self=0;
static bool forwarding_ok=true;
static U __fastcall forward_render(U a,U b) {
    ++render_calls;forwarding_ok &= a==list_id&&b==ctx_id&&GetLastError()==incoming_error;
    SetLastError(outgoing_error);return render_result;
}
static U __fastcall forward_view(U a,U b,U c) {
    ++view_calls;forwarding_ok &= a==expected_self&&b==ctx_id&&c==frame_id&&GetLastError()==incoming_error;
    SetLastError(outgoing_error);return view_result;
}
static void Originals() {render_all_original=forward_render;view_render_original=forward_view;}
static void Render() {SetLastError(incoming_error);Check(render_all_hook(list_id,ctx_id)==render_result,"Render opaque RAX");Check(GetLastError()==outgoing_error,"Render last error");}
static void ViewCall(U self=0) {expected_self=self;SetLastError(incoming_error);Check(view_render_hook(self,ctx_id,frame_id)==view_result,"View opaque RAX");Check(GetLastError()==outgoing_error,"View last error");}
struct FakeView { U vtable=0x1111,padding[6]{},targets=0x2222,scene=0x3333,camera=0x4444; };
static_assert(offsetof(FakeView,targets)==0x38&&offsetof(FakeView,scene)==0x40&&offsetof(FakeView,camera)==0x48);
static FakeView view;
static U __fastcall with_view(U a,U b) {
    ++render_calls;forwarding_ok &= a==list_id&&b==ctx_id&&GetLastError()==incoming_error;
    Check(scope.token!=0&&scope.list==list_id,"Missing admitted TLS scope");
    ViewCall(reinterpret_cast<U>(&view));SetLastError(outgoing_error);return render_result;
}
static void Forwarding() {
    Originals();Render();ViewCall();Check(render_calls==1&&view_calls==1&&forwarding_ok&&active==0,"Inactive forwarding");
    Check(event_attempts==0,"Inactive path admitted event");Check(open(list_id),"Open failed");
    render_all_original=with_view;Render();Check(render_calls==2&&view_calls==2&&forwarding_ok&&active==0,"Active forwarding");
    Check(event_attempts==2&&events[0].ready&&events[1].ready,"Events not published");
    Check(events[0].kind==1&&events[0].view==0&&events[0].context==ctx_id&&events[0].list==list_id,"Render fields");
    const auto& e=events[1];Check(e.kind==2&&e.parent==events[0].begin&&e.thread==GetCurrentThreadId(),"View parent/thread");
    Check(e.vtable==0x1111&&e.targets==0x2222&&e.scene==0x3333&&e.camera==0x4444&&e.valid==15,"Snapshot field bits");
    Check(events[0].begin<e.begin&&e.begin<e.end&&e.end<events[0].end,"Ordinal nesting");
    close_window();render_all_original=forward_render;Render();ViewCall();Check(forwarding_ok&&active==0&&event_attempts==2,"Closed forwarding");
}
static U __fastcall throwing_view(U,U,U) {++view_calls;SetLastError(outgoing_error);throw std::runtime_error("original");}
static U __fastcall throwing_render(U,U) {++render_calls;SetLastError(incoming_error);return view_render_hook(0,ctx_id,frame_id);}
static void Exceptions() {
    Originals();Check(open(list_id),"Open failed");render_all_original=throwing_render;view_render_original=throwing_view;
    scope={0x777,0x888,0x999};bool caught=false;
    try {render_all_hook(list_id,ctx_id);} catch(const std::runtime_error& e) {caught=std::string(e.what())=="original";}
    Check(caught&&active==0&&scope.token==0x777&&scope.list==0x888&&scope.context==0x999,"Exception/TLS/active forwarding");
    Check(event_attempts==2&&!events[0].ready&&!events[1].ready&&completed_frames==0,"Exception published complete event");
    close_window();caught=false;try {render_all_hook(list_id,ctx_id);} catch(const std::runtime_error&) {caught=true;}
    Check(caught&&active==0&&scope.token==0x777&&render_calls==2&&view_calls==2,"Inactive exception forwarding");
}
static unsigned depth=0;
static U __fastcall nested(U a,U b) {
    ++render_calls;Check(a==list_id&&b==ctx_id,"Nested arguments");const auto token=scope.token;
    if(depth++==0) {
        SetLastError(incoming_error);render_all_hook(list_id,ctx_id);Check(scope.token==token,"Nested TLS not restored");
    }
    ViewCall(reinterpret_cast<U>(&view));--depth;SetLastError(outgoing_error);return render_result;
}
static U __fastcall unmatched(U a,U b) {
    ++render_calls;Check(a==list_id+1&&b==ctx_id&&!scope.token,"Unmatched list inherited parent");
    ViewCall();return render_result;
}
static void Nesting() {
    Originals();Check(open(list_id,20),"Open failed");render_all_original=nested;Render();
    Check(event_attempts==4&&events[1].parent==events[0].begin&&events[2].parent==events[1].begin&&events[3].parent==events[0].begin,"Nested parent tokens");
    Check(!scope.token&&completed_frames==2,"Nested scope cleanup");
    auto n=event_attempts.load();ViewCall();Check(event_attempts==n&&unscoped_views==1,"Parentless view joined nearest event");
    scope={0x555,list_id,ctx_id};render_all_original=unmatched;
    render_all_hook(list_id+1,ctx_id);Check(scope.token==0x555&&unmatched_lists==1&&unscoped_views==2&&event_attempts==n,"Unmatched list filtering");
}
static U __fastcall many(U,U) {for(unsigned i=0;i<event_limit;++i) ViewCall();SetLastError(outgoing_error);return render_result;}
static void Bounds() {
    Originals();Check(open(list_id,1000),"Open failed");render_all_original=many;Render();
    Check(!observing&&event_attempts==129&&first_loss_sequence&&recording_cutoff&&active==0,"Overflow did not close admission");
    Check(events[127].ready&&events[0].end>recording_cutoff,"Straddling render not distinguishable by cutoff");
    auto n=event_attempts.load();render_all_original=forward_render;Render();Check(event_attempts==n,"Post-overflow allocation");
}
static void Guard() {
    void* pages=VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);Check(pages!=nullptr,"Allocate pages");
    DWORD old=0;Check(VirtualProtect(static_cast<char*>(pages)+4096,4096,PAGE_READWRITE|PAGE_GUARD,&old)!=0,"Protect guard");
    U scratch=0;Check(!read(reinterpret_cast<U>(pages)+4096,&scratch,1),"Consumed guard read");
    MEMORY_BASIC_INFORMATION region{};Check(VirtualQuery(static_cast<char*>(pages)+4096,&region,sizeof(region))&&region.Protect&PAGE_GUARD,"Guard flag consumed");
    Check(!read(reinterpret_cast<U>(pages)+4090,&scratch,8),"Cross-page guard read");
    Check(!read(UINTPTR_MAX-3,&scratch,8)&&!read(0,&scratch,8)&&!read(1,&scratch,8),"Bad address read");
    std::memcpy(pages,&list_id,8);Check(read(reinterpret_cast<U>(pages),&scratch,8)&&scratch==list_id,"Valid read rejected");
    Check(VirtualFree(pages,0,MEM_RELEASE)!=0,"Free pages");
}
static void Cutoff() {
    Originals();Check(open(list_id),"Open failed");for(unsigned i=0;i<4;++i) {Render();Check(completed_frames==i+1,"Completed frames");Check(bool(observing)==(i!=3),"Wrong stop threshold");}
    Check(recording_cutoff>events[3].end&&first_loss_sequence==0&&active==0,"Cutoff ordering");
    Render();Check(event_attempts==4&&render_calls==5,"Inactive original after fourth scope");
}
static void IdentityRead() {
    U holder[0x3d0/8]{};U manager[3]{};U root=reinterpret_cast<U>(manager);manager[1]=list_id;manager[2]=reinterpret_cast<U>(holder);holder[0x3c8/8]=0x12345678;
    Identity identity{};Check(read_identity(reinterpret_cast<U>(&root),identity),"Identity read failed");
    Check(identity.manager==root&&identity.list==list_id&&identity.view_manager==manager[2]&&identity.main_view==0x12345678,"Wrong root offsets");
    manager[1]=0;Check(!read_identity(reinterpret_cast<U>(&root),identity),"Null expected list admitted");
}
__declspec(noinline) static U __fastcall local_render(U a,U b) {return forward_render(a,b);}
__declspec(noinline) static U __fastcall local_view(U a,U b,U c) {return forward_view(a,b,c);}
static void MinHookFixture() {
    Originals();Check(open(list_id),"Open failed");void* targets[]{reinterpret_cast<void*>(&local_render),reinterpret_cast<void*>(&local_view)};
    unsigned char before[2][16]{};for(unsigned i=0;i<2;++i) std::memcpy(before[i],targets[i],16);
    Check(MH_Initialize()==MH_OK,"MH initialize");
    Check(MH_CreateHook(targets[0],reinterpret_cast<void*>(&render_all_hook),reinterpret_cast<void**>(&render_all_original))==MH_OK,"Render install");
    Check(MH_CreateHook(targets[1],reinterpret_cast<void*>(&view_render_hook),reinterpret_cast<void**>(&view_render_original))==MH_OK,"View install");
    for(auto* target:targets) Check(MH_QueueEnableHook(target)==MH_OK,"Queue enable");Check(MH_ApplyQueued()==MH_OK,"Enable apply");
    RenderAllFn volatile invoke_render=local_render;ViewRenderFn volatile invoke_view=local_view;
    for(unsigned inactive=0;inactive<2;++inactive) {
        if(inactive) close_window();expected_self=0;
        SetLastError(incoming_error);Check(invoke_render(list_id,ctx_id)==render_result&&GetLastError()==outgoing_error,"Hooked Render forwarding/error");
        SetLastError(incoming_error);Check(invoke_view(0,ctx_id,frame_id)==view_result&&GetLastError()==outgoing_error,"Hooked View forwarding/error");
    }
    Check(forwarding_ok&&render_calls==2&&view_calls==2&&active==0,"Hooked exact calls/arguments");
    for(auto* target:targets) Check(MH_QueueDisableHook(target)==MH_OK,"Queue disable");Check(MH_ApplyQueued()==MH_OK,"Disable apply");
    for(unsigned i=0;i<2;++i) Check(!std::memcmp(before[i],targets[i],16),"Independent prologue restoration");
    // Keep trampolines until host exits, as the DLL worker does.
}
static Report Metadata(unsigned pending=0) {
    Report r;r.image_base=0x140000000ull;r.pid=GetCurrentProcessId();r.identity.list=list_id;r.identity.main_view=reinterpret_cast<U>(&view);
    r.status="MH_OK";r.disable_status="MH_OK";r.restored={true,true};r.pending_at_shutdown=pending;r.allow_events=pending==0;return r;
}
static std::string ReportText(const Report& r) {
    FILE* file=nullptr;Check(tmpfile_s(&file)==0&&file,"Temporary report");
    const bool wrote=write_report(file,r);std::rewind(file);std::string text;char chunk[1024];
    while(const auto n=std::fread(chunk,1,sizeof chunk,file)) text.append(chunk,n);
    std::fclose(file);Check(wrote,"Report write failed");return text;
}
static HANDLE entered=nullptr,resume=nullptr;
static U __fastcall blocked(U,U) {SetEvent(entered);WaitForSingleObject(resume,2000);SetLastError(outgoing_error);return render_result;}
static DWORD WINAPI blocked_thread(void*) {return render_all_hook(list_id,ctx_id)==render_result?0:1;}
static void Shutdown() {
    Originals();Check(open(list_id),"Open failed");render_all_original=blocked;
    entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);resume=CreateEventW(nullptr,TRUE,FALSE,nullptr);Check(entered&&resume,"Fixture gates");
    HANDLE thread=CreateThread(nullptr,0,blocked_thread,nullptr,0,nullptr);Check(thread!=nullptr,"Fixture thread");
    Check(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0&&active==1,"Fixture did not enter");
    close_window();const auto pending=active.load();auto pending_report=Metadata(pending);
    // Summary only: never traverse mutable event storage while callback remains.
    const auto before=ReportText(pending_report);
    SetEvent(resume);Check(WaitForSingleObject(thread,2000)==WAIT_OBJECT_0&&active==0,"Callback did not drain");
    CloseHandle(thread);CloseHandle(entered);CloseHandle(resume);
    Check(before.find("\"phase\":\"event\"")==std::string::npos&&before.find("\"pending_at_shutdown\":1")!=std::string::npos,"Pending report published events");
    Check(events[0].ready&&events[0].end>recording_cutoff,"Late completion interval");
    const auto after=ReportText(Metadata());
    Check(after.find("\"saved_events\":0")!=std::string::npos&&after.find("\"event_attempts\":1")!=std::string::npos,"Cutoff straddler published");
}
static void Json() {
    Originals();Check(open(list_id),"Open failed");render_all_original=with_view;view.vtable=0x143e3e7e0ull;
    for(unsigned i=0;i<4;++i) Render();
    const auto text=ReportText(Metadata());
    Check(text.find("\"schema\":\"native_view_trace_v1\"")!=std::string::npos&&text.find("\"saved_events\":8")!=std::string::npos&&
        text.find("\"completed_frames\":4")!=std::string::npos&&text.find("\"frame_associated\":true")==std::string::npos,"JSON schema/counts");
    std::fputs(text.c_str(),stdout); // Usable synthetic JSONL, no game involved.
}
int main(int argc,char** argv) try {
    Check(argc==2,"Select case");const std::pair<const char*,void(*)()> cases[]={{"forwarding",Forwarding},{"exceptions",Exceptions},{"nesting",Nesting},{"bounds",Bounds},{"guard",Guard},{"cutoff",Cutoff},{"identity",IdentityRead},{"minhook",MinHookFixture},{"shutdown",Shutdown},{"json",Json}};
    for(const auto& c:cases) if(argv[1]==std::string(c.first)) {c.second();if(std::strcmp(c.first,"json")) std::printf("PASS %s\n",c.first);return 0;}
    throw std::runtime_error("Unknown case");
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
