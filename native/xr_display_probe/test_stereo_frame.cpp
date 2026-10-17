#include "stereo_frame.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
static void Require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
int main() try {
    for(int failEye:{-1,0,1}) {
        std::vector<int> acquired,released;int ends=0;bool submitted=true;
        const bool shown=PresentStereoFrame(true,[&](unsigned eye){acquired.push_back(eye);},
            [&](unsigned eye){return int(eye)!=failEye;},[&](unsigned eye){released.push_back(eye);},
            [&](bool ready){++ends;submitted=ready;});
        Require(shown==(failEye<0)&&submitted==shown&&ends==1,"An expired eye must end the frame with zero layers");
        Require(acquired==released&&acquired.size()==(failEye==0?1u:2u),"Every waited image must be released even when the source expires");
    }
    std::vector<int> released;int ends=0;bool submitted=true,threw=false;
    try {
        PresentStereoFrame(true,[](unsigned){},[](unsigned)->bool{throw std::runtime_error("copy failure");},
            [&](unsigned eye){released.push_back(eye);},[&](bool ready){++ends;submitted=ready;});
    } catch(const std::runtime_error&) { threw=true; }
    Require(threw&&released==std::vector<int>{0}&&ends==1&&!submitted,"Render errors must release the waited image and close the frame before propagating");
    PresentStereoFrame(false,[](unsigned){throw std::runtime_error("hidden acquire");},[](unsigned){return true;},[](unsigned){},[&](bool ready){Require(!ready,"Hidden frame must have zero layers");});
    std::cout<<"PASS: stereo pair expiry, release order and frame closure\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
