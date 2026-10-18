#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>

namespace frame_trace {
struct ForwardingAddressRange {
    std::uintptr_t begin=0,end=0; // exclusive end
    bool Contains(std::uintptr_t address,std::uintptr_t bytes) const noexcept {
        return begin&&begin<end&&address>=begin&&address<end&&bytes<=end-address;
    }
};
// Pinned adapter type and immutable image ranges, prepared before hooks run.
struct ContextForwardingLayout {
    std::uintptr_t adapter_vtable=0,adapter_slot1=0,adapter_slot5=0;
    ForwardingAddressRange tables{},code{};
    std::array<ForwardingAddressRange,96> code_sections{};
    unsigned code_section_count=0;
    bool Enabled() const noexcept { return adapter_vtable!=0; }
    bool ContainsCode(std::uintptr_t address) const noexcept {
        if(code_section_count>code_sections.size()) return false;
        if(code.Contains(address,1)) return true;
        for(unsigned i=0;i<code_section_count;++i)
            if(code_sections[i].Contains(address,1)) return true;
        return false;
    }
};
enum class ForwardingStatus { NotConfigured, Candidate, AdapterMismatch, NullPayload,
                              NullChild, InvalidChildVtable, InvalidTarget, Unreadable };
struct ContextForwardingSnapshot {
    ForwardingStatus status=ForwardingStatus::NotConfigured;
    std::uintptr_t parent=0,payload=0,child=0,child_vtable=0,child_slot8_target=0;
};
inline const char* ForwardingStatusName(ForwardingStatus status) noexcept {
    switch(status) {
    case ForwardingStatus::Candidate:return "candidate";
    case ForwardingStatus::AdapterMismatch:return "adapter_mismatch";
    case ForwardingStatus::NullPayload:return "null_payload";
    case ForwardingStatus::NullChild:return "null_child";
    case ForwardingStatus::InvalidChildVtable:return "invalid_child_vtable";
    case ForwardingStatus::InvalidTarget:return "invalid_target";
    case ForwardingStatus::Unreadable:return "unreadable";
    default:return "not_configured";
    }
}
// The caller is the currently active CPU invocation, BEFORE its original call.
// No calls to candidates, allocations, I/O or OS queries. Stage all fields; a
// rejected or faulting read publishes only a reason, never partial pointers.
// This is an entry snapshot, NOT an observed dispatch or stable GPU identity.
template<class Identity>
ContextForwardingSnapshot ReadContextForwarding(const Identity& token,const ContextForwardingLayout& layout) noexcept {
    ContextForwardingSnapshot result;
    if(!layout.Enabled()) return result;
    result.status=ForwardingStatus::AdapterMismatch;
    if(token.context<0x80||token.context%8||token.vtable!=layout.adapter_vtable||
       token.slot1_target!=layout.adapter_slot1||token.slot5_target!=layout.adapter_slot5||
       !layout.tables.Contains(token.vtable,6*sizeof(std::uintptr_t))||
       !layout.ContainsCode(token.slot1_target)||!layout.ContainsCode(token.slot5_target)) return result;
    __try {
        // Revalidate the observed type before following its pinned layout.
        if(*reinterpret_cast<const std::uintptr_t*>(token.context)!=layout.adapter_vtable) return result;
        const auto* adapter=reinterpret_cast<const std::uintptr_t*>(token.vtable);
        if(adapter[1]!=layout.adapter_slot1||adapter[5]!=layout.adapter_slot5) return result;
        const auto parent=token.context-0x80;
        const auto payload=*reinterpret_cast<const std::uintptr_t*>(token.context+8);
        if(!payload) { result.status=ForwardingStatus::NullPayload;return result; }
        const auto child=*reinterpret_cast<const std::uintptr_t*>(parent+0x60);
        if(!child) { result.status=ForwardingStatus::NullChild;return result; }
        result.status=ForwardingStatus::Unreadable;
        if(child%8) return result;
        const auto table=*reinterpret_cast<const std::uintptr_t*>(child);
        result.status=ForwardingStatus::InvalidChildVtable;
        if(table%8||!layout.tables.Contains(table,9*sizeof(std::uintptr_t))) return result;
        const auto target=reinterpret_cast<const std::uintptr_t*>(table)[8];
        result.status=ForwardingStatus::InvalidTarget;
        if(!layout.ContainsCode(target)) return result;
        result={ForwardingStatus::Candidate,parent,payload,child,table,target};
        return result;
    } __except(EXCEPTION_EXECUTE_HANDLER) { result.status=ForwardingStatus::Unreadable;return result; }
}
// Control-thread-only PE bounds. This does not substitute for the loader's
// executable hash/coop checks. Never called in a callback or for another module.
inline bool LoadGameForwardingLayout(HMODULE module,ContextForwardingLayout& result) noexcept {
    __try {
        if(!module) return false;
        const auto base=reinterpret_cast<std::uintptr_t>(module);
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>0x100000) return false;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||
           nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||!nt->FileHeader.NumberOfSections||
           nt->FileHeader.NumberOfSections>96) return false;
        ContextForwardingLayout layout;
        layout.adapter_vtable=base+0x4098e38;
        layout.adapter_slot1=base+0x2b9d140;
        layout.adapter_slot5=base+0x2b9db10;
        const auto size=nt->OptionalHeader.SizeOfImage;
        const auto* section=IMAGE_FIRST_SECTION(nt);
        for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
            const auto rva=section[i].VirtualAddress,bytes=section[i].Misc.VirtualSize;
            if(!bytes||rva>=size||bytes>size-rva) return false;
            if(std::memcmp(section[i].Name,".rdata\0\0",8)==0&&
               (section[i].Characteristics&IMAGE_SCN_MEM_READ)&&!(section[i].Characteristics&(IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)))
                if(ForwardingAddressRange{base+rva,base+rva+bytes}.Contains(layout.adapter_vtable,6*sizeof(std::uintptr_t)))
                    layout.tables={base+rva,base+rva+bytes};
            if(std::memcmp(section[i].Name,".text\0\0\0",8)==0&&
               (section[i].Characteristics&IMAGE_SCN_MEM_EXECUTE)&&!(section[i].Characteristics&IMAGE_SCN_MEM_WRITE))
            {
                const ForwardingAddressRange range{base+rva,base+rva+bytes};
                layout.code_sections[layout.code_section_count++]=range;
                if(range.Contains(layout.adapter_slot1,1)&&range.Contains(layout.adapter_slot5,1)) layout.code=range;
            }
        }
        if(!layout.tables.Contains(layout.adapter_vtable,6*sizeof(std::uintptr_t))||
           !layout.code.Contains(layout.adapter_slot1,1)||!layout.code.Contains(layout.adapter_slot5,1)) return false;
        result=layout;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool WriteContextForwarding(FILE* file,const ContextForwardingSnapshot& s) {
    return std::fprintf(file,",\"forwarding\":{\"layout_version\":1,\"status\":\"%s\","
        "\"sampled_at\":\"invocation_entry\",\"dispatch_observed\":false,"
        "\"parent\":\"0x%llx\",\"payload\":\"0x%llx\",\"child\":\"0x%llx\","
        "\"child_vtable\":\"0x%llx\",\"child_slot8_target\":\"0x%llx\"}",ForwardingStatusName(s.status),
        static_cast<unsigned long long>(s.parent),static_cast<unsigned long long>(s.payload),
        static_cast<unsigned long long>(s.child),static_cast<unsigned long long>(s.child_vtable),
        static_cast<unsigned long long>(s.child_slot8_target))>=0;
}
}
