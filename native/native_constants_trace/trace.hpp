#pragma once
#include <windows.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstddef>
namespace trace {
using U=std::uintptr_t;
using UploadFn=U(__fastcall*)(U,U,U,U);
using ResolveFn=U(__fastcall*)(U,U);
using DrawFn=U(__fastcall*)(U,unsigned,unsigned);
using TableFn=U(__fastcall*)(U,unsigned,U);
using DescriptorsFn=U(__fastcall*)(U,unsigned,unsigned,U,U);
extern UploadFn upload_original;
extern ResolveFn resolve_original;
extern DrawFn draw_original;
extern TableFn table_original;
extern DescriptorsFn descriptors_original;
extern std::atomic<bool> observing;
extern std::atomic<unsigned> active;
extern std::atomic<std::uint64_t> sequence;
extern thread_local U draw_scope;
extern thread_local U descriptor_scope;
struct Event {
    std::atomic<bool> ready{false};
    unsigned kind=0,thread=0;
    U begin=0,end=0,scope=0,caller=0,a[16]{};
    unsigned valid=0;
};
struct Upload {
    std::atomic<bool> ready{false};
    unsigned thread=0,valid=0;
    U begin=0,end=0,scope=0,caller=0,ref=0,buffer=0,context=0,source=0,size=0,destination=0;
    unsigned capacity=0,flags=0;
    unsigned char before[1024]{},after[1024]{};
};
inline constexpr unsigned upload_limit=4096,event_limit=32768;
extern std::array<Upload,upload_limit> uploads;
extern std::array<Event,event_limit> events;
extern std::atomic<unsigned> upload_count,event_count;
extern std::atomic<U> upload_calls,resolve_calls,draw_calls,table_calls;
extern std::atomic<U> descriptor_calls;
extern std::atomic<bool> stop_after_draw;
extern std::atomic<U> first_loss_sequence;
extern std::atomic<U> recording_cutoff;
void close_window() noexcept;
bool read(U address,void* output,std::size_t bytes) noexcept;
U __fastcall upload_hook(U,U,U,U);
U __fastcall resolve_hook(U,U);
U __fastcall draw_hook(U,unsigned,unsigned);
U __fastcall table_hook(U,unsigned,U);
U __fastcall descriptors_hook(U,unsigned,unsigned,U,U);
}
