#include "stereo_frame.hpp"
#include "capture_health.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
static void Require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
int main() try {
    for(int failEye:{-1,0,1}) {
        std::vector<int> acquired,released;int ends=0,validated=0;bool submitted=true;
        const bool shown=PresentStereoFrame(true,[&](unsigned eye){acquired.push_back(eye);},
            [&](unsigned eye){return int(eye)!=failEye;},[&](unsigned eye){released.push_back(eye);},
            [&]{++validated;return true;},
            [&](bool ready){++ends;submitted=ready;});
        Require(shown==(failEye<0)&&submitted==shown&&ends==1,"An expired eye must end the frame with zero layers");
        Require(acquired==released&&acquired.size()==(failEye==0?1u:2u),"Every waited image must be released even when the source expires");
        Require(validated==(failEye<0?1:0),"Final validation only follows a complete pair");
    }
    std::vector<int> released;int ends=0;bool submitted=true,threw=false;
    try {
        PresentStereoFrame(true,[](unsigned){},[](unsigned)->bool{throw std::runtime_error("copy failure");},
            [&](unsigned eye){released.push_back(eye);},[]{return true;},[&](bool ready){++ends;submitted=ready;});
    } catch(const std::runtime_error&) { threw=true; }
    Require(threw&&released==std::vector<int>{0}&&ends==1&&!submitted,"Render errors must release the waited image and close the frame before propagating");
    PresentStereoFrame(false,[](unsigned){throw std::runtime_error("hidden acquire");},[](unsigned){return true;},[](unsigned){},
        []()->bool{throw std::runtime_error("hidden validation");},[&](bool ready){Require(!ready,"Hidden frame must have zero layers");});
    // Simulate expiry after the right-eye freshness check, without a real
    // clock, GPU stall or runtime. Both copies themselves report success.
    for(bool duringRelease:{false,true}) for(std::uint64_t finalAge:{500u,501u}) {
        std::uint64_t now=1000;const std::uint64_t capturedAt=501;
        std::vector<unsigned> waited,returned;bool submitted=true;int ends=0;
        const bool shown=PresentStereoFrame(CaptureFresh(now,capturedAt,true),
            [&](unsigned eye){waited.push_back(eye);},
            [&](unsigned eye){if(eye==1&&!duringRelease) now=capturedAt+finalAge;return true;},
            [&](unsigned eye){returned.push_back(eye);if(eye==1&&duringRelease) now=capturedAt+finalAge;},
            [&]{Require(returned==std::vector<unsigned>{0,1},"Validate after both releases");return CaptureFresh(now,capturedAt,true);},
            [&](bool ready){submitted=ready;++ends;});
        Require(shown==(finalAge==500)&&submitted==shown&&ends==1,"Pair expired during final copy/release was submitted, or boundary changed");
        Require(waited==std::vector<unsigned>{0,1}&&returned==waited,"Late expiry must release both waited eyes");
    }
    // A failed producer/source check after both copies also discards the pair.
    bool sourceValid=true;int validations=0;ends=0;submitted=true;
    const bool shown=PresentStereoFrame(true,[](unsigned){},[](unsigned){return true;},
        [&](unsigned eye){if(eye==1) sourceValid=false;},[&]{++validations;return sourceValid;},
        [&](bool ready){++ends;submitted=ready;});
    Require(!shown&&!submitted&&ends==1&&validations==1,"Invalid source after release must discard the whole pair");
    released.clear();ends=0;submitted=true;threw=false;
    try {
        PresentStereoFrame(true,[](unsigned){},[](unsigned){return true;},[&](unsigned eye){released.push_back(eye);},
            []()->bool{throw std::runtime_error("source validation failure");},[&](bool ready){++ends;submitted=ready;});
    } catch(const std::runtime_error&) { threw=true; }
    Require(threw&&released==std::vector<int>{0,1}&&ends==1&&!submitted,"Validation errors must close with zero layers after both releases");
    std::cout<<"PASS: stereo pair expiry, release order and frame closure\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
