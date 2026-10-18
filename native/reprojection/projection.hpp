#pragma once
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace reprojection {
// Row-vector perspective: clip=(fx*x+jx*z, fy*y+jy*z, a*z+b, wsign*z).
// Device depth is [0,1]. ForwardDistance is w=wsign*z, in game units.
// Both parallel eyes use THIS SAME projection; jx/jy cancel, fy scales no
// horizontal displacement. This contract cannot describe asymmetric XR eyes.
struct Perspective {
    float fx,fy,a,b,wsign;
    float ForwardDistance(float depth) const { return wsign*b/(wsign*depth-a); }
    void Validate() const {
        if(!std::isfinite(fx)||fx<=0||!std::isfinite(fy)||fy<=0||!std::isfinite(a)||
           !std::isfinite(b)||b==0||(wsign!=1&&wsign!=-1))
            throw std::invalid_argument("Unsupported perspective coefficients");
        // SM5 flushes subnormal inputs/results to signed zero. Do not accept
        // coefficients whose meaning changes on entering shader arithmetic.
        for(float value:{fx,fy,a,b})
            if(std::fpclassify(value)==FP_SUBNORMAL)
                throw std::invalid_argument("Subnormal perspective coefficient unsupported by SM5");
        // The linear denominator must not cross a pole inside [0,1]. A pole
        // at an endpoint is allowed (infinite far); shaders reject that sample.
        const double d0=-double(a),d1=double(wsign)-a;
        if(d0*d1<0) throw std::invalid_argument("Perspective pole inside device depth range");
        for(float d:{0.f,1.f}) {
            if(wsign*d-a==0) continue;
            const float distance=ForwardDistance(d);
            if(!std::isnormal(distance)||distance<=0)
                throw std::invalid_argument("Perspective depth range outside finite forward half-space");
        }
    }
    static Perspective FromRowMatrix(const std::array<float,16>& m) {
        for(float value:m) if(!std::isfinite(value)) throw std::invalid_argument("Nonfinite perspective matrix");
        // Reject unsupported terms rather than silently dropping shear or an
        // orthographic/general projection. Principal-point offsets are allowed.
        for(unsigned n:{1u,2u,3u,4u,6u,7u,12u,13u,15u})
            if(m[n]!=0) throw std::invalid_argument("Unsupported row-vector perspective shape");
        Perspective result{m[0],m[5],m[10],m[14],m[11]};result.Validate();return result;
    }
};
enum class CalibrationProvenance { LegacyAc6Observation, ExplicitUnassociated };
// Static consumer configuration, NOT per-frame metadata and NOT a wire format.
struct ProjectionCalibration {
    Perspective model;
    CalibrationProvenance provenance;
    static ProjectionCalibration LegacyObservedAc6() {
        // PROJECTION_FINDINGS.md: helper observations on 2026-10-16, separate
        // from the color/depth interval. No claim these match a current frame.
        return {{0.572404087f,1.01760733f,-2.02655792e-6f,1.00000203f,1},CalibrationProvenance::LegacyAc6Observation};
    }
    void Validate() const {
        model.Validate();
        if(provenance==CalibrationProvenance::ExplicitUnassociated) return;
        const auto legacy=LegacyObservedAc6().model;
        if(provenance!=CalibrationProvenance::LegacyAc6Observation||model.fx!=legacy.fx||model.fy!=legacy.fy||
           model.a!=legacy.a||model.b!=legacy.b||model.wsign!=legacy.wsign)
            throw std::invalid_argument("Invalid projection provenance or altered legacy calibration");
    }
    std::string Json() const {
        Validate();
        std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(std::numeric_limits<float>::max_digits10);
        out<<"{\"provenance\":\""<<(provenance==CalibrationProvenance::LegacyAc6Observation?"ac6_helper_observation_2026_10_16":"explicit_unassociated_calibration")
           <<"\",\"frame_associated\":false,\"same_projection_parallel_eyes\":true,\"metric_scale_calibrated\":false,\"fx\":"<<model.fx
           <<",\"fy\":"<<model.fy<<",\"a\":"<<model.a<<",\"b\":"<<model.b<<",\"wsign\":"<<model.wsign<<"}";
        return out.str();
    }
};
}
