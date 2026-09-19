#pragma once
#ifdef _WIN32
#include "Config.h"
#include "HapticEngine.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class DualSenseAudio {
public:
    DualSenseAudio(HapticEngine& e,const ModConfig& c):engine_(e),cfg_(c){}
    ~DualSenseAudio(){Stop();}
    bool Start();
    void Stop();
    bool Running()const{return running_.load();}
private:
    void threadMain();
    HapticEngine& engine_;ModConfig cfg_;
    std::thread thread_;std::atomic<bool>stop_{false},running_{false};
    std::mutex readyMutex_;std::condition_variable readyCv_;
    bool ready_=false,startOk_=false;
};
#endif
