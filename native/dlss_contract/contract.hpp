#pragma once
#include "../reprojection/projection.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <limits>

namespace dlss_contract {
enum class Eye { Unknown, Left, Right };
enum class Association { Unknown, Unassociated, FrameAssociated };
enum class DepthDirection { Unknown, Forward, Reverse };
enum class MotionCoverage { Unknown, CameraOnly, FullScene };
enum class MotionConvention { Unknown, CurrentToPreviousRenderPixelsJitterExcluded };
enum class EvaluationResult { NotEvaluated, Failed, Succeeded };
enum class ResetReason { Explicit, CameraCut, Rebind };
enum class Reject {
    None, IncompletePair, Unassociated, WrongEye, FrameMismatch,
    WrongGeneration, Replay, InvalidExtent, InvalidProjection,
    InvalidJitter, InvalidDepth, InvalidMotion, StalePlan, EvaluationFailed
};
struct FrameKey {
    std::uint64_t generation=0, sourceFrame=0, simulation=0;
    bool operator==(const FrameKey& b) const {
        return generation==b.generation&&sourceFrame==b.sourceFrame&&simulation==b.simulation;
    }
    bool operator!=(const FrameKey& b) const { return !(*this==b); }
};
struct Extent {
    std::uint32_t width=0, height=0;
    bool operator==(const Extent& b) const { return width==b.width&&height==b.height; }
    bool operator!=(const Extent& b) const { return !(*this==b); }
};
// These are upstream declarations, not proof of texture contents or ownership.
struct Stamp {
    FrameKey frame{};
    Eye eye=Eye::Unknown;
    Association association=Association::Unknown;
    std::uint64_t associationRecord=0;
};
template<class T> struct Bound { Stamp stamp{}; T value{}; };
struct Surface { std::uint64_t resourceView=0; Extent size{}; };
struct View {
    // Projections only, never full view-projection matrices. Row vectors,
    // device depth [0,1]; image jitter is in render pixels (+X right/+Y down).
    std::array<float,16> unjitteredProjection{}, renderedProjection{};
    float jitterX=0, jitterY=0;
    std::optional<std::uint32_t> jitterPhase;
    DepthDirection depth=DepthDirection::Unknown;
};
struct Motion {
    Surface image{};
    FrameKey previous{};
    MotionCoverage coverage=MotionCoverage::Unknown;
    MotionConvention convention=MotionConvention::Unknown;
};
struct EyeInput {
    Bound<Surface> color{}, depth{};
    Bound<Motion> motion{};
    Bound<View> view{};
};
struct PairInput {
    FrameKey frame{};
    Extent renderSize{}, outputSize{};
    std::uint64_t configurationRevision=0;
    std::array<std::optional<EyeInput>,2> eyes{};
};
namespace detail { struct Owner {}; }
class History;
class Plan {
public:
    const PairInput& Input() const noexcept { return input_; }
    bool ResetHistory() const noexcept { return reset_; }
    const std::array<std::optional<EyeInput>,2>& Previous() const noexcept { return previous_; }
private:
    friend class History;
    Plan(std::shared_ptr<const detail::Owner> owner, std::uint64_t revision,
         const PairInput& input, bool reset,
         const std::array<std::optional<EyeInput>,2>& previous)
        : owner_(std::move(owner)), revision_(revision), input_(input), reset_(reset), previous_(previous) {}
    const std::shared_ptr<const detail::Owner> owner_;
    const std::uint64_t revision_;
    const PairInput input_;
    const bool reset_;
    const std::array<std::optional<EyeInput>,2> previous_;
};
struct Decision { Reject reason=Reject::None; std::optional<Plan> plan; };
class History {
public:
    explicit History(std::uint64_t expectedGeneration) : generation_(expectedGeneration) {
        if(!generation_) throw std::invalid_argument("History needs a nonzero generation");
    }
    History(const History&)=delete;
    History& operator=(const History&)=delete;
    History(History&&)=delete;
    History& operator=(History&&)=delete;
    // Admission ticket/revision/forceReset state may mutate. Committed eye
    // inputs and replay watermark do not. Each call consumes the outstanding
    // ticket; rejection or superseding unfinished work makes reuse unsafe.
    Decision Prepare(const PairInput& input) {
        if(pending_) forceReset_=true;
        pending_=false;
        AdvanceRevision();
        const auto reason=Validate(input);
        if(reason!=Reject::None) {
            forceReset_=true;
            return {reason,std::nullopt};
        }
        bool reset=forceReset_||!committed_;
        if(committed_) {
            const auto& old=*committed_;
            reset=reset||input.renderSize!=old.renderSize||input.outputSize!=old.outputSize||
                input.configurationRevision!=old.configurationRevision;
            for(unsigned eye=0;eye<2;++eye) {
                const auto& current=*input.eyes[eye];const auto& prior=*old.eyes[eye];
                reset=reset||current.motion.value.previous!=old.frame||
                    current.view.value.depth!=prior.view.value.depth||
                    current.view.value.unjitteredProjection[11]!=prior.view.value.unjitteredProjection[11];
            }
        }
        std::array<std::optional<EyeInput>,2> previous{};
        if(!reset) previous=committed_->eyes;
        pending_=true;
        return {Reject::None,Plan(owner_,revision_,input,reset,previous)};
    }
    Reject Finish(const Plan& plan,std::array<EvaluationResult,2> results) {
        if(plan.owner_!=owner_||plan.revision_!=revision_||!pending_) return Reject::StalePlan;
        pending_=false;
        AdvanceRevision();
        if(results[0]!=EvaluationResult::Succeeded||results[1]!=EvaluationResult::Succeeded) {
            forceReset_=true;
            return Reject::EvaluationFailed;
        }
        committed_=plan.input_;
        watermark_=plan.input_.frame.sourceFrame;
        forceReset_=false;
        return Reject::None;
    }
    void Reset(ResetReason reason,std::uint64_t expectedGeneration) {
        if(!expectedGeneration||(reason!=ResetReason::Explicit&&reason!=ResetReason::CameraCut&&reason!=ResetReason::Rebind)||
            (expectedGeneration!=generation_&&reason!=ResetReason::Rebind))
            throw std::invalid_argument("Generation changes require an explicit nonzero rebind");
        AdvanceRevision();
        pending_=false;forceReset_=true;
        if(expectedGeneration!=generation_) {
            generation_=expectedGeneration;watermark_=0;committed_.reset();
        }
    }
private:
    const std::shared_ptr<const detail::Owner> owner_=std::make_shared<const detail::Owner>();
    std::uint64_t generation_=0, revision_=0, watermark_=0;
    bool forceReset_=true, pending_=false;
    std::optional<PairInput> committed_;

