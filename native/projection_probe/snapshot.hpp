#pragma once
#include <cstdint>
#include <windows.h>
#include <cstring>
#include <cmath>
struct ProjectionSnapshot {
    std::uintptr_t view=0;
    float fov=0,near_plane=0,far_plane=0,aspect=0;
    std::uint32_t width=0,height=0;
    float jitter_x=0,jitter_y=0,shift_x=0,shift_y=0;
    float matrix[16]{};
    std::uint32_t projection_mode=0;
};
inline bool ReadProjectionSnapshot(const void* view,const void* matrix,ProjectionSnapshot& output) noexcept {
    __try {
        if(!view || !matrix) return false;
        const auto* bytes=static_cast<const unsigned char*>(view);
        ProjectionSnapshot result;
        result.view=reinterpret_cast<std::uintptr_t>(view);
        std::memcpy(&result.fov,bytes+0x50,4); std::memcpy(&result.near_plane,bytes+0x54,4);
        std::memcpy(&result.far_plane,bytes+0x58,4); std::memcpy(&result.width,bytes+0x5c,4);
        std::memcpy(&result.height,bytes+0x60,4); std::memcpy(&result.aspect,bytes+0x6c,4);
        std::memcpy(&result.jitter_x,bytes+0x74,4); std::memcpy(&result.jitter_y,bytes+0x78,4);
        std::memcpy(&result.projection_mode,bytes+0x94,4);
        std::memcpy(&result.shift_x,bytes+0x98,4); std::memcpy(&result.shift_y,bytes+0x9c,4);
        std::memcpy(result.matrix,matrix,64);
        if(result.projection_mode!=0 || result.width<128 || result.height<128 || result.width>16384 || result.height>16384 ||
           !std::isfinite(result.fov) || result.fov<=0 || result.fov>=3.141593f ||
           !std::isfinite(result.near_plane) || result.near_plane<=0 || !std::isfinite(result.far_plane) || result.far_plane<=result.near_plane ||
           !std::isfinite(result.aspect) || result.aspect<0 || result.aspect>16 ||
           !std::isfinite(result.jitter_x) || !std::isfinite(result.jitter_y) ||
           !std::isfinite(result.shift_x) || !std::isfinite(result.shift_y)) return false;
        for(float value:result.matrix) if(!std::isfinite(value)) return false;
        output=result; return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
