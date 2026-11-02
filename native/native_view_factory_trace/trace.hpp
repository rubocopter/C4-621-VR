#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace factory_trace {
using U=std::uintptr_t;
using FactoryFn=U(__fastcall*)(U,U,U);
static_assert(sizeof(U)==8,"x64 only");
inline constexpr U factory_rva=0x22f0c50;
// Exact 16 bytes read from the local disk PE, file offset 0x22f0250.
inline constexpr std::array<unsigned char,16> build_prologue{
    0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24};
enum class Window : unsigned { idle,open,pending,complete,closed };
struct Event {
    std::atomic<bool> ready{false};
    unsigned seq=0,thread=0,valid=0;
    U factory=0,descriptor_address=0,auxiliary_address=0,result=0;
    std::array<unsigned char,0x70> descriptor{};
    std::array<unsigned char,0x18> auxiliary{};
    std::array<unsigned char,0x78> target_params{};
    U view_vtable=0,targets=0,scene=0,camera=0,pipeline_owner=0,pipeline_state=0;
};
extern FactoryFn original;
extern Event event;
extern std::atomic<Window> window;
extern std::atomic<unsigned> active,attempts,completed;
bool read(U address,void* output,std::size_t bytes) noexcept;
bool open() noexcept;
void close_window() noexcept;
U __fastcall factory_hook(U factory,U descriptor,U auxiliary);
}
