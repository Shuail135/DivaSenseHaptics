#pragma once
#ifdef _WIN32
#include "Config.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class DualSenseHid {
public:
    using ReportCallback=std::function<void(const unsigned char*,size_t)>;
    explicit DualSenseHid(const ModConfig& cfg):cfg_(cfg){}
    ~DualSenseHid(){Stop();}
    bool Start(ReportCallback cb);
    void Stop();
    bool Running()const{return running_.load();}
    bool HasRecentInput(unsigned maxAgeMs=500) const;
private:
    void threadMain();
    bool openController();
    void closeController();
    bool sendAudioHapticsEnable();

    ModConfig cfg_;
    ReportCallback callback_;
    // Keep input and output/control handles separate. Steam or the game can
    // sometimes make a read/write HID open fail even though read-only input
    // remains available. Gameplay haptics only require the read handle.
    void* inputHandle_=reinterpret_cast<void*>(-1);
    void* controlHandle_=reinterpret_cast<void*>(-1);
    unsigned inputReportBytes_=64;
    unsigned outputReportBytes_=48;
    std::atomic<unsigned long long> lastInputTick_{0};
    std::thread thread_;
    std::atomic<bool> stop_{false},running_{false};
    std::mutex readyMutex_;std::condition_variable readyCv_;
    bool ready_=false,startOk_=false;
};
#endif
