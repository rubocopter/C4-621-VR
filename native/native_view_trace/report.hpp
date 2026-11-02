#pragma once
#include "trace.hpp"
#include <cstdio>
namespace view_trace {
struct Report {
    U image_base=0;
    DWORD pid=0;
    Identity identity{};
    const char* status="not_started";
    const char* disable_status="not_started";
    std::array<bool,2> restored{};
    unsigned pending_at_shutdown=0;
    bool allow_events=false;
};
// Worker/host fixtures only, never called from either native callback.
bool write_report(FILE* output,const Report& report) noexcept;
}
