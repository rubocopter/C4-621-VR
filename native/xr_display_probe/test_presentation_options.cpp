#include "presentation_options.hpp"
#include "frame_metrics.hpp"
#include <iostream>
static void Require(bool condition) { if(!condition) throw std::runtime_error("Presentation settings/metrics regression"); }
int main() try {
    PresentationOptions options;options.Validate(true,false);
    options.eye_width=1920;options.eye_height=1080;options.rotation_limit=90;options.Validate(true,true);
    bool rejected=false;try { options.Validate(false,true); } catch(const std::invalid_argument&) { rejected=true; }Require(rejected);
    rejected=false;try { options.Validate(true,false); } catch(const std::invalid_argument&) { rejected=true; }Require(rejected);
    options.eye_height=720;rejected=false;try { options.Validate(true,true); } catch(const std::invalid_argument&) { rejected=true; }Require(rejected);
    Require(ParseEyeDimension(L"1920")==1920&&ParseRotationLimit(L"90")==90);
    for(const auto* text:{L"-1",L"1920x",L"4294969216",L""}) { rejected=false;try { ParseEyeDimension(text); } catch(const std::exception&) { rejected=true; }Require(rejected); }
    for(const auto* text:{L"nan",L"inf",L"0",L"181",L"90junk"}) { rejected=false;try { ParseRotationLimit(text); } catch(const std::exception&) { rejected=true; }Require(rejected); }
    FrameMetric metric;Require(metric.Summary().count==0);
    for(double x:{1.,2.,3.,100.}) metric.Add(x);
    const auto summary=metric.Summary();Require(summary.count==4&&summary.mean==26.5&&summary.p50==2&&summary.p95==100&&summary.maximum==100);
    rejected=false;try { metric.Add(-1); } catch(const std::invalid_argument&) { rejected=true; }Require(rejected);
    std::cout<<"Presentation options and frame metrics passed\n";return 0;
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
