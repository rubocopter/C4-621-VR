#include "../reprojection/projection.hpp"
#include <iostream>
#include <limits>
#include <functional>
using namespace reprojection;
static void Require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
static void Reject(const std::function<void()>& action) {
    bool rejected=false;try { action(); } catch(const std::invalid_argument&) { rejected=true; }
    Require(rejected,"Invalid projection accepted");
}
int main() try {
    const RayIntrinsics source{2,4,.25f,-.125f,1};
    ParallelEyeProjections eyes{source,{RayIntrinsics{1,2,-.125f,.125f,1},RayIntrinsics{4,8,.25f,0,1}}};
    ProjectionCalibration explicitEyes{{2,4,-1,2,1},CalibrationProvenance::ExplicitUnassociated,eyes};
    explicitEyes.Validate();
    Require(explicitEyes.Json().find("\"same_projection_parallel_eyes\":false")!=std::string::npos,"Distinct eye metadata must not claim identical projections");
    const auto left=eyes.Transform(0),right=eyes.Transform(1);
    Require(left.scaleX==.5f&&left.scaleY==.5f&&left.offsetX==-.25f&&left.offsetY==.1875f,"Left ray transform");
    Require(right.scaleX==2&&right.scaleY==2&&right.offsetX==-.25f&&right.offsetY==.25f,"Right ray transform");
    Reject([&]{eyes.Transform(2);});
    auto inconsistent=explicitEyes;inconsistent.eyeProjections->source.fx=3;Reject([&]{inconsistent.Validate();});
    inconsistent=explicitEyes;inconsistent.eyeProjections->target[1].wsign=-1;Reject([&]{inconsistent.Validate();});
    auto mislabeled=ProjectionCalibration::LegacyObservedAc6();mislabeled.eyeProjections=eyes;Reject([&]{mislabeled.Validate();});
    for(float bad:{0.f,-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::denorm_min()}) {
        auto invalid=eyes;invalid.target[0].fx=bad;Reject([&]{invalid.Transform(0);});
    }
    auto overflow=eyes;overflow.source.fx=std::numeric_limits<float>::min();overflow.target[0].fx=std::numeric_limits<float>::max();Reject([&]{overflow.Transform(0);});
    // Finite valid intrinsics can overflow at raster time. GPU must reject
    // the nonfinite coordinate before converting it into any buffer index.
    ParallelEyeProjections extreme{{1,1,0,0,1},{RayIntrinsics{1,1e38f,0,-1e38f,1},RayIntrinsics{}}};
    extreme.Validate(Perspective{1,1,-1,2,1});
    auto identity=eyes;identity.target={source,source};Require(identity.Transform(0).Identity()&&identity.Transform(1).Identity(),"Identity rays must preserve legacy path");
    auto sameEyes=explicitEyes;sameEyes.eyeProjections=identity;
    Require(sameEyes.Json().find("\"same_projection_parallel_eyes\":true")!=std::string::npos,"Identical eye metadata");
    for(float sign:{1.f,-1.f}) {
        std::array<float,16> m{2,0,0,0,0,4,0,0,.25f*sign,-.125f*sign,-sign,sign,0,0,2,0};
        auto rays=RayIntrinsics::FromRowMatrix(m);
        Require(rays.cx==.25f&&rays.cy==-.125f&&rays.wsign==sign,"Principal point must be normalized by W sign");
    }
    for(float sign:{1.f,-1.f}) for(bool reverse:{false,true}) {
        const Perspective p{1,2,sign*(reverse?-1.f:2.f),reverse?2.f:-2.f,sign};
        p.Validate();
        Require(p.ForwardDistance(reverse?1.f:0.f)==1,"Near distance/sign");
        Require(p.ForwardDistance(reverse?0.f:1.f)==2,"Far distance/sign");
        Require(p.ForwardDistance(0.5f)==4.f/3.f,"Interior depth inversion");
        std::array<float,16> m{1,0,0,0,0,2,0,0,0.125f,-0.25f,p.a,sign,0,0,p.b,0};
        auto extracted=Perspective::FromRowMatrix(m);
        Require(extracted.ForwardDistance(0.5f)==4.f/3.f,"Matrix convention/jitter extraction");
        for(unsigned index:{1u,2u,3u,4u,6u,7u,12u,13u,15u}) {
            auto bad=m;bad[index]=0.001f;Reject([&]{Perspective::FromRowMatrix(bad);});
        }
    }
    Perspective infinite{1,1,0,1,1};infinite.Validate();
    Require(std::isinf(infinite.ForwardDistance(0)),"Infinite far endpoint must remain a direction");
    Require(infinite.ForwardDistance(0.5f)==2,"Infinite far interior");
    for(Perspective bad: {Perspective{1,1,.5f,1,1},Perspective{1,1,2,1,1},Perspective{1,1,-1,-1,1},
                         Perspective{0,1,-1,2,1},Perspective{1,-1,-1,2,1},Perspective{1,1,-1,0,1},
                         Perspective{1,1,-1,2,0},Perspective{1,1,-1,2,1.01f}}) Reject([&]{bad.Validate();});
    // SM5 flushes subnormal arithmetic to zero: a CPU-positive endpoint
    // distance can disappear in GPU even with zero baseline.
    for(float sign:{1.f,-1.f}) {
        Perspective tiny{1,1,-sign,std::numeric_limits<float>::min(),sign};
        Reject([&]{tiny.Validate();});
    }
    for(unsigned field=0;field<4;++field) {
        Perspective tiny{1,1,-1,2,1};const auto denorm=std::numeric_limits<float>::denorm_min();
        switch(field) { case 0:tiny.fx=denorm;break;case 1:tiny.fy=denorm;break;case 2:tiny.a=-denorm;break;default:tiny.b=denorm; }
        Reject([&]{tiny.Validate();});
    }
    for(unsigned field=0;field<5;++field) for(float value:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        Perspective bad{1,1,-1,2,1};
        switch(field) { case 0:bad.fx=value;break;case 1:bad.fy=value;break;case 2:bad.a=value;break;case 3:bad.b=value;break;default:bad.wsign=value; }
        Reject([&]{bad.Validate();});
    }
    auto calibration=ProjectionCalibration::LegacyObservedAc6();calibration.Validate();
    Require(calibration.model.ForwardDistance(1)>0,"Legacy model");
    calibration.model.fx=1;Reject([&]{calibration.Validate();}); // Cannot mislabel a different model as the observation.
    calibration=ProjectionCalibration::LegacyObservedAc6();
    calibration.provenance=static_cast<CalibrationProvenance>(999);Reject([&]{calibration.Validate();});
    std::cout<<"PASS projection contract: endpoints, conventions, matrix shape, poles, finite coefficients, provenance\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
