#pragma once
#include <cmath>
#include <stdexcept>
#include <string>
struct PresentationOptions {
    unsigned eye_width=1280,eye_height=720;
    float rotation_limit=25;
    void Validate(bool paired,bool tracking) const {
        if(!((eye_width==1280&&eye_height==720)||(eye_width==1920&&eye_height==1080)||(eye_width==2560&&eye_height==1440)))
            throw std::invalid_argument("Eye size must be 1280x720, 1920x1080 or 2560x1440");
        if(!paired&&(eye_width!=1280||eye_height!=720)) throw std::invalid_argument("Custom eye resolution requires the paired GPU stream");
        if(!std::isfinite(rotation_limit)||rotation_limit<1||rotation_limit>180||(!tracking&&rotation_limit!=25))
            throw std::invalid_argument("Rotation limit requires head tracking and must be 1..180 degrees");
    }
};
inline unsigned ParseEyeDimension(const std::wstring& value) {
    if(value.empty()||value.find_first_not_of(L"0123456789")!=std::wstring::npos) throw std::invalid_argument("Invalid eye dimension");
    const auto result=std::stoul(value);
    if(result>4096) throw std::invalid_argument("Eye dimension out of range");
    return static_cast<unsigned>(result);
}
inline float ParseRotationLimit(const std::wstring& value) {
    size_t used=0;const float result=std::stof(value,&used);
    if(used!=value.size()||!std::isfinite(result)||result<1||result>180) throw std::invalid_argument("Rotation limit must be 1..180 degrees");
    return result;
}
