#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <stdexcept>
namespace live_pair {
constexpr LONG Version=1;
enum Status : LONG { Preparing=0,Ready=1,Stopped=2,Failed=3 };
struct alignas(8) Slot { volatile LONG64 sequence=0,tick=0; };
struct alignas(8) Header {
    LONG version=Version;
    DWORD producer_pid=0;
    LUID adapter{};
    UINT width=0,height=0,color_format=0,depth_pitch=0,color_pitch=0;
    volatile LONG stop_requested=0;
    UINT64 depth_offset=0,color_offset=0,buffer_bytes=0;
    volatile LONG status=Preparing;
    volatile LONG consumer_pid=0;
    Slot slots[3]{};
};
static_assert(sizeof(Header)==120&&offsetof(Header,stop_requested)==36,"Windows x64 protocol layout must remain fixed");
inline LONG64 Read64(const volatile LONG64& value) { return InterlockedCompareExchange64(const_cast<volatile LONG64*>(&value),0,0); }
inline LONG Read32(const volatile LONG& value) { return InterlockedCompareExchange(const_cast<volatile LONG*>(&value),0,0); }
inline bool Reusable(UINT64 sequence,UINT64 ready,UINT64 released) {
    return ready!=UINT64_MAX&&released!=UINT64_MAX&&(!sequence || (ready>=sequence&&released>=sequence));
}
inline int Newest(const Header& h,UINT64 last) {
    int selected=-1; UINT64 best=last;
    for(unsigned i=0;i<3;++i) { const auto n=static_cast<UINT64>(Read64(h.slots[i].sequence)); if(n>best) { best=n;selected=static_cast<int>(i); } }
    return selected;
}
inline bool Fresh(UINT64 now,UINT64 tick) { return tick&&now>=tick&&now-tick<=500; }
inline bool Valid(const Header& h) {
    if(h.version!=Version||!h.producer_pid||h.width<128||h.height<128||h.width>8192||h.height>8192||
       static_cast<UINT64>(h.width)*h.height>65535ull*256||h.depth_pitch<h.width*4||h.color_pitch<h.width*4||
       h.depth_pitch%256||h.color_pitch%256||h.depth_offset%512||h.color_offset%512||
       (h.color_format!=24&&h.color_format!=28&&h.color_format!=87)) return false;
    const auto depth_end=h.depth_offset+UINT64(h.depth_pitch)*h.height;
    const auto color_end=h.color_offset+UINT64(h.color_pitch)*h.height;
    return depth_end>=h.depth_offset&&color_end>=h.color_offset&&depth_end<=h.buffer_bytes&&color_end<=h.buffer_bytes&&
        depth_end<=UINT_MAX&&color_end<=UINT_MAX&&
        (depth_end<=h.color_offset||color_end<=h.depth_offset);
}
inline std::wstring ObjectName(const std::wstring& base,const wchar_t* suffix) { return base+suffix; }
}
