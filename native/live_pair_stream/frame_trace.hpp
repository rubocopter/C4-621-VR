#pragma once
// Private, bounded diagnostic storage. Not a shared-memory/wire contract.
// A resource transition is NOT a draw/write witness or a camera association.
#include "protocol.hpp"
#include "../frame_pair_probe/pair_ledger.hpp"
#include <cstdio>
namespace frame_trace {
using Id=frame_pair::Id;
struct Source {
    Id queue=0,list=0;
    UINT64 submit=0,recording=0,transition=0;
    UINT list_index=0;
    bool valid=false;
};
struct Pair {
    UINT64 sequence=0,tick=0,copy_submit=0;
    unsigned slot=0,backbuffer=0;
    Id depth_resource=0,color_resource=0;
    Source depth,color;
};
class Trace {
public:
    static constexpr unsigned PairCapacity=512,ListCapacity=8192;
    UINT64 submit=0,overflow=0;
    Id queue=0;
    Source depth;
    std::array<Source,3> color{};
    struct List { Id queue=0,list=0;UINT64 submit=0,qpc=0;UINT index=0;frame_pair::Ledger::Summary summary; };
    std::array<List,ListCapacity> lists{};
    std::array<Pair,PairCapacity> pairs{};
    unsigned list_count=0,pair_count=0;
    UINT64 qpc=0;
    // Called under the producer enqueue mutex; recording threads never write here.
    void BeginSubmission(Id incoming,UINT64 now_qpc) {
        if(queue&&queue!=incoming) depth={};
        queue=incoming;++submit;qpc=now_qpc;color={};
    }
    void Observe(Id list,UINT index,const frame_pair::Ledger::Summary& s) {
        if(!s.trusted||s.aliasing) { depth={};color={}; }
        else {
            if(s.depth_touched) depth={queue,list,submit,s.recording,s.depth_transition,index,s.depth_known};
            for(unsigned i=0;i<3;++i) if(s.color_touched&(1u<<i))
                color[i]={queue,list,submit,s.recording,s.color_transition[i],index,(s.color_present&(1u<<i))!=0};
        }
        // Ignore trusted lists with no tracked transitions. They still count in
        // submission ordinals, and are NOT claimed to be free of resource access.
        if(s.trusted&&!s.aliasing&&!s.depth_touched&&!s.color_touched) return;
        if(list_count==ListCapacity) { ++overflow;return; }
        lists[list_count++]={queue,list,submit,qpc,index,s};
    }
    void RetireDepth() { depth={}; }
    void Publish(UINT64 seq,unsigned slot,unsigned backbuffer,Id depth_id,Id color_id,UINT64 tick) {
        if(pair_count==PairCapacity) { ++overflow;return; }
        pairs[pair_count++]={seq,tick,submit,slot,backbuffer,depth_id,color_id,depth,color[backbuffer]};
    }
    // Diagnostic lookup only. Caller must hold the enqueue mutex (or hooks must
    // be disabled). Require the currently published sequence, never slot alone.
    const Pair* Find(UINT64 sequence,unsigned slot,UINT64 now,LONG status,UINT64 published_sequence) const {
        if(!sequence||sequence!=published_sequence||slot>=3||
           (status!=live_pair::Ready&&status!=live_pair::Stopped)) return nullptr;
        for(unsigned i=0;i<pair_count;++i) {
            const auto& p=pairs[i];
            if(p.sequence==sequence&&p.slot==slot&&p.depth.valid&&p.color.valid&&live_pair::Fresh(now,p.tick)) return &p;
        }
        return nullptr;
    }
    static void WriteSource(FILE* log,const char* label,const Source& s) {
        std::fprintf(log,"\"%s\":{\"valid\":%s,\"queue\":\"0x%llx\",\"submit\":%llu,\"list\":\"0x%llx\",\"list_index\":%u,\"recording\":%llu,\"tracked_transition\":%llu}",label,s.valid?"true":"false",static_cast<UINT64>(s.queue),s.submit,static_cast<UINT64>(s.list),s.list_index,s.recording,s.transition);
    }
    void Write(FILE* log) const {
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
        std::fprintf(log,"{\"phase\":\"frame_trace_contract\",\"diagnostic_version\":1,\"qpc_frequency\":%llu,\"submissions\":%llu,\"overflow\":%llu,\"pair_records\":%u,\"list_records\":%u,\"frame_associated\":false,\"draw_binding_observed\":false,\"viewport_observed\":false}\n",frequency.QuadPart,submit,overflow,pair_count,list_count);
        for(unsigned i=0;i<list_count;++i) {
            const auto& e=lists[i];const auto& s=e.summary;
            std::fprintf(log,"{\"phase\":\"frame_trace_list\",\"queue\":\"0x%llx\",\"submit\":%llu,\"qpc\":%llu,\"list\":\"0x%llx\",\"list_index\":%u,\"recording\":%llu,\"trusted\":%s,\"aliasing\":%s,\"depth_touched\":%s,\"depth_known\":%s,\"depth_state\":%u,\"depth_transition\":%llu,\"color_touched\":%u,\"color_present\":%u,\"color_transitions\":[%llu,%llu,%llu]}\n",static_cast<UINT64>(e.queue),e.submit,e.qpc,static_cast<UINT64>(e.list),e.index,s.recording,s.trusted?"true":"false",s.aliasing?"true":"false",s.depth_touched?"true":"false",s.depth_known?"true":"false",s.depth_state,s.depth_transition,s.color_touched,s.color_present,s.color_transition[0],s.color_transition[1],s.color_transition[2]);
        }
        for(unsigned i=0;i<pair_count;++i) {
            const auto& p=pairs[i];
            std::fprintf(log,"{\"phase\":\"frame_trace_pair\",\"sequence\":%llu,\"slot\":%u,\"tick\":%llu,\"copy_submit\":%llu,\"backbuffer\":%u,\"depth_resource\":\"0x%llx\",\"color_resource\":\"0x%llx\",\"frame_associated\":false,",p.sequence,p.slot,p.tick,p.copy_submit,p.backbuffer,static_cast<UINT64>(p.depth_resource),static_cast<UINT64>(p.color_resource));
            WriteSource(log,"depth_transition",p.depth);std::fputc(',',log);WriteSource(log,"color_transition",p.color);std::fputs("}\n",log);
        }
        if(std::ferror(log)) throw std::runtime_error("Write frame diagnostics failed");
    }
};
}
