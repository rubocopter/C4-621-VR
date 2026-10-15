#pragma once
#include <cstdint>
inline bool CaptureFresh(std::uint64_t now,std::uint64_t last,bool seen) {
    return seen && now>=last && now-last<=500;
}
inline bool HeadTrackingReady(bool visible,bool rendering,bool fresh,bool orientation_valid,bool orientation_tracked) {
    return visible && rendering && fresh && orientation_valid && orientation_tracked;
}
