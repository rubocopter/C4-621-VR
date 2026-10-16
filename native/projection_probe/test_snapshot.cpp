#include "snapshot.hpp"
#include <array>
#include <cstring>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
static void Check(bool v,const char* why) { if(!v) throw std::runtime_error(why); }
int main() try {
    alignas(16) std::array<unsigned char,0xc0> view{};
    auto f=[&](unsigned o,float value) { std::memcpy(view.data()+o,&value,4); };
    auto u=[&](unsigned o,std::uint32_t value) { std::memcpy(view.data()+o,&value,4); };
    f(0x50,1.553343f); f(0x54,1); f(0x58,500000); f(0x6c,16.f/9);
    u(0x5c,2560); u(0x60,1440); f(0x74,0.25f); f(0x78,-0.2f);
    float matrix[16]={0.572f,0,0,0, 0,1.0176f,0,0, 0.001f,-0.002f,-0.000002f,1, 0,0,1.000002f,0};
    ProjectionSnapshot sample;
    Check(ReadProjectionSnapshot(view.data(),matrix,sample),"Valid main-view projection must be observed");
    Check(sample.width==2560 && sample.height==1440 && sample.near_plane==1 && sample.far_plane==500000,"Viewport fields must preserve their offsets");
    Check(std::memcmp(sample.matrix,matrix,sizeof(matrix))==0,"Read-only snapshot must retain all projection coefficients");
    Check(sample.jitter_x==0.25f && sample.jitter_y==-0.2f,"Jitter must not be silently removed");
    u(0x94,1); Check(!ReadProjectionSnapshot(view.data(),matrix,sample),"Orthographic mode must be rejected");
    u(0x94,0); f(0x54,0); Check(!ReadProjectionSnapshot(view.data(),matrix,sample),"Zero near plane must be rejected");
    f(0x54,1); matrix[10]=std::numeric_limits<float>::quiet_NaN();
    Check(!ReadProjectionSnapshot(view.data(),matrix,sample),"Nonfinite matrix must be rejected");
    Check(!ReadProjectionSnapshot(nullptr,nullptr,sample),"Invalid addresses must not escape snapshot reader");
    std::cout<<"Projection snapshot tests passed\n"; return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
