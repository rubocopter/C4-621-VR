#include "static_stereo.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#include <string>
static void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() try {
    const auto folder=std::filesystem::temp_directory_path()/
        ("ac6-stereo-fixture-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(folder);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove(path/"left.raw",e); std::filesystem::remove(path/"right.raw",e); std::filesystem::remove(path,e); } } cleanup{folder};
    const std::array<std::filesystem::path,2> paths={folder/"left.raw",folder/"right.raw"};
    const std::vector<std::uint8_t> left={255,0,0,0, 0,0,0,0},right={0,0,255,255, 20,30,40,0};
    auto write=[](const auto& path,const auto& data) { std::ofstream f(path,std::ios::binary); f.write(reinterpret_cast<const char*>(data.data()),data.size()); };
    write(paths[0],left); write(paths[1],right);
    const auto pair=LoadStaticStereo(paths,2,1);
    Require(pair[0][0]==255&&pair[0][2]==0&&pair[1][0]==0&&pair[1][2]==255,"Eye order and RGB channels must be retained");
    Require(pair[0][3]==255&&pair[0][7]==255&&pair[1][7]==255,"Output must be opaque, including black holes");
    Require(pair[0][4]==0&&pair[1][4]==20&&pair[1][5]==30,"No resizing, filling or image swapping");
    bool rejected=false; write(paths[1],std::vector<std::uint8_t>{0,1,2});
    try { LoadStaticStereo(paths,2,1); } catch(const std::runtime_error&) { rejected=true; }
    Require(rejected,"Truncated raw must be rejected");
    rejected=false; write(paths[1],std::vector<std::uint8_t>(12));
    try { LoadStaticStereo(paths,2,1); } catch(const std::runtime_error&) { rejected=true; }
    Require(rejected,"Wrong dimensions or trailing bytes must be rejected");
    rejected=false; try { LoadStaticStereo(paths,0,1); } catch(const std::exception&) { rejected=true; }
    Require(rejected,"Zero dimensions must be rejected");
    std::cout<<"PASS: static stereo eye order, channels, opaque holes and exact size\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
