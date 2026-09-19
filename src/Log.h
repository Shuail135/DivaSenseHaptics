#pragma once
#include <filesystem>
#include <string>

namespace Log {
    void Init(const std::filesystem::path& path);
    void Info(const std::string& s);
    void Warn(const std::string& s);
    void Error(const std::string& s);
}
