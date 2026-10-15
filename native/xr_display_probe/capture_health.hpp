#pragma once
#include <cstdint>
inline bool CaptureFresh(std::uint64_t now,std::uint64_t last,bool seen) {
    return seen && now>=last && now-last<=500;
}
