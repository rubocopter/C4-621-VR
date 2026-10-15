#include "head_rotation.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
static Quaternion Yaw(float degrees) {
    const float a=degrees*3.14159265358979323846f/360;
    return {0,std::sin(a),0,std::cos(a)};
}
static void Equal(const std::array<float,9>& a,const std::array<float,9>& b) {
    for(unsigned i=0;i<9;++i) if(std::abs(a[i]-b[i])>1e-5f) throw std::runtime_error("Relative head rotation mismatch");
}
int main() {
    try {
        Equal(RelativeRows(Yaw(37),Yaw(37)),{1,0,0,0,1,0,0,0,1});
        const float a=25*3.14159265358979323846f/180;
        Equal(RelativeRows(Yaw(30),Yaw(120)),{std::cos(a),0,-std::sin(a),0,1,0,std::sin(a),0,std::cos(a)});
        const float s=std::sqrt(0.5f);
        // Reference roll 90 degrees followed by local yaw 20 degrees.
        const auto y=Yaw(20); const Quaternion current{-s*y[1],s*y[1],s*y[3],s*y[3]};
        const float b=20*3.14159265358979323846f/180;
        Equal(RelativeRows({0,0,s,s},current),{std::cos(b),0,-std::sin(b),0,1,0,std::sin(b),0,std::cos(b)});
        bool rejected=false;
        try { RelativeRows(Yaw(0),{0,0,0,0}); } catch(const std::invalid_argument&) { rejected=true; }
        if(!rejected) throw std::runtime_error("Zero quaternion accepted");
        // XR looks along -Z; this AC6 camera needs reflected Z coordinates.
        // Reflection must invert yaw/pitch together while preserving roll.
        Equal(CameraRows(Yaw(30),Yaw(120)),{std::cos(a),0,std::sin(a),0,1,0,-std::sin(a),0,std::cos(a)});
        const float half=10*3.14159265358979323846f/360;
        const float angle=half*2;
        Equal(CameraRows(Yaw(0),{std::sin(half),0,0,std::cos(half)}),
            {1,0,0,0,std::cos(angle),-std::sin(angle),0,std::sin(angle),std::cos(angle)});
        Equal(CameraRows(Yaw(0),{0,0,std::sin(half),std::cos(half)}),
            {std::cos(angle),std::sin(angle),0,-std::sin(angle),std::cos(angle),0,0,0,1});
        std::cout << "Head rotation tests passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
