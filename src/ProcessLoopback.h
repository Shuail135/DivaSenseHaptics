#pragma once
#ifdef _WIN32
#include "Config.h"
#include "HapticEngine.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class ProcessLoopback {
public:
    ProcessLoopback(HapticEngine& engine,const ModConfig& cfg):engine_(engine),cfg_(cfg){}
    ~ProcessLoopback(){Stop();}
    bool Start(unsigned long processId);
    void Stop();
    bool Running() const {return running_.load();}
private:
    void threadMain(unsigned long pid);
    HapticEngine& engine_;
    ModConfig cfg_;
    std::thread thread_;
    std::atomic<bool> stop_{false}, running_{false};
    std::mutex readyMutex_;
    std::condition_variable readyCv_;
    bool ready_=false, startOk_=false;
};
#endif
