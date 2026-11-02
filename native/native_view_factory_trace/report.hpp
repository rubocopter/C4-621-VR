#pragma once
#include "trace.hpp"
#include <cstdio>
namespace factory_trace {
struct Report {
    U image_base=0,process_creation=0;
    std::uint64_t closed_tick_ms=0;
    DWORD pid=0;
    const char* status="host_rejected";
    const char* disable_status="MH_OK";
    bool restored=false;
    unsigned pending_at_shutdown=0;
};
bool write_report(FILE* file,const Report& report) noexcept;
}
