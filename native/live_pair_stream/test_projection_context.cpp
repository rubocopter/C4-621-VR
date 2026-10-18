// CPU-only end-to-end scope publication; no MinHook initialization or game.
#include "projection_observer.hpp"
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
using namespace frame_trace;
static void Require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
static ProjectionSnapshot Sample() {
    ProjectionSnapshot s;s.view=0x1234;s.fov=1.5f;s.near_plane=1;s.far_plane=500000;
    s.width=2560;s.height=1440;s.aspect=16.f/9;
    const float matrix[16]={.6f,0,0,0,0,1.1f,0,0,0,0,-.000002f,1,0,0,1.000002f,0};
    std::memcpy(s.matrix,matrix,sizeof matrix);return s;
}
int main(int argc,char** argv) try {
    RenderContextObserver contexts;
    ProjectionObserver projections;
    std::uintptr_t vtable[6]={0x1000,0x1100,0x1200,0x1300,0x1400,0x1500};
    struct Context { std::uintptr_t* vtable; } context{vtable};
    auto sample=[&] {
        LARGE_INTEGER begin{},end{};QueryPerformanceCounter(&begin);QueryPerformanceCounter(&end);
        Require(projections.Record(Sample(),0x5678,0,begin.QuadPart,end.QuadPart,GetCurrentThreadId()),"CPU helper sample rejected");
    };
    auto* result=contexts.Invoke(reinterpret_cast<void*>(0x10),&context,reinterpret_cast<void*>(0x30),
        [&](void*,void*,void*)->void* {
            Require(CurrentContextInvocation().ordinal!=0,"Invocation identity missing inside callback");
            sample();return reinterpret_cast<void*>(0x42);
        });
    Require(result==reinterpret_cast<void*>(0x42)&&CurrentContextInvocation().ordinal==0,"Original result or scope restoration changed");
    sample(); // Same matrix and helper path outside the invocation must stay unknown.
    contexts.Stop();projections.Stop();
    FILE* file=nullptr;
    if(argc==2) Require(fopen_s(&file,argv[1],"w+x")==0&&file,"Choose a fresh output path");
    else Require(tmpfile_s(&file)==0&&file,"Cannot open test output");
    std::unique_ptr<FILE,decltype(&std::fclose)> close(file,&std::fclose);
    Require(contexts.Write(file)&&projections.Write(file)&&std::fflush(file)==0,"Incomplete scope log");
    std::rewind(file);std::string output;char bytes[4096];
    while(const auto size=std::fread(bytes,1,sizeof bytes,file)) output.append(bytes,size);
    Require(!std::ferror(file),"Scope log read failed");
    Require(output.find("\"cpu_context\":{\"invocation_ordinal\":1")!=std::string::npos,"Immutable helper scope token lost");
    Require(output.find("\"cpu_context\":{\"invocation_ordinal\":0")!=std::string::npos,"Unscoped helper wrongly inherited context");
    Require(output.find("\"frame_associated\":true")==std::string::npos,"CPU scope must never certify captured frame identity");
    // v20 filled its helper window before the selected CPU context ran. A
    // context-only window must survive arbitrarily many unscoped helper calls.
    ProjectionObserver scoped_only(true,true);
    for(unsigned i=0;i<ProjectionObserver::Capacity+10;++i)
        Require(!scoped_only.Record(Sample(),0x5678,0,1,2,GetCurrentThreadId()),"Unscoped calls exhausted the context-only window");
    RenderContextObserver later_context;
    later_context.Invoke(reinterpret_cast<void*>(0x10),&context,reinterpret_cast<void*>(0x30),
        [&](void*,void*,void*)->void* {
            Require(scoped_only.Record(Sample(),0x5678,0,1,2,GetCurrentThreadId()),"Selected context lost its helper capacity");
            return nullptr;
        });
    scoped_only.Stop();
    FILE* selected_file=nullptr;Require(tmpfile_s(&selected_file)==0&&selected_file,"Cannot open selected output");
    std::unique_ptr<FILE,decltype(&std::fclose)> selected_close(selected_file,&std::fclose);
    Require(scoped_only.Write(selected_file)&&std::fflush(selected_file)==0,"Context-only log failed");
    std::rewind(selected_file);std::string selected;
    while(const auto size=std::fread(bytes,1,sizeof bytes,selected_file)) selected.append(bytes,size);
    Require(selected.find("\"samples\":1")!=std::string::npos&&
            selected.find("\"outside_context_skipped\":266")!=std::string::npos,
            "Context-only skip accounting or selected publication missing");
    std::cout<<"Projection helper preserved one scoped and one unscoped immutable CPU sample\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
