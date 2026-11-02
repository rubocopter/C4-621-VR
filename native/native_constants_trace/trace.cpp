#include "trace.hpp"
#include <cstring>
#include <intrin.h>
namespace trace {
UploadFn upload_original=nullptr;
ResolveFn resolve_original=nullptr;
DrawFn draw_original=nullptr;
TableFn table_original=nullptr;
DescriptorsFn descriptors_original=nullptr;
std::atomic<bool> observing{false};
std::atomic<unsigned> active{0};
std::atomic<std::uint64_t> sequence{0};
thread_local U draw_scope=0;
thread_local U descriptor_scope=0;
std::array<Upload,upload_limit> uploads;
std::array<Event,event_limit> events;
std::atomic<unsigned> upload_count{0},event_count{0};
std::atomic<U> upload_calls{0},resolve_calls{0},draw_calls{0},table_calls{0};
std::atomic<U> descriptor_calls{0};
std::atomic<bool> stop_after_draw{false};
std::atomic<U> first_loss_sequence{0};
std::atomic<U> recording_cutoff{0};
void close_window() noexcept {
    U zero=0,point=++sequence;
    recording_cutoff.compare_exchange_strong(zero,point);
    observing.store(false,std::memory_order_release);
}
bool read(U address,void* output,std::size_t bytes) noexcept {
    if(!address || !output || bytes>4096 || address>UINTPTR_MAX-bytes) return false;
    // Do not consume PAGE_GUARD state while rejecting an invalid snapshot.
    U cursor=address;
    while(cursor<address+bytes) {
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<void*>(cursor),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
           region.Protect&(PAGE_NOACCESS|PAGE_GUARD)) return false;
        U end=reinterpret_cast<U>(region.BaseAddress)+region.RegionSize;
        if(end<=cursor) return false;
        cursor=end<address+bytes?end:address+bytes;
    }
    __try { std::memcpy(output,reinterpret_cast<void*>(address),bytes); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void lost() noexcept {
    U zero=0,point=++sequence;
    first_loss_sequence.compare_exchange_strong(zero,point);
    close_window();
}
static U word(U address) noexcept { U out=0; return read(address,&out,sizeof(out))?out:0; }
static Event* reserve(unsigned kind,U caller) noexcept {
    auto index=event_count.fetch_add(1,std::memory_order_relaxed);
    if(index>=event_limit) { lost(); return nullptr; }
    auto* e=&events[index]; e->kind=kind; e->caller=caller; e->begin=++sequence;
    e->thread=GetCurrentThreadId(); e->scope=draw_scope; return e;
}
static void finish(Event* e) noexcept { if(e) { e->end=++sequence; e->ready.store(true,std::memory_order_release); } }
U __fastcall upload_hook(U ref,U context,U source,U size) {
    ++upload_calls; ++active;
    if(!observing.load(std::memory_order_acquire)) { __try { return upload_original(ref,context,source,size); } __finally { --active; } }
    const DWORD incoming=GetLastError(); Upload* e=nullptr; U result=0;
    __try {
        auto index=upload_count.fetch_add(1,std::memory_order_relaxed);
        if(index<upload_limit) {
            e=&uploads[index]; e->begin=++sequence; e->thread=GetCurrentThreadId(); e->scope=draw_scope;
            e->caller=reinterpret_cast<U>(_ReturnAddress()); e->ref=ref; e->buffer=word(ref);
            e->context=context; e->source=source; e->size=size;
            if(e->buffer && read(e->buffer+0x28,&e->capacity,4) && read(e->buffer+0x34,&e->flags,4)) e->valid|=4;
            if(size && size<=sizeof(e->before) && read(source,e->before,size)) e->valid|=1;
        } else lost();
        SetLastError(incoming); result=upload_original(ref,context,source,size); const DWORD outgoing=GetLastError();
        if(e) {
            e->destination=result;
            if(size && size<=sizeof(e->after) && read(result,e->after,size)) e->valid|=2;
            e->end=++sequence; e->ready.store(true,std::memory_order_release);
        }
        SetLastError(outgoing);
    } __finally { --active; }
    return result;
}
U __fastcall resolve_hook(U owner,U cpu) {
    ++resolve_calls; ++active;
    if(!observing.load(std::memory_order_acquire)) { __try { return resolve_original(owner,cpu); } __finally { --active; } }
    const DWORD incoming=GetLastError(); Event* e=nullptr; U result=0;
    __try {
        e=reserve(1,reinterpret_cast<U>(_ReturnAddress()));
        if(e) { e->a[0]=owner; e->a[1]=cpu; e->a[7]=descriptor_scope;
            if(owner && read(owner+0x18,&e->a[2],8)) e->valid|=1;
            if(owner && read(owner+0x20,&e->a[3],8)) e->valid|=2;
        }
        SetLastError(incoming); result=resolve_original(owner,cpu); const DWORD outgoing=GetLastError();
        if(e) { e->a[4]=result;
            if(owner && read(owner+0x18,&e->a[5],8)) e->valid|=4;
            if(owner && read(owner+0x20,&e->a[6],8)) e->valid|=8;
        }
        finish(e); SetLastError(outgoing);
    } __finally { --active; }
    return result;
}
static void bindings(Event* e,U context) noexcept {
    if(!e) return;
    const U offsets[]={0x1608,0x1678,0x6c8,0x738,0xa98,0xb08};
    for(unsigned i=0;i<6;++i) { if(read(context+offsets[i],&e->a[6+i],sizeof(U))) e->valid|=1u<<i; }
}
U __fastcall draw_hook(U context,unsigned vertices,unsigned start) {
    ++draw_calls; ++active;
    if(!observing.load(std::memory_order_acquire)) { __try { return draw_original(context,vertices,start); } __finally { --active; } }
    const DWORD incoming=GetLastError(); Event* e=nullptr; U result=0; const U parent=draw_scope;
    __try {
        e=reserve(2,reinterpret_cast<U>(_ReturnAddress()));
        if(e) { e->a[0]=context; e->a[1]=vertices; e->a[2]=start;
            if(context && read(context+0x10,&e->a[3],8)) e->valid|=64;
            if(e->a[3] && read(e->a[3],&e->a[4],8)) e->valid|=128;
            if(e->a[3] && read(e->a[3]+0x70,&e->a[12],8)) e->valid|=256;
            bindings(e,context); draw_scope=e->begin; }
        else draw_scope=0; // No recorded enclosing event: never attribute to the parent.
        SetLastError(incoming); result=draw_original(context,vertices,start); const DWORD outgoing=GetLastError();
        if(e) { e->a[5]=result; } finish(e);
        // A short, completed native recording scope is enough for the joiner.
        // Stop admitting snapshots instead of filling buffers for eight seconds.
        if(stop_after_draw.load() && upload_count.load()>=256 && event_count.load()>=1024) close_window();
        SetLastError(outgoing);
    } __finally { draw_scope=parent; --active; }
    return result;
}
U __fastcall table_hook(U wrapper,unsigned root,U handle) {
    ++table_calls; ++active;
    if(!observing.load(std::memory_order_acquire)) { __try { return table_original(wrapper,root,handle); } __finally { --active; } }
    const DWORD incoming=GetLastError(); Event* e=nullptr; U result=0;
    __try {
        e=reserve(3,reinterpret_cast<U>(_ReturnAddress()));
        if(e) { e->a[0]=wrapper; e->a[1]=root; e->a[2]=handle;
            if(wrapper && read(wrapper+0x78,&e->a[4],1)) e->valid|=1;
            if(wrapper && read(wrapper,&e->a[3],8)) e->valid|=2;
            if(wrapper && read(wrapper+0x70,&e->a[6],8)) e->valid|=4;
        }
        SetLastError(incoming); result=table_original(wrapper,root,handle); const DWORD outgoing=GetLastError();
        if(e) { e->a[5]=result;
            if(wrapper && read(wrapper,&e->a[7],8)) e->valid|=8;
            if(wrapper && read(wrapper+0x70,&e->a[8],8)) e->valid|=16;
        } finish(e); SetLastError(outgoing);
    } __finally { --active; }
    return result;
}
U __fastcall descriptors_hook(U heap,unsigned index,unsigned count,U buffers,U mapped) {
    ++descriptor_calls; ++active;
    if(!observing.load(std::memory_order_acquire)) { __try { return descriptors_original(heap,index,count,buffers,mapped); } __finally { --active; } }
    const DWORD incoming=GetLastError(); Event* e=nullptr; U result=0; const U parent=descriptor_scope;
    __try {
        e=reserve(4,reinterpret_cast<U>(_ReturnAddress())); descriptor_scope=e?e->begin:0;
        if(e) {
            e->a[0]=heap; e->a[1]=index; e->a[2]=count; e->a[3]=buffers; e->a[4]=mapped;
            if(read(heap,&e->a[5],8)) e->valid|=1;
            if(read(heap+8,&e->a[6],8)) e->valid|=2;
            if(read(heap+0x10,&e->a[7],8)) e->valid|=4;
            if(read(heap+0x20,&e->a[8],8)) e->valid|=8;
            if(e->a[8] && read(e->a[8],&e->a[9],8)) e->valid|=16;
            if(read(heap+0x18,&e->a[11],8)) e->valid|=32;
            if(count>32) e->valid|=0x80000000u;
            for(unsigned j=0;j<count && j<32;++j) {
                auto* item=reserve(5,0); if(!item) break;
                item->a[0]=e->begin; item->a[1]=j;
                if(buffers && read(buffers+8ull*j,&item->a[2],8)) item->valid|=1;
                if(mapped && read(mapped+8ull*j,&item->a[3],8)) item->valid|=2;
                if(item->a[2] && read(item->a[2]+0x28,&item->a[4],4)) item->valid|=4;
                item->a[5]=word(item->a[2]+0x38);
                // Derive handles offline with checked arithmetic; these are sampled fields.
                finish(item);
            }
        }
        SetLastError(incoming); result=descriptors_original(heap,index,count,buffers,mapped); const DWORD outgoing=GetLastError();
        if(e) e->a[10]=result; finish(e); SetLastError(outgoing);
    } __finally { descriptor_scope=parent; --active; }
    return result;
}
}
