#include "Log.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace {
std::mutex gMutex;
std::ofstream gFile;
void write(const char* level, const std::string& s) {
    std::lock_guard lock(gMutex);
    if (!gFile) return;
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    gFile << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
          << " [" << level << "] " << s << "\n";
    gFile.flush();
}
}
namespace Log {
void Init(const std::filesystem::path& path) {
    std::lock_guard lock(gMutex);
    gFile.open(path, std::ios::out | std::ios::app);
}
void Info(const std::string& s) { write("INFO", s); }
void Warn(const std::string& s) { write("WARN", s); }
void Error(const std::string& s) { write("ERROR", s); }
}
