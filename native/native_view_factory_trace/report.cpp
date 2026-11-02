#include "report.hpp"
#include <cstdarg>
#include <cstring>
namespace factory_trace {
static bool emit(FILE* file,const char* format,...) noexcept {
    va_list args;va_start(args,format);const int n=std::vfprintf(file,format,args);va_end(args);return n>=0;
}
template<std::size_t N> static void hex(const std::array<unsigned char,N>& bytes,char (&out)[N*2+1],bool valid) noexcept {
    if(!valid) {out[0]=0;return;}
    constexpr char digits[]="0123456789abcdef";
    for(std::size_t i=0;i<N;++i) {out[2*i]=digits[bytes[i]>>4];out[2*i+1]=digits[bytes[i]&15];}
    out[N*2]=0;
}
bool write_report(FILE* file,const Report& r) noexcept {
    if(!file) return false;
    bool ok=emit(file,"{\"phase\":\"observing\",\"schema\":\"native_view_factory_trace_v1\",\"image_base\":%llu,\"pid\":%lu,\"process_creation\":%llu,\"closed_tick_ms\":%llu}\n",
        r.image_base,r.pid,r.process_creation,r.closed_tick_ms);
    ok=emit(file,"{\"phase\":\"restoration\",\"rva\":%llu,\"restored\":%s}\n",factory_rva,r.restored?"true":"false")&&ok;
    const bool inactive=window.load(std::memory_order_acquire)==Window::closed&&
        !r.pending_at_shutdown&&!active.load(std::memory_order_acquire)&&r.restored&&!std::strcmp(r.disable_status,"MH_OK");
    // Never inspect mutable event payload while a callback is pending; never
    // publish a straddler or an unsuccessful original invocation.
    if(inactive&&!std::strcmp(r.status,"MH_OK")&&event.ready.load(std::memory_order_acquire)) {
        char descriptor[0x70*2+1]{},auxiliary[0x18*2+1]{},params[0x78*2+1]{};
        hex(event.descriptor,descriptor,(event.valid&1)!=0);hex(event.auxiliary,auxiliary,(event.valid&2)!=0);
        hex(event.target_params,params,(event.valid&8)!=0);
        ok=emit(file,"{\"phase\":\"factory_event\",\"seq\":%u,\"thread\":%u,\"factory\":%llu,\"descriptor_address\":%llu,\"auxiliary_address\":%llu,\"result\":%llu,\"valid\":%u,\"descriptor_hex\":\"%s\",\"auxiliary_hex\":\"%s\",\"target_params_hex\":\"%s\",\"view_vtable\":%llu,\"targets\":%llu,\"scene\":%llu,\"camera\":%llu,\"pipeline_owner\":%llu,\"pipeline_state\":%llu}\n",
            event.seq,event.thread,event.factory,event.descriptor_address,event.auxiliary_address,event.result,event.valid,
            descriptor,auxiliary,params,event.view_vtable,event.targets,event.scene,event.camera,event.pipeline_owner,event.pipeline_state)&&ok;
    }
    ok=emit(file,"{\"phase\":\"disabled\",\"status\":\"%s\",\"disable_status\":\"%s\",\"hooks_restored\":%s,\"attempts\":%u,\"completed\":%u,\"pending_at_shutdown\":%u,\"dll_retained_inactive\":%s,\"frame_associated\":false}\n",
        r.status,r.disable_status,r.restored?"true":"false",attempts.load(),completed.load(),r.pending_at_shutdown,inactive?"true":"false")&&ok;
    return ok&&std::fflush(file)==0;
}
}
