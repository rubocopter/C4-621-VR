#pragma once
#include "head_rotation.hpp"
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

struct PublishedPose {
    std::uint32_t magic,version,valid,sequence;
    std::uint64_t tick;
    float delta[9];
};
static_assert(offsetof(PublishedPose,delta)==24 && sizeof(PublishedPose)==64);

class PosePublisher {
    HANDLE mapping=nullptr,mutex=nullptr;
    void* memory=nullptr;
    Quaternion reference{};
    bool centered=false;
    std::uint32_t sequence=0;
    void Write(bool valid,const std::array<float,9>& delta) {
        const DWORD waited=WaitForSingleObject(mutex,0);
        if(waited!=WAIT_OBJECT_0 && waited!=WAIT_ABANDONED) return;
        PublishedPose packet{0x36565241,1,valid?1u:0u,++sequence,GetTickCount64(),{}};
        std::memcpy(packet.delta,delta.data(),sizeof(packet.delta));
        std::memcpy(memory,&packet,sizeof(packet));
        ReleaseMutex(mutex);
    }
public:
    explicit PosePublisher(DWORD pid) {
        const auto suffix=std::to_wstring(pid);
        mutex=CreateMutexW(nullptr,FALSE,(L"Local\\AC6VRPoseMutex-"+suffix).c_str());
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,128,(L"Local\\AC6VRPose-"+suffix).c_str());
        if(mapping) memory=MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,128);
        if(!mutex || !memory) {
            if(memory) UnmapViewOfFile(memory);
            if(mapping) CloseHandle(mapping);
            if(mutex) CloseHandle(mutex);
            throw std::runtime_error("Cannot create pose transport");
        }
        Invalidate();
    }
    void Invalidate() { Write(false,{1,0,0,0,1,0,0,0,1}); }
    void Update(Quaternion current,bool valid) {
        if(!valid) { Invalidate(); return; }
        // Validate before using the first sample as the reference.
        RelativeRows(current,current);
        if(!centered) { reference=current; centered=true; }
        Write(true,CameraRows(reference,current));
    }
    ~PosePublisher() {
        Invalidate(); UnmapViewOfFile(memory); CloseHandle(mapping); CloseHandle(mutex);
    }
};
