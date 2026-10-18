#pragma once
#include <string>
namespace live_pair {
inline std::string JsonModuleName(const std::wstring& name) {
    // Windows module filenames are UTF-16. ASCII JSON escapes preserve those
    // code units without depending on the process-wide CRT locale.
    constexpr char hex[]="0123456789abcdef";
    std::string result="\"";
    for(wchar_t character:name) {
        const auto unit=static_cast<unsigned short>(character);
        if(unit>=0x20&&unit<0x7f&&unit!='"'&&unit!='\\') result+=static_cast<char>(unit);
        else {
            result+="\\u";
            for(int shift=12;shift>=0;shift-=4) result+=hex[(unit>>shift)&15];
        }
    }
    result+='"';
    return result;
}
}
