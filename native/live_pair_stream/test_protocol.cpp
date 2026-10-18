#include "protocol.hpp"
#include "json_diagnostic.hpp"
#include <iostream>
static void Require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
int main() try {
    using namespace live_pair;
    Require(JsonModuleName(L"overlay.dll")=="\"overlay.dll\"","ASCII module name changed");
    Require(JsonModuleName(L"m\u00f3d.dll")=="\"m\\u00f3d.dll\"","Unicode module names must be valid JSON independent of CRT locale");
    Require(JsonModuleName(L"a\"\\\n.dll")=="\"a\\u0022\\u005c\\u000a.dll\"","JSON metacharacters must be escaped");
    Require(JsonModuleName(std::wstring{wchar_t(0xd83d),wchar_t(0xde80)})=="\"\\ud83d\\ude80\"","UTF-16 surrogate pairs must be preserved");
    Require(Reusable(0,0,0),"Unused slot should be writable");
    Require(!Reusable(4,3,4)&&!Reusable(4,4,3),"Both GPU timelines must complete before reuse");
    Require(Reusable(4,6,7),"Skipping old frames releases only after both fences");
    Require(!Reusable(0,UINT64_MAX,0),"Device removal must reject even unused slots");
    Header h; h.producer_pid=123; h.width=2560; h.height=1440; h.color_format=24;
    h.depth_pitch=h.color_pitch=10240; h.color_offset=14745600; h.buffer_bytes=29491200;
    Require(Valid(h),"Valid padded shared buffer layout rejected");
    h.slots[0].sequence=1; h.slots[1].sequence=5; h.slots[2].sequence=3;
    Require(Newest(h,0)==1&&Newest(h,4)==1&&Newest(h,5)==-1,"Newest selection must be monotonic");
    Require(Fresh(1000,500)&&!Fresh(1001,500)&&!Fresh(99,100)&&!Fresh(0,0),"Stale/future/uninitialised timestamps rejected");
    h.color_offset=512; Require(!Valid(h),"Overlapping planes rejected");
    h.depth_pitch=h.color_pitch=0xffffff00; h.color_offset=UINT64(h.depth_pitch)*h.height;
    h.buffer_bytes=h.color_offset*2;
    Require(!Valid(h),"Shader byte-address offsets must not overflow uint32");
    std::cout<<"PASS: ring ownership, drops, newest selection, freshness and metadata bounds\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
