#include "ChartAwareness.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

double ChartAwareness::seconds(TP t){return std::chrono::duration<double>(t.time_since_epoch()).count();}

bool ChartAwareness::shapeMatches(const Obs&o,const DscChartGroup&g){
    const bool gs=g.slideMask!=0;
    if(o.slide!=gs) return false;
    if(o.success && !(g.specialFaceMask||g.specialSlideMask)) return false;
    return g.faceMask!=0 || g.slideMask!=0;
}

void ChartAwareness::SetChart(DscChart chart){
    std::lock_guard lock(mutex_);
    chart_=std::move(chart); recent_.clear(); locked_=false; offset_=0; lastIndex_=0; lastMatched_={}; challengeState_.reset(); misses_=0;
    if(chart_.valid) Log::Info("Chart awareness loaded DSC; waiting for confirmed judgements to lock chart timing.");
}

void ChartAwareness::ClearChart(){
    std::lock_guard lock(mutex_);
    chart_={}; recent_.clear(); locked_=false; offset_=0; lastIndex_=0; lastMatched_={}; challengeState_.reset(); misses_=0;
}

bool ChartAwareness::HasChart() const{std::lock_guard lock(mutex_);return chart_.valid;}
bool ChartAwareness::IsLocked() const{std::lock_guard lock(mutex_);return locked_;}

bool ChartAwareness::tryLockLocked(ChartJudgementMatch& latest){
    if(!chart_.valid || recent_.size()<4) return false;
    const int use=std::min<int>(6,static_cast<int>(recent_.size()));
    std::vector<Obs> obs(recent_.end()-use,recent_.end());
    double bestAvg=std::numeric_limits<double>::infinity();
    std::vector<size_t> bestIndices;

    for(size_t start=0;start<chart_.groups.size();++start){
        if(!shapeMatches(obs[0],chart_.groups[start])) continue;
        const double candidateOffset=seconds(obs[0].when)-chart_.groups[start].hitSeconds;
        std::vector<size_t> idx{start};
        size_t prev=start;
        double total=0.0,maxErr=0.0;
        bool ok=true;
        for(int j=1;j<use;++j){
            const double predicted=seconds(obs[j].when)-candidateOffset;
            size_t bestK=0; double bestErr=0.095; bool found=false;
            const size_t stop=std::min(chart_.groups.size(),prev+5);
            for(size_t k=prev+1;k<stop;++k){
                if(!shapeMatches(obs[j],chart_.groups[k])) continue;
                const double e=std::abs(chart_.groups[k].hitSeconds-predicted);
                if(e<bestErr){bestErr=e;bestK=k;found=true;}
            }
            if(!found){ok=false;break;}
            total+=bestErr; maxErr=std::max(maxErr,bestErr); idx.push_back(bestK); prev=bestK;
        }
        if(!ok) continue;
        const double avg=total/std::max(1,use-1);
        if(avg<bestAvg && avg<=0.040 && maxErr<=0.085){
            bestAvg=avg; bestIndices=std::move(idx);
        }
    }
    if(bestIndices.empty()) return false;

    // Average all observed offsets for a more stable initial lock.
    double sum=0.0;
    for(size_t j=0;j<bestIndices.size();++j) sum+=seconds(obs[j].when)-chart_.groups[bestIndices[j]].hitSeconds;
    offset_=sum/static_cast<double>(bestIndices.size());
    locked_=true; lastIndex_=bestIndices.back(); lastMatched_=obs.back().when; misses_=0;
    challengeState_=chart_.groups[lastIndex_].challenge;
    latest.chartAvailable=true; latest.locked=true; latest.matched=true; latest.group=chart_.groups[lastIndex_]; latest.groupIndex=static_cast<int>(lastIndex_);
    latest.timingErrorMs=(seconds(obs.back().when)-offset_-latest.group.hitSeconds)*1000.0;
    latest.challengeActive=latest.group.challenge;
    Log::Info("DSC chart timing locked; average residual="+std::to_string(bestAvg*1000.0)+" ms, group="+std::to_string(lastIndex_)+".");
    return true;
}

std::optional<size_t> ChartAwareness::findLockedMatchLocked(const Obs&o,double predicted) const{
    if(!chart_.valid) return std::nullopt;
    size_t begin=lastIndex_+1;
    if(begin>=chart_.groups.size()) return std::nullopt;
    // Search a small sequential window first; this tolerates a few missed/unreported groups.
    size_t stop=std::min(chart_.groups.size(),begin+7);
    size_t best=0; double bestErr=0.140; bool found=false;
    for(size_t k=begin;k<stop;++k){
        if(!shapeMatches(o,chart_.groups[k])) continue;
        const double e=std::abs(chart_.groups[k].hitSeconds-predicted);
        if(e<bestErr){bestErr=e;best=k;found=true;}
    }
    if(found) return best;
    return std::nullopt;
}

ChartJudgementMatch ChartAwareness::ObserveJudgement(TP when,bool slide,bool successNote){
    ChartJudgementMatch out;
    std::lock_guard lock(mutex_);
    out.chartAvailable=chart_.valid; out.locked=locked_;
    if(!chart_.valid) return out;
    Obs o{when,slide,successNote};
    recent_.push_back(o); while(recent_.size()>10)recent_.pop_front();

    if(!locked_){
        tryLockLocked(out);
        return out;
    }

    const double predicted=seconds(when)-offset_;
    auto idx=findLockedMatchLocked(o,predicted);
    if(!idx){
        ++misses_;
        if(misses_>=2){
            locked_=false; challengeState_.reset(); misses_=0;
            Log::Warn("DSC chart timing lost; relocking from confirmed judgements.");
            tryLockLocked(out);
        }
        out.locked=locked_;
        return out;
    }

    misses_=0; lastIndex_=*idx; lastMatched_=when;
    const auto&g=chart_.groups[*idx];
    const double measured=seconds(when)-g.hitSeconds;
    offset_=offset_*0.94+measured*0.06;
    out.locked=true; out.matched=true; out.group=g; out.groupIndex=static_cast<int>(*idx);
    out.timingErrorMs=(seconds(when)-offset_-g.hitSeconds)*1000.0;
    out.challengeActive=g.challenge;
    if(challengeState_.has_value() && *challengeState_!=g.challenge) out.challengeTransition=true;
    challengeState_=g.challenge;
    return out;
}
