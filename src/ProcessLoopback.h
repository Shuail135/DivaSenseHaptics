#pragma once
#ifdef _WIN32
#include "HapticEngine.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class ProcessLoopback {
public:
    explicit ProcessLoopback(HapticEngine& engine):engine_(engine){}
    ~ProcessLoopback(){Stop();}
    bool Start(unsigned long processId);
    void Stop();
private:
    void threadMain(unsigned long pid);
    HapticEngine& engine_;
    std::thread thread_;
    std::atomic<bool> stop_{false}, running_{false};
    std::mutex readyMutex_;
    std::condition_variable readyCv_;
    bool ready_=false, startOk_=false;
};
#endif
