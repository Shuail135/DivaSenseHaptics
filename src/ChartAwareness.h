#pragma once
#include "DscChart.h"
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>

struct ChartJudgementMatch {
    bool matched=false;
    DscChartGroup group{};
    bool challengeTransition=false;
    bool challengeActive=false;
};

class ChartAwareness {
public:
    using Clock=std::chrono::steady_clock;
    using TP=Clock::time_point;

    void SetChart(DscChart chart);
    void ClearChart();
    ChartJudgementMatch ObserveJudgement(TP when,bool slide,bool successNote);
    bool HasChart() const;

private:
    struct Obs { TP when{}; bool slide=false; bool success=false; };
    static double seconds(TP t);
    static bool shapeMatches(const Obs& o,const DscChartGroup& g);
    bool tryLockLocked(ChartJudgementMatch& latest);
    std::optional<size_t> findLockedMatchLocked(const Obs& o,double predicted) const;

    mutable std::mutex mutex_;
    DscChart chart_{};
    std::deque<Obs> recent_;
    bool locked_=false;
    double offset_=0.0; // wall seconds - chart seconds
    size_t lastIndex_=0;
    std::optional<bool> challengeState_;
    int misses_=0;
};
