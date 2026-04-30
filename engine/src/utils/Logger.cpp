#include "utils/Logger.h"
#include <Windows.h>

void Logger::Info(const std::string& msg) {
    OutputDebugStringA(("[INFO] " + msg + "\n").c_str());
}

void Logger::Error(const std::string& msg) {
    OutputDebugStringA(("[ERROR] " + msg + "\n").c_str());
}
