#include "trace.hpp"
#include <cstring>
namespace factory_trace {
FactoryFn original=nullptr;
Event event;
std::atomic<Window> window{Window::idle};
std::atomic<unsigned> active{0},attempts{0},completed{0};
// Minimal copy of native_view_trace's proven guarded read. Validate every
// region before touching source bytes, including a trailing guard page.
bool read(U address,void* output,std::size_t bytes) noexcept {
    if(!address||!output||!bytes||bytes>4096||address>UINTPTR_MAX-bytes) return false;
    U cursor=address;
    while(cursor<address+bytes) {
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<void*>(cursor),&region,sizeof region)||region.State!=MEM_COMMIT||
            (region.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
        const DWORD access=region.Protect&0xff;
        if(access!=PAGE_READONLY&&access!=PAGE_READWRITE&&access!=PAGE_WRITECOPY&&
            access!=PAGE_EXECUTE_READ&&access!=PAGE_EXECUTE_READWRITE&&access!=PAGE_EXECUTE_WRITECOPY) return false;
        const U base=reinterpret_cast<U>(region.BaseAddress);
        if(base>UINTPTR_MAX-region.RegionSize) return false;
        const U end=base+region.RegionSize;if(end<=cursor) return false;
        cursor=end<address+bytes?end:address+bytes;
    }
    __try {std::memcpy(output,reinterpret_cast<void*>(address),bytes);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool open() noexcept {
    Window expected=Window::idle;
    return window.compare_exchange_strong(expected,Window::open,std::memory_order_acq_rel);
}
void close_window() noexcept {window.exchange(Window::closed,std::memory_order_acq_rel);}
static U word(const unsigned char* bytes,unsigned offset) noexcept {
    U result=0;std::memcpy(&result,bytes+offset,sizeof result);return result;
}
static void after_original(U result) noexcept {
    event.result=result;
    unsigned char prefix[0x140]{};
    if(read(result,prefix,sizeof prefix)) {
        event.valid|=4;event.view_vtable=word(prefix,0);event.targets=word(prefix,0x38);
        event.scene=word(prefix,0x40);event.camera=word(prefix,0x48);
        event.pipeline_owner=word(prefix,0x130);event.pipeline_state=word(prefix,0x138);
        if(event.targets&&event.targets<=UINTPTR_MAX-0x20&&
            read(event.targets+0x20,event.target_params.data(),event.target_params.size())) event.valid|=8;
    }
}
U __fastcall factory_hook(U factory,U descriptor,U auxiliary) {
    const DWORD incoming=GetLastError();active.fetch_add(1,std::memory_order_acq_rel);
    bool admitted=false;U result=0;
    __try {
        Window expected=Window::open;
        admitted=window.compare_exchange_strong(expected,Window::pending,std::memory_order_acq_rel);
        if(admitted) {
            attempts.fetch_add(1,std::memory_order_relaxed);
            event.seq=1;event.thread=GetCurrentThreadId();event.factory=factory;
            event.descriptor_address=descriptor;event.auxiliary_address=auxiliary;
            if(read(descriptor,event.descriptor.data(),event.descriptor.size())) event.valid|=1;
            if(read(auxiliary,event.auxiliary.data(),event.auxiliary.size())) event.valid|=2;
        }
        SetLastError(incoming);result=original(factory,descriptor,auxiliary);
        const DWORD outgoing=GetLastError();
        if(admitted) {
            completed.fetch_add(1,std::memory_order_release);
            if(window.load(std::memory_order_acquire)==Window::pending) {
                after_original(result);
                Window pending=Window::pending;
                // Linearize completion against worker cutoff. A late result
                // can never become ready, even when the worker has exited.
                if(window.compare_exchange_strong(pending,Window::complete,std::memory_order_acq_rel))
                    event.ready.store(true,std::memory_order_release);
            }
        }
        SetLastError(outgoing);
    } __finally {
        const DWORD outgoing=GetLastError();active.fetch_sub(1,std::memory_order_acq_rel);SetLastError(outgoing);
    }
    return result;
}
}
