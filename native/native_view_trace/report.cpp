#include "report.hpp"
#include <cstdarg>
namespace view_trace {
static bool emit(FILE* file,const char* format,...) noexcept {
    va_list args;va_start(args,format);const int n=std::vfprintf(file,format,args);va_end(args);return n>=0;
}
bool write_report(FILE* file,const Report& r) noexcept {
    if(!file) return false;
    bool ok=emit(file,"{\"phase\":\"observing\",\"schema\":\"native_view_trace_v1\",\"image_base\":%llu,\"pid\":%lu,\"expected_list\":%llu,\"expected_main_view\":%llu,\"frame_associated\":false}\n",
        r.image_base,r.pid,r.identity.list,r.identity.main_view);
    const U rvas[]{render_all_rva,view_render_rva};
    for(unsigned i=0;i<2;++i)
        ok=emit(file,"{\"phase\":\"restoration\",\"rva\":%llu,\"restored\":%s}\n",rvas[i],r.restored[i]?"true":"false")&&ok;
    unsigned saved=0;
    const U cutoff=recording_cutoff.load(std::memory_order_acquire);
    // On a failed drain/restoration, do not traverse any event storage.
    const bool safe=r.allow_events&&r.restored[0]&&r.restored[1]&&!r.pending_at_shutdown&&
        !active.load(std::memory_order_acquire)&&!observing.load(std::memory_order_acquire);
    if(safe) for(const auto& e:events) {
        if(!e.ready.load(std::memory_order_acquire)||!e.begin||e.begin>=e.end||!cutoff||e.end>=cutoff) continue;
        ++saved;
        ok=emit(file,"{\"phase\":\"event\",\"kind\":%u,\"begin\":%llu,\"end\":%llu,\"parent\":%llu,\"thread\":%u,\"list\":%llu,\"view\":%llu,\"context\":%llu,\"vtable\":%llu,\"targets\":%llu,\"scene\":%llu,\"camera\":%llu,\"valid\":%u}\n",
            e.kind,e.begin,e.end,e.parent,e.thread,e.list,e.view,e.context,e.vtable,e.targets,e.scene,e.camera,e.valid)&&ok;
    }
    const unsigned attempts=event_attempts.load();
    ok=emit(file,"{\"phase\":\"disabled\",\"status\":\"%s\",\"disable_status\":\"%s\",\"hooks_restored\":%s,\"saved_events\":%u,\"event_attempts\":%u,\"event_overflow\":%u,\"pending_at_shutdown\":%u,\"completed_frames\":%u,\"recording_cutoff\":%llu,\"first_loss_sequence\":%llu,\"unmatched_lists\":%llu,\"unscoped_views\":%llu,\"dll_retained_inactive\":true,\"frame_associated\":false}\n",
        r.status,r.disable_status,r.restored[0]&&r.restored[1]?"true":"false",saved,attempts,attempts>event_limit?attempts-event_limit:0,
        r.pending_at_shutdown,completed_frames.load(),cutoff,first_loss_sequence.load(),unmatched_lists.load(),unscoped_views.load())&&ok;
    return ok&&std::fflush(file)==0;
}
}
