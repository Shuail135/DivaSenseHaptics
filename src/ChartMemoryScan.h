#pragma once
#ifdef _WIN32
#include "DscChart.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

class ChartMemoryScan {
public:
    using Callback = std::function<void(DscChart)>;

    explicit ChartMemoryScan(Callback callback);
    ~ChartMemoryScan();

    bool Start();
    void Stop();
    void Request();

private:
    void threadMain();
    bool scanOnce();

    Callback callback_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool requested_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<bool> runningScan_{false};
    int noFindLogs_ = 0;
};
#endif
