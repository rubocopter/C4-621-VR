#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <stdexcept>
#include <fstream>

inline std::array<std::vector<std::uint8_t>,2> LoadStaticStereo(
    const std::array<std::filesystem::path,2>& paths,unsigned width,unsigned height) {
    if(!width||!height||width>16384||height>16384)
        throw std::invalid_argument("Invalid static stereo dimensions");
    const auto bytes=static_cast<std::size_t>(width)*height*4;
    std::array<std::vector<std::uint8_t>,2> result;
    for(unsigned eye=0;eye<2;++eye) {
        if(paths[eye].empty()||std::filesystem::file_size(paths[eye])!=bytes)
            throw std::runtime_error("Static eye must be exact-size RGBA8");
        std::ifstream file(paths[eye],std::ios::binary);
        result[eye].resize(bytes);
        file.read(reinterpret_cast<char*>(result[eye].data()),bytes);
        if(!file||file.peek()!=std::char_traits<char>::eof())
            throw std::runtime_error("Incomplete or changed static eye file");
        for(std::size_t i=3;i<bytes;i+=4) result[eye][i]=255;
    }
    return result;
}
