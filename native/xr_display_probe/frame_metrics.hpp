#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>
// CPU elapsed milliseconds, including GPU/runtime waits; not GPU timestamps.
class FrameMetric {
    std::vector<double> samples;
    std::uint64_t count=0;
    double sum=0,maximum=0;
public:
    struct Result { std::uint64_t count=0,sampled=0;double mean=0,p50=0,p95=0,maximum=0; };
    void Add(double milliseconds) {
        if(!std::isfinite(milliseconds)||milliseconds<0) throw std::invalid_argument("Invalid frame timing");
        ++count;sum+=milliseconds;maximum=std::max(maximum,milliseconds);
        if(samples.size()<32768) samples.push_back(milliseconds);
    }
    Result Summary() const {
        if(!count) return {};
        auto sorted=samples;std::sort(sorted.begin(),sorted.end());
        auto percentile=[&](double fraction) { return sorted[static_cast<size_t>(std::ceil(fraction*sorted.size()))-1]; };
        return {count,sorted.size(),sum/count,percentile(.5),percentile(.95),maximum};
    }
};
