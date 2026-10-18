#pragma once
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <optional>

namespace reprojection {
// Row-vector perspective: clip=(fx*x+jx*z, fy*y+jy*z, a*z+b, wsign*z).
// Device depth is [0,1]. ForwardDistance is w=wsign*z, in game units.
// This reduced depth model omits jx/jy. Identical parallel projections cancel
// them; explicit destination intrinsics must supply them through RayIntrinsics.
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
// Image intrinsics in the source view axes: NDC=(fx*X/w+cx,fy*Y/w+cy).
// These describe rays only, not destination near/far clipping or eye rotation.
struct RayIntrinsics {
    float fx=1,fy=1,cx=0,cy=0,wsign=1;
    // Signed ray slopes X/forwardDistance and Y/forwardDistance at the edges.
    // Not radians, clip distances, view poses, or a source-frame association.
    // The caller must express both eyes in the same parallel view axes.
    static RayIntrinsics FromFrustumTangents(double left,double right,double down,double up,float wsign) {
        for(double edge:{left,right,down,up})
            if(!std::isfinite(edge)) throw std::invalid_argument("Nonfinite frustum edge");
        const double width=right-left,height=up-down;
        if(!std::isfinite(width)||!std::isfinite(height)||width<=0||height<=0)
            throw std::invalid_argument("Frustum edges must be ordered with finite positive spans");
        const double fx=2/width,fy=2/height,cx=-(right+left)/width,cy=-(up+down)/height;
        for(double value:{fx,fy,cx,cy})
            if(!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max()||
               (value!=0&&std::abs(value)<std::numeric_limits<float>::min()))
                throw std::invalid_argument("Frustum intrinsics outside normal float32 range");
        RayIntrinsics result{float(fx),float(fy),float(cx),float(cy),wsign};
        result.Validate();
        return result;
    }
    // Signed radians measured from the forward ray, in the same view axes as
    // FromFrustumTangents. A FOV carries no relative eye pose or metric scale.
    static RayIntrinsics FromFovAngles(double left,double right,double down,double up,float wsign) {
        constexpr double halfPi=1.57079632679489661923;
        for(double angle:{left,right,down,up})
            if(!std::isfinite(angle)||angle<=-halfPi||angle>=halfPi)
                throw std::invalid_argument("FOV radians must lie strictly between -pi/2 and pi/2");
        return FromFrustumTangents(std::tan(left),std::tan(right),std::tan(down),std::tan(up),wsign);
    }
    void Validate() const {
        if(!std::isnormal(fx)||fx<=0||!std::isnormal(fy)||fy<=0||
           !std::isfinite(cx)||!std::isfinite(cy)||(wsign!=1&&wsign!=-1)||
           std::fpclassify(cx)==FP_SUBNORMAL||std::fpclassify(cy)==FP_SUBNORMAL)
            throw std::invalid_argument("Invalid parallel image intrinsics");
    }
    static RayIntrinsics FromRowMatrix(const std::array<float,16>& matrix) {
        const auto p=Perspective::FromRowMatrix(matrix);
        RayIntrinsics rays{p.fx,p.fy,matrix[8]/p.wsign,matrix[9]/p.wsign,p.wsign};
        rays.Validate();return rays;
    }
    void WriteJson(std::ostream& out) const {
        out<<"{\"fx\":"<<fx<<",\"fy\":"<<fy<<",\"cx_ndc\":"<<cx<<",\"cy_ndc\":"<<cy<<",\"wsign\":"<<wsign<<"}";
    }
    bool Same(const RayIntrinsics& other) const {
        return fx==other.fx&&fy==other.fy&&cx==other.cx&&cy==other.cy&&wsign==other.wsign;
    }
};
struct PixelTransform {
    float scaleX=1,scaleY=1,offsetX=0,offsetY=0;
    bool Identity() const { return scaleX==1&&scaleY==1&&offsetX==0&&offsetY==0; }
    void Validate() const {
        if(!std::isnormal(scaleX)||scaleX<=0||!std::isnormal(scaleY)||scaleY<=0||
           !std::isfinite(offsetX)||!std::isfinite(offsetY)||
           std::fpclassify(offsetX)==FP_SUBNORMAL||std::fpclassify(offsetY)==FP_SUBNORMAL)
            throw std::invalid_argument("Image intrinsics transform outside supported float32 range");
    }
};
struct ParallelEyeProjections {
    RayIntrinsics source;
    std::array<RayIntrinsics,2> target;
    PixelTransform Transform(unsigned eye) const {
        if(eye>1) throw std::invalid_argument("Eye must be 0 or 1");
        source.Validate();target[eye].Validate();
        if(source.wsign!=target[eye].wsign) throw std::invalid_argument("Parallel image intrinsics must share view axes/W sign");
        const double sx=double(target[eye].fx)/source.fx,sy=double(target[eye].fy)/source.fy;
        PixelTransform result{float(sx),float(sy),float(target[eye].cx-sx*source.cx),float(target[eye].cy-sy*source.cy)};
        result.Validate();return result;
    }
    void Validate(const Perspective& model) const {
        if(source.fx!=model.fx||source.fy!=model.fy||source.wsign!=model.wsign)
            throw std::invalid_argument("Image source intrinsics disagree with depth calibration");
        Transform(0);Transform(1);
    }
};
enum class CalibrationProvenance { LegacyAc6Observation, ExplicitUnassociated };
// Static consumer configuration, NOT per-frame metadata and NOT a wire format.
struct ProjectionCalibration {
    Perspective model;
    CalibrationProvenance provenance;
    std::optional<ParallelEyeProjections> eyeProjections;
    static ProjectionCalibration LegacyObservedAc6() {
        // PROJECTION_FINDINGS.md: helper observations on 2026-10-16, separate
        // from the color/depth interval. No claim these match a current frame.
        return {{0.572404087f,1.01760733f,-2.02655792e-6f,1.00000203f,1},CalibrationProvenance::LegacyAc6Observation};
    }
    void Validate() const {
        model.Validate();
        if(eyeProjections) {
            eyeProjections->Validate(model);
            if(provenance!=CalibrationProvenance::ExplicitUnassociated)
                throw std::invalid_argument("Distinct eye intrinsics require an explicit unassociated source calibration");
        }
        if(provenance==CalibrationProvenance::ExplicitUnassociated) return;
        const auto legacy=LegacyObservedAc6().model;
        if(provenance!=CalibrationProvenance::LegacyAc6Observation||model.fx!=legacy.fx||model.fy!=legacy.fy||
           model.a!=legacy.a||model.b!=legacy.b||model.wsign!=legacy.wsign)
            throw std::invalid_argument("Invalid projection provenance or altered legacy calibration");
    }
    PixelTransform Transform(unsigned eye) const {
        if(eye>1) throw std::invalid_argument("Eye must be 0 or 1");
        return eyeProjections?eyeProjections->Transform(eye):PixelTransform{};
    }
    std::string Json() const {
        Validate();
        std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(std::numeric_limits<float>::max_digits10);
        out<<"{\"provenance\":\""<<(provenance==CalibrationProvenance::LegacyAc6Observation?"ac6_helper_observation_2026_10_16":"explicit_unassociated_calibration")
           <<"\",\"frame_associated\":false,\"same_projection_parallel_eyes\":"<<(!eyeProjections||(eyeProjections->source.Same(eyeProjections->target[0])&&eyeProjections->source.Same(eyeProjections->target[1]))?"true":"false")<<",\"metric_scale_calibrated\":false,\"fx\":"<<model.fx
           <<",\"fy\":"<<model.fy<<",\"a\":"<<model.a<<",\"b\":"<<model.b<<",\"wsign\":"<<model.wsign;
        if(eyeProjections) {
            out<<",\"image_intrinsics\":{\"source\":";eyeProjections->source.WriteJson(out);
            out<<",\"left\":";eyeProjections->target[0].WriteJson(out);
            out<<",\"right\":";eyeProjections->target[1].WriteJson(out);out<<"}";
        }
        out<<"}";
        return out.str();
    }
};
}
