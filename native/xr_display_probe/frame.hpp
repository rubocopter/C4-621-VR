#pragma once
#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <vector>

// Input is window-capture BGRA with potentially padded rows; output is RGBA.
inline std::vector<std::uint8_t> FitWindowFrame(const std::uint8_t* source, unsigned sw,
    unsigned sh, unsigned stride, unsigned width, unsigned height) {
    if (!source || !sw || !sh || !width || !height || sw>16384 || sh>16384 ||
        width>16384 || height>16384 || stride<sw*4)
        throw std::invalid_argument("Invalid window frame dimensions or stride");
    std::vector<std::uint8_t> result(static_cast<std::size_t>(width)*height*4,0);
    for (std::size_t i=3; i<result.size(); i+=4) result[i]=255;
    const double scale=std::min(double(width)/sw,double(height)/sh);
    const unsigned fw=std::max(1u,unsigned(sw*scale)), fh=std::max(1u,unsigned(sh*scale));
    const unsigned left=(width-fw)/2, top=(height-fh)/2;
    for (unsigned y=0; y<fh; ++y) for (unsigned x=0; x<fw; ++x) {
        const auto* in=source+std::size_t(std::uint64_t(y)*sh/fh)*stride+(std::uint64_t(x)*sw/fw)*4;
        auto* out=result.data()+(std::size_t(y+top)*width+x+left)*4;
        out[0]=in[2]; out[1]=in[1]; out[2]=in[0];
    }
    return result;
}
