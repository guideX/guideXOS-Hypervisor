#pragma once

#include "NativeOutputLogger.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

namespace ia64 {

class BootStageTrace {
public:
    static void Stage(int stage, const std::string& label, const std::string& context = {}) {
        Instance().writeStage(stage, label, context);
    }

    static void Event(const std::string& tag, const std::string& context = {}) {
        Instance().writeLine("BOOT_EVENT " + tag, context);
    }

    static void EventOnce(const std::string& tag, const std::string& context = {}) {
        Instance().writeEventOnce(tag, context);
    }

    static std::string Hex(uint64_t value) {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << value;
        return oss.str();
    }

    static const std::string& Path() {
        return Instance().path_;
    }

private:
    static BootStageTrace& Instance() {
        static BootStageTrace instance;
        return instance;
    }

    BootStageTrace() {
        NativeOutputLogger::Initialize();
        path_ = NativeOutputLogger::LogDirectory() + "\\boot-trace-" + timestampForFile() + ".log";
        stream_.open(path_, std::ios::out | std::ios::app);
        writeLineLocked("BOOT_TRACE_BEGIN", "path=\"" + path_ + "\"");
    }

    void writeStage(int stage, const std::string& label, const std::string& context) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!seenStages_.insert(stage).second) {
            return;
        }

        std::ostringstream tag;
        tag << "BOOT_STAGE_" << std::setw(3) << std::setfill('0') << stage << " " << label;
        writeLineLocked(tag.str(), context);
    }

    void writeEventOnce(const std::string& tag, const std::string& context) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!seenEvents_.insert(tag).second) {
            return;
        }
        writeLineLocked("BOOT_EVENT " + tag, context);
    }

    void writeLine(const std::string& tag, const std::string& context) {
        std::lock_guard<std::mutex> lock(mutex_);
        writeLineLocked(tag, context);
    }

    void writeLineLocked(const std::string& tag, const std::string& context) {
        if (!stream_.is_open()) {
            return;
        }

        stream_ << timestampIso() << " " << tag;
        if (!context.empty()) {
            stream_ << " " << context;
        }
        stream_ << "\n";
        stream_.flush();
    }

    static std::string timestampForFile() {
        auto now = std::chrono::system_clock::now();
        std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm localTime = {};
        localtime_s(&localTime, &nowTime);

        std::ostringstream oss;
        oss << std::put_time(&localTime, "%Y%m%d-%H%M%S");
        return oss.str();
    }

    static std::string timestampIso() {
        auto now = std::chrono::system_clock::now();
        std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm localTime = {};
        localtime_s(&localTime, &nowTime);

        std::ostringstream oss;
        oss << std::put_time(&localTime, "%Y-%m-%dT%H:%M:%S");
        return oss.str();
    }

    std::mutex mutex_;
    std::ofstream stream_;
    std::string path_;
    std::set<int> seenStages_;
    std::set<std::string> seenEvents_;
};

} // namespace ia64
