#pragma once
#include <array>
#include <cmath>
#include <stdexcept>
using Quaternion=std::array<float,4>;
inline std::array<float,9> RelativeRows(Quaternion reference,Quaternion current,float limit=25.f) {
    auto normalize=[](Quaternion& q) {
        float norm=0;
        for(float v:q) { if(!std::isfinite(v)) throw std::invalid_argument("Nonfinite quaternion"); norm+=v*v; }
        if(norm<1e-12f) throw std::invalid_argument("Zero quaternion");
        norm=std::sqrt(norm); for(float& v:q) v/=norm;
    };
    if(!std::isfinite(limit) || limit<0 || limit>180) throw std::invalid_argument("Invalid angle limit");
    normalize(reference); normalize(current);
    const auto& a=reference; const auto& b=current;
    Quaternion q={a[3]*b[0]-a[0]*b[3]-a[1]*b[2]+a[2]*b[1],
                  a[3]*b[1]+a[0]*b[2]-a[1]*b[3]-a[2]*b[0],
                  a[3]*b[2]-a[0]*b[1]+a[1]*b[0]-a[2]*b[3],
                  a[3]*b[3]+a[0]*b[0]+a[1]*b[1]+a[2]*b[2]};
    if(q[3]<0) for(float& v:q) v=-v;
    const float norm=std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]);
    const float maximum=limit*3.14159265358979323846f/180;
    if(2*std::atan2(norm,q[3])>maximum && norm>1e-8f) {
        const float scale=std::sin(maximum/2)/norm;
        for(unsigned i=0;i<3;++i) q[i]*=scale;
        q[3]=std::cos(maximum/2);
    }
    normalize(q);
    const float x=q[0],y=q[1],z=q[2],w=q[3];
    // Transpose of XR column-vector rotation, matching the tested Python path.
    return {1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w),
            2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w),
            2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y)};
}
inline std::array<float,9> CameraRows(Quaternion reference,Quaternion current) {
    auto rows=RelativeRows(reference,current);
    const float signs[3]={1,1,-1};
    for(unsigned i=0;i<3;++i) for(unsigned j=0;j<3;++j) rows[i*3+j]*=signs[i]*signs[j];
    return rows;
}
