#include "trace.hpp"
#include <cstring>
namespace view_trace {
RenderAllFn render_all_original=nullptr;
ViewRenderFn view_render_original=nullptr;
thread_local Scope scope{};
std::array<Event,event_limit> events;
std::atomic<bool> observing{false};
std::atomic<unsigned> active{0},event_attempts{0},completed_frames{0};
std::atomic<U> sequence{0},recording_cutoff{0},first_loss_sequence{0},unmatched_lists{0},unscoped_views{0};
U expected_list=0;
static unsigned stop_after_frames=4;
bool read(U address,void* output,std::size_t bytes) noexcept {
    if(!address||!output||!bytes||bytes>4096||address>UINTPTR_MAX-bytes) return false;
    U cursor=address;
    while(cursor<address+bytes) {
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<void*>(cursor),&region,sizeof(region))||region.State!=MEM_COMMIT||
            (region.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
        // PAGE_EXECUTE alone does not declare readable memory.
        const DWORD access=region.Protect&0xff;
        if(access!=PAGE_READONLY&&access!=PAGE_READWRITE&&access!=PAGE_WRITECOPY&&
            access!=PAGE_EXECUTE_READ&&access!=PAGE_EXECUTE_READWRITE&&access!=PAGE_EXECUTE_WRITECOPY) return false;
        const U base=reinterpret_cast<U>(region.BaseAddress);
        if(base>UINTPTR_MAX-region.RegionSize) return false;
        const U end=base+region.RegionSize;
        if(end<=cursor) return false;
        cursor=end<address+bytes?end:address+bytes;
    }
    __try {std::memcpy(output,reinterpret_cast<void*>(address),bytes);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
static bool word(U object,U offset,U& value) noexcept {
    return object&&object<=UINTPTR_MAX-offset&&read(object+offset,&value,sizeof(value));
}
static bool identity_once(U global,Identity& i) noexcept {
    return word(global,0,i.manager)&&i.manager&&word(i.manager,8,i.list)&&i.list&&
        word(i.manager,0x10,i.view_manager)&&i.view_manager&&word(i.view_manager,0x3c8,i.main_view)&&i.main_view;
}
bool read_identity(U global,Identity& result) noexcept {
    Identity a{},b{};
    if(!identity_once(global,a)||!identity_once(global,b)||a.manager!=b.manager||a.list!=b.list||
        a.view_manager!=b.view_manager||a.main_view!=b.main_view) return false;
    result=a;return true; // Equal samples are not atomic ownership/lifetime proof.
}
bool open(U list,unsigned stop_after) noexcept {
    if(!list||!stop_after||active.load()||observing.load()||event_attempts.load()||recording_cutoff.load()) return false;
    expected_list=list;stop_after_frames=stop_after;
    observing.store(true,std::memory_order_release);return true;
}
void close_window() noexcept {
    if(!recording_cutoff.load(std::memory_order_acquire)) {
        const U point=sequence.fetch_add(1)+1;U zero=0;
        recording_cutoff.compare_exchange_strong(zero,point,std::memory_order_acq_rel);
    }
    observing.store(false,std::memory_order_release);
}
static Event* reserve(unsigned kind,U list,U view,U context) noexcept {
    const auto index=event_attempts.fetch_add(1);
    if(index>=event_limit) {
        const U point=sequence.fetch_add(1)+1;U zero=0;
        first_loss_sequence.compare_exchange_strong(zero,point);
        close_window();return nullptr;
    }
    auto* e=&events[index];e->kind=kind;e->list=list;e->view=view;e->context=context;
    e->thread=GetCurrentThreadId();e->parent=scope.token;e->begin=sequence.fetch_add(1)+1;
    return e;
}
static void finish(Event* e) noexcept {
    if(!e) return;
    e->end=sequence.fetch_add(1)+1;
    e->ready.store(true,std::memory_order_release);
    // The worker filters straddlers using end < cutoff. Count successful
    // admitted RenderAll completions, never original exceptions.
    if(e->kind==1&&completed_frames.fetch_add(1)+1>=stop_after_frames) close_window();
}
U __fastcall render_all_hook(U list,U context) {
    const DWORD incoming=GetLastError();active.fetch_add(1);
    const Scope parent=scope;Event* e=nullptr;U result=0;
    __try {
        // Shadow even an unmatched nested call: its descendants cannot inherit
        // the enclosing list's token. All paths restore the parent's TLS.
        scope={};
        if(observing.load(std::memory_order_acquire)) {
            if(list==expected_list) {
                scope=parent;e=reserve(1,list,0,context);scope={};
                if(e) scope={e->begin,list,context};
            } else unmatched_lists.fetch_add(1);
        }
        SetLastError(incoming);result=render_all_original(list,context);
        const DWORD outgoing=GetLastError();finish(e);SetLastError(outgoing);
    } __finally {
        const DWORD outgoing=GetLastError();scope=parent;active.fetch_sub(1);SetLastError(outgoing);
    }
    return result;
}
U __fastcall view_render_hook(U self,U context,U frame_index_ptr) {
    const DWORD incoming=GetLastError();active.fetch_add(1);
    Event* e=nullptr;U result=0;
    __try {
        if(observing.load(std::memory_order_acquire)) {
            if(!scope.token) unscoped_views.fetch_add(1);
            else {
                e=reserve(2,scope.list,self,context);
                if(e) {
                    if(word(self,0,e->vtable)) e->valid|=1;
                    if(word(self,0x38,e->targets)) e->valid|=2;
                    if(word(self,0x40,e->scene)) e->valid|=4;
                    if(word(self,0x48,e->camera)) e->valid|=8;
                }
            }
        }
        SetLastError(incoming);result=view_render_original(self,context,frame_index_ptr);
        const DWORD outgoing=GetLastError();finish(e);SetLastError(outgoing);
    } __finally {
        const DWORD outgoing=GetLastError();active.fetch_sub(1);SetLastError(outgoing);
    }
    return result;
}
}
