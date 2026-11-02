#include "contract.hpp"
#include <functional>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <type_traits>

using namespace dlss_contract;
static void Require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
static constexpr std::array<EvaluationResult,2> Success{EvaluationResult::Succeeded,EvaluationResult::Succeeded};

// Hand-sized fixture: quarter-pixel right, half-pixel up at 8x4 gives
// NDC offsets +1/16,+1/4. Principal points differ deliberately by eye.
static PairInput Pair(std::uint64_t frame=1,std::uint64_t generation=7,float sign=1,bool reverse=true) {
    PairInput p;p.frame={generation,frame,100+frame};p.renderSize={8,4};p.outputSize={16,8};p.configurationRevision=1;
    for(unsigned i=0;i<2;++i) {
        EyeInput e;
        const Stamp s{p.frame,i?Eye::Right:Eye::Left,Association::FrameAssociated,frame*10+i+1};
        e.color={s,{11+i*10,p.renderSize}};e.depth={s,{12+i*10,p.renderSize}};
        e.motion.stamp=s;e.motion.value.image={13+i*10,p.renderSize};
        if(frame>1) e.motion.value.previous={generation,frame-1,99+frame};
        e.motion.value.coverage=MotionCoverage::FullScene;
        e.motion.value.convention=MotionConvention::CurrentToPreviousRenderPixelsJitterExcluded;
        e.view.stamp=s;auto& v=e.view.value;
        v.unjitteredProjection={1,0,0,0,0,2,0,0,(i?-.25f:.125f)*sign,-.125f*sign,
            (reverse?-1.f:2.f)*sign,sign,0,0,reverse?2.f:-2.f,0};
        v.renderedProjection=v.unjitteredProjection;
        v.renderedProjection[8]+=.0625f*sign;v.renderedProjection[9]+=.25f*sign;
        v.jitterX=.25f;v.jitterY=-.5f;v.jitterPhase=0;
        v.depth=reverse?DepthDirection::Reverse:DepthDirection::Forward;p.eyes[i]=e;
    }
    return p;
}
static std::array<Stamp*,4> Stamps(EyeInput& e) { return {&e.color.stamp,&e.depth.stamp,&e.motion.stamp,&e.view.stamp}; }
static Plan Accept(History& h,const PairInput& p) {
    auto d=h.Prepare(p);Require(d.reason==Reject::None&&d.plan.has_value(),"Coherent pair rejected");return *d.plan;
}
static void Rejects(History& h,const PairInput& p,Reject expected) {
    auto d=h.Prepare(p);
    if(d.reason!=expected||d.plan)
        throw std::runtime_error("Expected rejection "+std::to_string(int(expected))+", got "+std::to_string(int(d.reason)));
}
static void Commit(History& h,const PairInput& p) { Require(h.Finish(Accept(h,p),Success)==Reject::None,"Commit failed"); }
static void Throws(const std::function<void()>& action) {
    bool threw=false;try { action(); } catch(const std::invalid_argument&) { threw=true; }
    Require(threw,"Invalid history reset/bind accepted");
}
static void Admission() {
    History h(7);Rejects(h,{},Reject::IncompletePair);
    for(unsigned missing=0;missing<2;++missing) {
        auto p=Pair();p.eyes[missing].reset();Rejects(h,p,Reject::IncompletePair);
    }
    for(auto provenance:{Association::Unknown,Association::Unassociated}) {
        auto p=Pair();for(auto& e:p.eyes) for(auto* s:Stamps(*e)) s->association=provenance;
        Rejects(h,p,Reject::Unassociated);h.Reset(ResetReason::Explicit,7);Rejects(h,p,Reject::Unassociated);
    }
    auto p=Pair();p.eyes[0]->view.stamp.associationRecord=0;Rejects(h,p,Reject::Unassociated);
    // Static calibration is numerically valid but never supplies provenance.
    auto legacy=reprojection::ProjectionCalibration::LegacyObservedAc6();legacy.Validate();
    legacy.provenance=reprojection::CalibrationProvenance::ExplicitUnassociated;legacy.Validate();
    p=Pair();p.eyes[0]->view.stamp.association=Association::Unassociated;Rejects(h,p,Reject::Unassociated);
    auto plan=Accept(h,Pair());Require(plan.ResetHistory()&&!plan.Previous()[0]&&!plan.Previous()[1],"First pair reused history");
    p=Pair();p.renderSize.width=0;Rejects(h,p,Reject::InvalidExtent);
    p=Pair();p.outputSize.height=0;Rejects(h,p,Reject::InvalidExtent);
    p=Pair();p.outputSize.width=8193;Rejects(h,p,Reject::InvalidExtent);
    p=Pair();p.outputSize.width=4;Rejects(h,p,Reject::InvalidExtent);
    p=Pair();p.configurationRevision=0;Rejects(h,p,Reject::InvalidExtent);
    for(unsigned i=0;i<2;++i) for(unsigned role=0;role<3;++role) {
        p=Pair();auto& e=*p.eyes[i];Surface* surfaces[]{&e.color.value,&e.depth.value,&e.motion.value.image};
        surfaces[role]->size.height=5;Rejects(h,p,Reject::InvalidExtent);
        p=Pair();auto& e2=*p.eyes[i];Surface* surfaces2[]{&e2.color.value,&e2.depth.value,&e2.motion.value.image};
        surfaces2[role]->resourceView=0;Rejects(h,p,Reject::InvalidExtent);
    }
}
static void AssociationIdentity() {
    History h(7);
    for(unsigned eye=0;eye<2;++eye) for(unsigned role=0;role<4;++role) {
        auto p=Pair();Stamps(*p.eyes[eye])[role]->eye=eye?Eye::Left:Eye::Right;Rejects(h,p,Reject::WrongEye);
        p=Pair();Stamps(*p.eyes[eye])[role]->eye=Eye::Unknown;Rejects(h,p,Reject::WrongEye);
        for(unsigned field=0;field<3;++field) {
            p=Pair();auto& f=Stamps(*p.eyes[eye])[role]->frame;
            if(field==0) ++f.generation;else if(field==1) ++f.sourceFrame;else ++f.simulation;
            Rejects(h,p,Reject::FrameMismatch);
        }
        p=Pair();Stamps(*p.eyes[eye])[role]->associationRecord+=1000;Rejects(h,p,Reject::FrameMismatch);
        p=Pair();Stamps(*p.eyes[eye])[role]->association=Association::Unknown;Rejects(h,p,Reject::Unassociated);
    }
    auto p=Pair();p.eyes[1]=p.eyes[0];Rejects(h,p,Reject::WrongEye);
    p=Pair();p.frame.sourceFrame=0;Rejects(h,p,Reject::FrameMismatch);
    p=Pair();p.frame.simulation=0;Rejects(h,p,Reject::FrameMismatch);
    Rejects(h,Pair(1,8),Reject::WrongGeneration);
}
static void Jitter() {
    for(float sign:{1.f,-1.f}) for(bool reverse:{false,true}) {
        History h(7);auto p=Pair(1,7,sign,reverse);Accept(h,p);
        for(unsigned eye=0;eye<2;++eye) {
            auto bad=p;bad.eyes[eye]->view.value.renderedProjection[8]-=.125f*sign;
            Rejects(h,bad,Reject::InvalidJitter);
            bad=p;bad.eyes[eye]->view.value.renderedProjection[9]-=.5f*sign;
            Rejects(h,bad,Reject::InvalidJitter);
            bad=p;bad.eyes[eye]->view.value.renderedProjection[0]=2;Rejects(h,bad,Reject::InvalidJitter);
            bad=p;bad.eyes[eye]->view.value.jitterPhase.reset();Rejects(h,bad,Reject::InvalidJitter);
        }
        auto bad=p;bad.eyes[1]->view.value.jitterPhase=1;Rejects(h,bad,Reject::InvalidJitter);
        bad=p;bad.eyes[1]->view.value.jitterX=0;bad.eyes[1]->view.value.renderedProjection[8]=bad.eyes[1]->view.value.unjitteredProjection[8];
        Rejects(h,bad,Reject::InvalidJitter);
        // A relative coefficient tolerance would bless lost jitter here.
        bad=p;for(auto& e:bad.eyes) {
            e->view.value.unjitteredProjection[8]=1e10f*sign;
            e->view.value.renderedProjection[8]=1e10f*sign;
        }
        Rejects(h,bad,Reject::InvalidJitter);
        // At the fixed pixel tolerance (1/1024 pixel = 1/4096 NDC
        // coefficient here), then one float step inside and outside it.
        auto boundary=p;
        for(auto& e:boundary.eyes) e->view.value.renderedProjection[8]+=.000244140625f*sign;
        Accept(h,boundary);
        auto inside=boundary,outside=boundary;
        for(auto& e:inside.eyes) {
            auto& v=e->view.value;v.renderedProjection[8]=std::nextafter(v.renderedProjection[8],v.unjitteredProjection[8]);
        }
        for(auto& e:outside.eyes) {
            auto& v=e->view.value;v.renderedProjection[8]=std::nextafter(v.renderedProjection[8],sign*std::numeric_limits<float>::infinity());
        }
        Accept(h,inside);Rejects(h,outside,Reject::InvalidJitter);
        for(auto& e:p.eyes) { auto& v=e->view.value;v.jitterX=0;v.jitterY=0;v.renderedProjection=v.unjitteredProjection; }
        Accept(h,p); // Known, applied zero jitter is supported.
    }
}
static void DepthMotion() {
    History h(7);
    for(unsigned eye=0;eye<2;++eye) {
        for(auto d:{DepthDirection::Unknown,DepthDirection::Forward}) {
            auto p=Pair();p.eyes[eye]->view.value.depth=d;Rejects(h,p,Reject::InvalidDepth);
        }
        for(auto coverage:{MotionCoverage::Unknown,MotionCoverage::CameraOnly}) {
            auto p=Pair();p.eyes[eye]->motion.value.coverage=coverage;Rejects(h,p,Reject::InvalidMotion);
        }
        auto p=Pair();p.eyes[eye]->motion.value.convention=MotionConvention::Unknown;Rejects(h,p,Reject::InvalidMotion);
        p=Pair();p.eyes[eye]->motion.value.previous={7,0,101};Rejects(h,p,Reject::InvalidMotion);
        p=Pair();p.eyes[eye]->motion.value.previous={8,1,101};Rejects(h,p,Reject::InvalidMotion);
        p=Pair();p.eyes[eye]->motion.value.previous=p.frame;Rejects(h,p,Reject::InvalidMotion);
        for(unsigned index:{0u,3u,10u,11u,14u,15u}) {
            p=Pair();p.eyes[eye]->view.value.unjitteredProjection[index]=0;
            if(index==3||index==15) p.eyes[eye]->view.value.unjitteredProjection[index]=1;
            if(index==10) p.eyes[eye]->view.value.unjitteredProjection[index]=.5f;
            Rejects(h,p,Reject::InvalidProjection);
        }
    }
}
static void Nonfinite() {
    History h(7);
    for(unsigned eye=0;eye<2;++eye) for(float value:{std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        for(unsigned matrix=0;matrix<2;++matrix) for(unsigned index=0;index<16;++index) {
            auto p=Pair();auto& v=p.eyes[eye]->view.value;
            (matrix?v.renderedProjection:v.unjitteredProjection)[index]=value;
            Rejects(h,p,Reject::InvalidProjection);
        }
        for(unsigned axis=0;axis<2;++axis) {
            auto p=Pair();auto& v=p.eyes[eye]->view.value;(axis?v.jitterY:v.jitterX)=value;
            Rejects(h,p,Reject::InvalidJitter);
        }
    }
    auto p=Pair();for(auto& e:p.eyes) {e->view.value.jitterX=std::numeric_limits<float>::max();e->view.value.renderedProjection[8]=std::numeric_limits<float>::max();}
    Rejects(h,p,Reject::InvalidJitter);
    for(unsigned eye=0;eye<2;++eye) {
        for(unsigned matrix=0;matrix<2;++matrix) for(unsigned index:{0u,5u,8u,9u,10u,11u,14u}) {
            p=Pair();auto& v=p.eyes[eye]->view.value;
            (matrix?v.renderedProjection:v.unjitteredProjection)[index]=std::numeric_limits<float>::denorm_min();
            Rejects(h,p,Reject::InvalidProjection);
        }
        for(unsigned axis=0;axis<2;++axis) {
            p=Pair();auto& v=p.eyes[eye]->view.value;(axis?v.jitterY:v.jitterX)=std::numeric_limits<float>::denorm_min();
            Rejects(h,p,Reject::InvalidJitter);
        }
    }
}
static void Transaction() {
    History h(7);auto p=Pair();auto plan=Accept(h,p);
    p.eyes[0]->view.value.unjitteredProjection[8]=99; // Plan owns its snapshot.
    Require(plan.Input().eyes[0]->view.value.unjitteredProjection[8]==.125f,"Plan aliases caller input");
    Require(h.Finish(plan,Success)==Reject::None,"First finish failed");
    auto second=Accept(h,Pair(2));Require(!second.ResetHistory(),"Consecutive pair reset");
    Require(second.Previous()[0]->color.stamp.eye==Eye::Left&&second.Previous()[1]->color.stamp.eye==Eye::Right,"Eye histories crossed");
    Require(second.Previous()[0]->view.stamp.associationRecord==11&&second.Previous()[1]->view.stamp.associationRecord==12,"Eye provenance lost");
    Require(second.Previous()[0]->view.value.unjitteredProjection[8]==.125f&&second.Previous()[1]->view.value.unjitteredProjection[8]==-.25f,"Projection histories crossed");
    Require(h.Finish(second,Success)==Reject::None,"Second finish failed");
    for(auto left:{EvaluationResult::Succeeded,EvaluationResult::Failed,EvaluationResult::NotEvaluated})
    for(auto right:{EvaluationResult::Succeeded,EvaluationResult::Failed,EvaluationResult::NotEvaluated}) {
        if(left==EvaluationResult::Succeeded&&right==EvaluationResult::Succeeded) continue;
        const std::array<EvaluationResult,2> results{left,right};

        // Prove watermark retention immediately after failure, before any
        // successful retry can advance it and hide a cleared watermark.
        History replayCheck(7);Commit(replayCheck,Pair());Commit(replayCheck,Pair(2));
        Require(replayCheck.Finish(Accept(replayCheck,Pair(3)),results)==Reject::EvaluationFailed,"Failed evaluation committed");
        Rejects(replayCheck,Pair(2),Reject::Replay);

        // A separate history is essential: the replay rejection above would
        // itself force reset, masking a missing invalidation in Finish.
        History reuseCheck(7);Commit(reuseCheck,Pair());Commit(reuseCheck,Pair(2));
        auto third=Accept(reuseCheck,Pair(3));Require(!third.ResetHistory(),"Failure fixture was already reset");
        Require(reuseCheck.Finish(third,results)==Reject::EvaluationFailed,"Failed evaluation committed");
        auto retry=Accept(reuseCheck,Pair(3));
        Require(retry.ResetHistory()&&!retry.Previous()[0]&&!retry.Previous()[1],"Failed evaluation retained history");
        Require(reuseCheck.Finish(retry,Success)==Reject::None,"Failed frame retry rejected");
        Rejects(reuseCheck,Pair(2),Reject::Replay);
    }
}
static void Tickets() {
    static_assert(!std::is_copy_constructible<History>::value&&!std::is_move_constructible<History>::value,"History identity must remain stable");
    static_assert(!std::is_default_constructible<Plan>::value&&!std::is_copy_assignable<Plan>::value,"Ticket must be private/immutable");
    History h(7),other(7);auto foreign=Accept(other,Pair());auto own=Accept(h,Pair());
    Require(h.Finish(foreign,Success)==Reject::StalePlan,"Foreign ticket accepted");
    Require(h.Finish(own,Success)==Reject::None,"Foreign ticket invalidated owner's current plan");
    Require(h.Finish(own,Success)==Reject::StalePlan,"Finished ticket reused");
    auto old=Accept(h,Pair(2));auto newer=Accept(h,Pair(2));
    Require(newer.ResetHistory(),"Superseding unfinished work retained reusable backend history");
    Require(h.Finish(old,Success)==Reject::StalePlan,"Superseded plan accepted");
    Require(h.Finish(newer,Success)==Reject::None,"Latest plan rejected");
    auto beforeReset=Accept(h,Pair(3));h.Reset(ResetReason::Explicit,7);
    Require(h.Finish(beforeReset,Success)==Reject::StalePlan,"Reset ticket accepted");
    // An owner token must survive address reuse: a raw History* is insufficient.
    alignas(History) unsigned char storage[sizeof(History)];
    auto* first=new(storage) History(7);auto obsolete=Accept(*first,Pair());first->~History();
    auto* replacement=new(storage) History(7);auto replacementPlan=Accept(*replacement,Pair());
    const auto stale=replacement->Finish(obsolete,Success),valid=replacement->Finish(replacementPlan,Success);
    replacement->~History();Require(stale==Reject::StalePlan&&valid==Reject::None,"Reused address inherited ticket identity");
}
static void ResetAndDiscontinuity() {
    History h(7);Commit(h,Pair());
    for(auto reason:{ResetReason::Explicit,ResetReason::CameraCut,ResetReason::Rebind}) {
        History clean(7);Commit(clean,Pair());clean.Reset(reason,7);auto p=Accept(clean,Pair(2));
        Require(p.ResetHistory()&&!p.Previous()[0]&&!p.Previous()[1],"Reset retained reuse");
        Rejects(clean,Pair(),Reject::Replay);
    }
    Commit(h,Pair(2));
    auto incomplete=Pair(3);incomplete.eyes[1].reset();auto pending=Accept(h,Pair(3));
    Rejects(h,incomplete,Reject::IncompletePair);
    Require(h.Finish(pending,Success)==Reject::StalePlan,"Rejected pair left pending ticket usable");
    Require(Accept(h,Pair(3)).ResetHistory(),"Incomplete pair did not force reset");Commit(h,Pair(3));
    auto invalid=Pair(4);invalid.eyes[0]->motion.value.coverage=MotionCoverage::Unknown;
    Rejects(h,invalid,Reject::InvalidMotion);Require(Accept(h,Pair(4)).ResetHistory(),"Invalid pair did not force reset");Commit(h,Pair(4));
    auto sizeChange=Pair(5);sizeChange.outputSize={32,16};Require(Accept(h,sizeChange).ResetHistory(),"Output resize reused history");
    auto configChange=Pair(5);configChange.configurationRevision=2;Require(Accept(h,configChange).ResetHistory(),"Config change reused history");
    auto gap=Pair(6);Require(Accept(h,gap).ResetHistory(),"Motion reference gap reused history");
    // Discarded plans may already have reached a backend; settle with a reset
    // commit before checking a deliberately skipped publication's continuity.
    Commit(h,Pair(5));gap=Pair(7);
    gap.eyes[0]->motion.value.previous=Pair(5).frame;gap.eyes[1]->motion.value.previous=Pair(5).frame;
    auto continuous=Accept(h,gap);Require(!continuous.ResetHistory(),"Explicit last-committed reference unnecessarily reset");
    Require(h.Finish(continuous,Success)==Reject::None,"Continuous skipped publication did not commit");
    auto oneEyeGap=Pair(8);oneEyeGap.eyes[1]->motion.value.previous={};
    Require(Accept(h,oneEyeGap).ResetHistory(),"One eye discontinuity retained the other history");
    // Each trigger is checked against clean reusable history, so an unrelated
    // outstanding plan cannot mask a missing configuration/reset check.
    for(unsigned change=0;change<6;++change) {
        History clean(7);Commit(clean,Pair());auto changed=Pair(2);
        if(change==0) changed.outputSize={32,16};
        if(change==1) changed.configurationRevision=2;
        if(change==2) changed=Pair(2,7,-1);
        if(change==3) changed=Pair(2,7,1,false);
        if(change==4) changed=Pair(3);
        if(change==5) {
            changed.renderSize={16,4};
            for(auto& e:changed.eyes) {
                e->color.value.size=changed.renderSize;e->depth.value.size=changed.renderSize;e->motion.value.image.size=changed.renderSize;
                auto& v=e->view.value;v.renderedProjection[8]=v.unjitteredProjection[8]+.03125f;
            }
        }
        const auto reset=Accept(clean,changed);Require(reset.ResetHistory(),"Discontinuity/configuration change reused history");
    }
    for(unsigned trigger=0;trigger<3;++trigger) {
        History clean(7);Commit(clean,Pair());auto bad=Pair(trigger==2?3:2);
        if(trigger==0) clean.Reset(ResetReason::CameraCut,7);
        if(trigger==1) bad.configurationRevision=2;
        bad.eyes[1]->motion.value.coverage=MotionCoverage::Unknown;
        Rejects(clean,bad,Reject::InvalidMotion);
    }
    h.Reset(ResetReason::Rebind,8);Require(Accept(h,Pair(1,8)).ResetHistory(),"Rebind retained history");
    Rejects(h,Pair(5,7),Reject::WrongGeneration);Commit(h,Pair(1,8));h.Reset(ResetReason::Explicit,8);Rejects(h,Pair(1,8),Reject::Replay);
    Throws([]{History invalid(0);});Throws([&]{h.Reset(ResetReason::Explicit,0);});Throws([&]{h.Reset(ResetReason::Explicit,9);});
}
int main(int argc,char** argv) try {
    const std::pair<const char*,void(*)()> cases[]={{"admission",Admission},{"association",AssociationIdentity},
        {"jitter",Jitter},{"depth_motion",DepthMotion},{"nonfinite",Nonfinite},{"transaction",Transaction},{"tickets",Tickets},{"reset",ResetAndDiscontinuity}};
    Require(argc==2,"Specify a contract case");
    for(const auto& entry:cases) if(argv[1]==std::string(entry.first)) { entry.second();std::cout<<entry.first<<" passed\n";return 0; }
    throw std::runtime_error("Unknown case");
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
