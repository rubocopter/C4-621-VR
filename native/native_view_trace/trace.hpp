#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace view_trace {
using U=std::uintptr_t;
using RenderAllFn=U(__fastcall*)(U,U);
using ViewRenderFn=U(__fastcall*)(U,U,U);
static_assert(sizeof(U)==8,"x64 only");
inline constexpr unsigned event_limit=128;
inline constexpr U render_all_rva=0x2466510,view_render_rva=0x2524790,manager_rva=0x4e23028;
inline constexpr const char* build_sha256="e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6";
using Signatures=std::array<std::array<unsigned char,16>,2>;
inline constexpr Signatures build_signatures{{
    {{0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24}},
    {{0x40,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xf0,0x48,0x81,0xec}}
}};
struct Scope { U token=0,list=0,context=0; };
struct Event {
    std::atomic<bool> ready{false};
    unsigned kind=0,thread=0,valid=0;
    U begin=0,end=0,parent=0,list=0,view=0,context=0;
    U vtable=0,targets=0,scene=0,camera=0;
};
struct Identity { U manager=0,list=0,view_manager=0,main_view=0; };
extern RenderAllFn render_all_original;
extern ViewRenderFn view_render_original;
extern thread_local Scope scope;
extern std::array<Event,event_limit> events;
extern std::atomic<bool> observing;
extern std::atomic<unsigned> active,event_attempts,completed_frames;
extern std::atomic<U> sequence,recording_cutoff,first_loss_sequence,unmatched_lists,unscoped_views;
extern U expected_list;
bool read(U address,void* output,std::size_t bytes) noexcept;
bool read_identity(U global_address,Identity& result) noexcept;
// One initialization per module/process. Fixtures each run in a fresh process.
bool open(U list,unsigned stop_after=4) noexcept;
void close_window() noexcept;
U __fastcall render_all_hook(U list,U context);
U __fastcall view_render_hook(U self,U context,U frame_index_ptr);
}
