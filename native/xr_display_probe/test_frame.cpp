#include "frame.hpp"
#include "capture_health.hpp"
#include <iostream>
#include <stdexcept>

static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
int main() {
    try {
        // Two pixels and row padding. Alpha from capture is deliberately zero.
        const std::uint8_t source[] = {30,20,10,0, 60,50,40,0, 99,99,99,99};
        auto result = FitWindowFrame(source, 2, 1, 12, 4, 4);
        Check(result.size() == 64, "Wrong output dimensions");
        for (unsigned y=0; y<4; ++y) for (unsigned x=0; x<4; ++x) {
            const auto* p = result.data() + 4 * (y*4+x);
            Check(p[3] == 255, "Output must be opaque");
            if (y==0 || y==3) Check(p[0]==0 && p[1]==0 && p[2]==0, "Letterbox must be black");
            else Check(p[0]==(x<2?10:40) && p[1]==(x<2?20:50) && p[2]==(x<2?30:60), "Channels or aspect fit incorrect");
        }
        const std::uint8_t padded[] = {3,2,1,0, 77,77,77,77, 6,5,4,0, 88,88,88,88};
        result = FitWindowFrame(padded,1,2,8,1,2);
        Check(result[0]==1 && result[4]==4, "Input stride ignored");
        bool rejected=false;
        try { FitWindowFrame(source,2,1,4,4,4); } catch(const std::invalid_argument&) { rejected=true; }
        Check(rejected,"Undersized input row must be rejected");
        Check(!CaptureFresh(500,0,false),"No image must not count as fresh");
        Check(CaptureFresh(1000,500,true),"A 500 ms image should remain within the limit");
        Check(!CaptureFresh(1001,500,true),"Stale image must be rejected");
        Check(!CaptureFresh(499,500,true),"Future timestamp must be rejected");
        Check(HeadTrackingReady(true,true,true,true,true),"Visible tracked head pose must not require controller input focus");
        Check(!HeadTrackingReady(false,true,true,true,true),"Hidden session must not control the game camera");
        Check(!HeadTrackingReady(true,false,true,true,true),"Non-rendering session must not control the game camera");
        Check(!HeadTrackingReady(true,true,false,true,true),"Stale image must stop camera tracking");
        Check(!HeadTrackingReady(true,true,true,false,true),"Invalid orientation must stop camera tracking");
        Check(!HeadTrackingReady(true,true,true,true,false),"Untracked orientation must stop camera tracking");
        std::cout << "Window frame tests passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
