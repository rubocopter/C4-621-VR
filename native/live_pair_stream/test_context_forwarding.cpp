// CPU fixtures only: no hooks, game process or virtual dispatch.
#include "render_context_trace.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <atomic>
#include <thread>
using namespace frame_trace;
static void Require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
static void* __fastcall Target(void*,void*,void*) { throw std::runtime_error("Candidate target must never run"); }
struct Fixture {
    std::array<std::uintptr_t,32> tables{};
    std::array<std::uintptr_t,18> parent{};
    std::uintptr_t child=0;
    Fixture() {
        tables[1]=tables[5]=tables[16+8]=reinterpret_cast<std::uintptr_t>(&Target);
        parent[12]=reinterpret_cast<std::uintptr_t>(&child); // parent+0x60
        parent[16]=reinterpret_cast<std::uintptr_t>(tables.data()); // context=parent+0x80
        parent[17]=0x1230; // context+8: only copied, never dereferenced
        child=reinterpret_cast<std::uintptr_t>(tables.data()+16);
    }
    ContextForwardingLayout Layout() const {
        const auto code=reinterpret_cast<std::uintptr_t>(&Target);
        return {reinterpret_cast<std::uintptr_t>(tables.data()),code,code,
                {reinterpret_cast<std::uintptr_t>(tables.data()),reinterpret_cast<std::uintptr_t>(tables.data()+tables.size())},
                {code,code+1}};
    }
    ContextInvocation Token() const {
        ContextInvocation c;c.ordinal=1;c.thread=GetCurrentThreadId();
        c.context=reinterpret_cast<std::uintptr_t>(parent.data()+16);
        c.vtable=parent[16];c.slot1_target=tables[1];c.slot5_target=tables[5];return c;
    }
};
static void RejectedWithoutPayload(const ContextForwardingSnapshot& s) {
    Require(s.status!=ForwardingStatus::Candidate,"Invalid forwarding admitted");
    Require(!s.parent&&!s.payload&&!s.child&&!s.child_vtable&&!s.child_slot8_target,"Rejected read exposed partial payload");
}
static std::string Log(RenderContextObserver& observer) {
    observer.Stop();FILE* file=nullptr;Require(!tmpfile_s(&file)&&file,"Cannot create log");
    std::unique_ptr<FILE,decltype(&std::fclose)> close(file,&std::fclose);
    Require(observer.Write(file)&&std::fflush(file)==0,"Cannot publish forwarding log");
    std::rewind(file);std::string text;char bytes[4096];
    while(const auto n=std::fread(bytes,1,sizeof bytes,file)) text.append(bytes,n);
    return text;
}
static void CachedImageBoundsRejectWritableOrIncompatibleSections() {
    // Reserve the pinned RVA extent, commit only synthetic PE headers. No code
    // is mapped or executed; the production setup reader only needs headers.
    auto* image=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x4500000,MEM_RESERVE,PAGE_NOACCESS));
    Require(image!=nullptr,"Cannot reserve synthetic image");
    struct FreeImage { void* image;~FreeImage(){VirtualFree(image,0,MEM_RELEASE);} } free_image{image};
    Require(VirtualAlloc(image,4096,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"Cannot commit synthetic headers");
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(image);dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=128;
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(image+128);nt->Signature=IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;nt->FileHeader.NumberOfSections=3;
    nt->FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt->OptionalHeader.SizeOfImage=0x4500000;
    auto* sections=IMAGE_FIRST_SECTION(nt);
    std::memcpy(sections[0].Name,".text",6);sections[0].VirtualAddress=0x1000;
    sections[0].Misc.VirtualSize=0x3fff000;sections[0].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE;
    std::memcpy(sections[1].Name,".rdata",7);sections[1].VirtualAddress=0x4000000;
    sections[1].Misc.VirtualSize=0x200000;sections[1].Characteristics=IMAGE_SCN_MEM_READ;
    // The real pinned executable has a second .text after .idata. A later
    // same-named section must not discard the known adapter's code range.
    std::memcpy(sections[2].Name,".text",6);sections[2].VirtualAddress=0x4300000;
    sections[2].Misc.VirtualSize=0x100000;sections[2].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE;
    ContextForwardingLayout layout;
    Require(LoadGameForwardingLayout(reinterpret_cast<HMODULE>(image),layout)&&layout.Enabled(),"Compatible image bounds rejected");
    const auto base=reinterpret_cast<std::uintptr_t>(image);
    Require(layout.adapter_vtable==base+0x4098e38&&layout.adapter_slot1==base+0x2b9d140&&
            layout.adapter_slot5==base+0x2b9db10,"Pinned adapter RVAs changed");
    Require(layout.ContainsCode(base+0x4300000)&&!layout.ContainsCode(base+0x4200000),
            "Secondary code section discarded or non-executable gap admitted");
    sections[1].Characteristics|=IMAGE_SCN_MEM_WRITE;
    Require(!LoadGameForwardingLayout(reinterpret_cast<HMODULE>(image),layout),"Writable vtable range accepted");
    sections[1].Characteristics=IMAGE_SCN_MEM_READ;sections[0].Characteristics|=IMAGE_SCN_MEM_WRITE;
    Require(!LoadGameForwardingLayout(reinterpret_cast<HMODULE>(image),layout),"Writable target range accepted");
    sections[0].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE;nt->OptionalHeader.SizeOfImage=0x4100000;
    Require(!LoadGameForwardingLayout(reinterpret_cast<HMODULE>(image),layout),"Out-of-image section accepted");
    Require(!LoadGameForwardingLayout(reinterpret_cast<HMODULE>(1),layout),"Unreadable image headers accepted");
}
int main(int argc,char** argv) try {
    CachedImageBoundsRejectWritableOrIncompatibleSections();
    Fixture f;const auto identity=f.Token();const auto layout=f.Layout();
    auto s=ReadContextForwarding(identity,layout);
    Require(s.status==ForwardingStatus::Candidate&&s.parent==reinterpret_cast<std::uintptr_t>(f.parent.data())&&
            s.payload==0x1230&&s.child==reinterpret_cast<std::uintptr_t>(&f.child)&&
            s.child_vtable==f.child&&s.child_slot8_target==f.tables[24],"Known adapter forwarding missing");
    auto unknown=layout;unknown.adapter_vtable+=8;
    RejectedWithoutPayload(ReadContextForwarding(identity,unknown));
    auto wrong=identity;wrong.slot1_target+=1;
    RejectedWithoutPayload(ReadContextForwarding(wrong,layout));
    wrong=identity;wrong.context=0x40;RejectedWithoutPayload(ReadContextForwarding(wrong,layout));
    f.parent[17]=0;Require(ReadContextForwarding(identity,layout).status==ForwardingStatus::NullPayload,"Null payload followed child");
    f.parent[17]=0x1230;f.parent[12]=0;
    Require(ReadContextForwarding(identity,layout).status==ForwardingStatus::NullChild,"Null child accepted");
    f.parent[12]=1;RejectedWithoutPayload(ReadContextForwarding(identity,layout));
    f.parent[12]=reinterpret_cast<std::uintptr_t>(&f.child);
    f.child=1;RejectedWithoutPayload(ReadContextForwarding(identity,layout));
    f.child=reinterpret_cast<std::uintptr_t>(f.tables.data()+16);
    f.tables[24]=layout.code.end;RejectedWithoutPayload(ReadContextForwarding(identity,layout));
    f.tables[24]=layout.code.begin;
    // A stale token cannot authorize following a recycled adapter at the same address.
    f.parent[16]+=8;RejectedWithoutPayload(ReadContextForwarding(identity,layout));f.parent[16]-=8;

    // Entry payload must remain immutable even if the original changes every link.
    RenderContextObserver observer(true,layout);unsigned calls=0;
    observer.Invoke(reinterpret_cast<void*>(0x10),f.parent.data()+16,reinterpret_cast<void*>(0x30),
        [&](void*,void*,void*)->void* { ++calls;f.parent[12]=0;f.child=0;return reinterpret_cast<void*>(0x42); });
    const auto log=Log(observer);
    Require(calls==1&&log.find("\"status\":\"candidate\"")!=std::string::npos,"Forwarding was read after original or original repeated");
    Require(log.find("\"dispatch_observed\":false")!=std::string::npos&&
            log.find("\"frame_associated\":true")==std::string::npos,"Candidate promoted to frame/dispatch evidence");

    // Readable table prefix but inaccessible slot8: SEH must discard ALL fields.
    SYSTEM_INFO info{};GetSystemInfo(&info);
    auto* pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,2*info.dwPageSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    Require(pages,"Cannot allocate guarded fixture");
    struct FreePages { void* pages;~FreePages(){VirtualFree(pages,0,MEM_RELEASE);} } free_pages{pages};
    DWORD old=0;Require(VirtualProtect(pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&old)!=0,"Cannot protect fixture");
    f.parent[12]=reinterpret_cast<std::uintptr_t>(&f.child);
    f.child=reinterpret_cast<std::uintptr_t>(pages+info.dwPageSize-8);
    auto guarded_layout=layout;guarded_layout.tables={reinterpret_cast<std::uintptr_t>(pages),reinterpret_cast<std::uintptr_t>(pages+2*info.dwPageSize)};
    // Adapter table must also stay in the declared data range, so use a readable
    // adapter at the start of this mapping and token matching its actual table.
    auto* adapter=reinterpret_cast<std::uintptr_t*>(pages);adapter[1]=adapter[5]=layout.code.begin;
    guarded_layout.adapter_vtable=reinterpret_cast<std::uintptr_t>(adapter);
    f.parent[16]=guarded_layout.adapter_vtable;
    auto guarded=f.Token();guarded.vtable=f.parent[16];guarded.slot1_target=guarded.slot5_target=layout.code.begin;
    const auto fault=ReadContextForwarding(guarded,guarded_layout);
    Require(fault.status==ForwardingStatus::Unreadable,"Faulting slot read did not reject");RejectedWithoutPayload(fault);

    // A snapshot read at entry must not be published before its enclosing call
    // completes; Write cannot wait for the original to return.
    Fixture delayed_fixture;RenderContextObserver delayed(true,delayed_fixture.Layout());
    std::atomic<bool> entered{false},release{false};
    std::thread worker([&]{delayed.Invoke(reinterpret_cast<void*>(0x10),delayed_fixture.parent.data()+16,reinterpret_cast<void*>(0x30),
        [&](void*,void*,void*)->void* {entered.store(true);while(!release.load()) std::this_thread::yield();return nullptr;});});
    struct Join { std::atomic<bool>& release;std::thread& worker;~Join(){release.store(true);if(worker.joinable()) worker.join();} } join{release,worker};
    const auto deadline=GetTickCount64()+5000;
    while(!entered.load()&&GetTickCount64()<deadline) std::this_thread::yield();
    Require(entered.load(),"Delayed fixture did not start");
    const auto partial=Log(delayed);
    Require(partial.find("\"forwarding\":")==std::string::npos&&partial.find("\"snapshot_complete\":false")!=std::string::npos,
            "Incomplete invocation published forwarding snapshot");
    release.store(true);worker.join();
    const auto complete=Log(delayed);
    Require(complete.find("\"status\":\"candidate\"")!=std::string::npos,"Completed snapshot lost entry payload");
    if(argc==2) { FILE* output=nullptr;Require(!fopen_s(&output,argv[1],"w+x")&&output,"Choose fresh output");
        const auto written=std::fwrite(log.data(),1,log.size(),output);std::fclose(output);Require(written==log.size(),"Cannot write fixture"); }
    std::cout<<"PASS: guarded forwarding and immutable invocation-entry publication\n";return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
