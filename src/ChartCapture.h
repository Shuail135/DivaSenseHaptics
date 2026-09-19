#pragma once
#ifdef _WIN32
#include "DscChart.h"
#include <Windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class ChartCapture {
public:
    using Callback=std::function<void(DscChart)>;
    explicit ChartCapture(Callback callback);
    ~ChartCapture();
    bool Start();
    void Stop();
    void OnOpened(HANDLE handle,const std::wstring& path);
private:
    struct Item{HANDLE handle=INVALID_HANDLE_VALUE;std::wstring path;};
    void threadMain();
    Callback callback_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    std::thread thread_;
};
#endif