    void AdvanceRevision() {
        if(revision_==(std::numeric_limits<std::uint64_t>::max)())
            throw std::overflow_error("History revision exhausted; create a new History");
        ++revision_;
    }
    static bool Complete(const FrameKey& f) { return f.generation&&f.sourceFrame&&f.simulation; }
    static bool Empty(const FrameKey& f) { return !f.generation&&!f.sourceFrame&&!f.simulation; }
    static bool ValidSize(Extent s) {
        // Same dimension ceiling as the existing source transport. No GPU
        // allocation/format support follows from passing this host bound.
        return s.width&&s.height&&s.width<=8192&&s.height<=8192;
    }
    static bool ValidSurface(const Surface& s,Extent render) { return s.resourceView&&s.size==render; }
    static Reject ValidateView(const View& v,Extent size) {
        reprojection::Perspective projection{};
        try {
            projection=reprojection::Perspective::FromRowMatrix(v.unjitteredProjection);
            reprojection::RayIntrinsics::FromRowMatrix(v.unjitteredProjection);
            reprojection::RayIntrinsics::FromRowMatrix(v.renderedProjection);
        } catch(const std::invalid_argument&) { return Reject::InvalidProjection; }
        if((v.depth!=DepthDirection::Forward&&v.depth!=DepthDirection::Reverse)||
            (v.depth==DepthDirection::Reverse)!=(projection.b>0)) return Reject::InvalidDepth;
        if(!v.jitterPhase||!std::isfinite(v.jitterX)||!std::isfinite(v.jitterY)||
            std::fpclassify(v.jitterX)==FP_SUBNORMAL||std::fpclassify(v.jitterY)==FP_SUBNORMAL)
            return Reject::InvalidJitter;
        for(unsigned index=0;index<16;++index)
            if(index!=8&&index!=9&&v.renderedProjection[index]!=v.unjitteredProjection[index])
                return Reject::InvalidJitter;
        // Compute in double before subtraction/scaling to avoid float overflow.
        // Tolerance is bounded in pixels, never relative to a huge principal
        // point that could otherwise hide a completely unapplied jitter.
        const double x=(double(v.renderedProjection[8])-v.unjitteredProjection[8])/projection.wsign*size.width/2;
        const double y=-(double(v.renderedProjection[9])-v.unjitteredProjection[9])/projection.wsign*size.height/2;
        constexpr double pixelTolerance=1.0/1024;
        if(std::abs(x-v.jitterX)>pixelTolerance||std::abs(y-v.jitterY)>pixelTolerance)
            return Reject::InvalidJitter;
        return Reject::None;
    }
    Reject Validate(const PairInput& p) const {
        if(!p.eyes[0]||!p.eyes[1]) return Reject::IncompletePair;
        if(p.frame.generation!=generation_) return Reject::WrongGeneration;
        if(!Complete(p.frame)) return Reject::FrameMismatch;
        if(p.frame.sourceFrame<=watermark_) return Reject::Replay;
        if(!ValidSize(p.renderSize)||!ValidSize(p.outputSize)||!p.configurationRevision||
            p.outputSize.width<p.renderSize.width||p.outputSize.height<p.renderSize.height) return Reject::InvalidExtent;
        for(unsigned eye=0;eye<2;++eye) {
            const auto& e=*p.eyes[eye];
            const std::array<const Stamp*,4> stamps{&e.color.stamp,&e.depth.stamp,&e.motion.stamp,&e.view.stamp};
            // Check all provenance first: an absent view record must not get
            // misreported as disagreement with an otherwise populated plane.
            for(const auto* s:stamps)
                if(s->association!=Association::FrameAssociated||!s->associationRecord) return Reject::Unassociated;
            for(const auto* s:stamps) {
                if(s->eye!=(eye?Eye::Right:Eye::Left)) return Reject::WrongEye;
                if(s->frame!=p.frame||s->associationRecord!=e.view.stamp.associationRecord) return Reject::FrameMismatch;
            }
            if(!ValidSurface(e.color.value,p.renderSize)||!ValidSurface(e.depth.value,p.renderSize)||
                !ValidSurface(e.motion.value.image,p.renderSize)) return Reject::InvalidExtent;
            const auto viewResult=ValidateView(e.view.value,p.renderSize);
            if(viewResult!=Reject::None) return viewResult;
            const auto& m=e.motion.value;
            if(m.coverage!=MotionCoverage::FullScene||m.convention!=MotionConvention::CurrentToPreviousRenderPixelsJitterExcluded||
                (!Empty(m.previous)&&(!Complete(m.previous)||m.previous.generation!=generation_||m.previous.sourceFrame>=p.frame.sourceFrame)))
                return Reject::InvalidMotion;
        }
        const auto& left=p.eyes[0]->view.value;const auto& right=p.eyes[1]->view.value;
        if(left.jitterPhase!=right.jitterPhase||left.jitterX!=right.jitterX||left.jitterY!=right.jitterY)
            return Reject::InvalidJitter;
        return Reject::None;
    }
};
}
