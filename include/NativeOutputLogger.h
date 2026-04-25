#pragma once

#include <Windows.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>

namespace ia64 {

class NativeOutputLogger {
public:
    static void Initialize() {
        static std::once_flag initFlag;
        std::call_once(initFlag, []() {
            Instance().initialize();
        });
    }

    static const std::string& LogDirectory() {
        Initialize();
        return Instance().logDirectory_;
    }

    static const std::string& StdoutLogPath() {
        Initialize();
        return Instance().stdoutLogPath_;
    }

    static const std::string& StderrLogPath() {
        Initialize();
        return Instance().stderrLogPath_;
    }

private:
    class TeeStreamBuf : public std::streambuf {
    public:
        TeeStreamBuf(std::streambuf* primary, std::streambuf* secondary)
            : primary_(primary), secondary_(secondary) {}

    protected:
        int overflow(int ch) override {
            if (ch == traits_type::eof()) {
                return traits_type::not_eof(ch);
            }

            const int first = primary_ ? primary_->sputc(static_cast<char>(ch)) : ch;
            const int second = secondary_ ? secondary_->sputc(static_cast<char>(ch)) : ch;
            return (first == traits_type::eof() || second == traits_type::eof())
                ? traits_type::eof()
                : ch;
        }

        int sync() override {
            const int first = primary_ ? primary_->pubsync() : 0;
            const int second = secondary_ ? secondary_->pubsync() : 0;
            return (first == 0 && second == 0) ? 0 : -1;
        }

    private:
        std::streambuf* primary_;
        std::streambuf* secondary_;
    };

    static NativeOutputLogger& Instance() {
        static NativeOutputLogger instance;
        return instance;
    }

    ~NativeOutputLogger() {
        if (originalCout_ != nullptr) {
            std::cout.rdbuf(originalCout_);
        }

        if (originalCerr_ != nullptr) {
            std::cerr.rdbuf(originalCerr_);
        }

        if (stdoutFile_) {
            stdoutFile_->flush();
        }

        if (stderrFile_) {
            stderrFile_->flush();
        }
    }

    void initialize() {
        logDirectory_ = getExecutableDirectory() + "\\logs";
        CreateDirectoryA(logDirectory_.c_str(), nullptr);

        const std::string timestamp = makeTimestamp();
        stdoutLogPath_ = logDirectory_ + "\\native-" + timestamp + ".log";
        stderrLogPath_ = logDirectory_ + "\\native-error-" + timestamp + ".log";

        stdoutFile_ = std::make_unique<std::ofstream>(stdoutLogPath_, std::ios::out | std::ios::app);
        stderrFile_ = std::make_unique<std::ofstream>(stderrLogPath_, std::ios::out | std::ios::app);

        if (stdoutFile_ && stdoutFile_->is_open()) {
            originalCout_ = std::cout.rdbuf();
            coutTee_ = std::make_unique<TeeStreamBuf>(originalCout_, stdoutFile_->rdbuf());
            std::cout.rdbuf(coutTee_.get());
            std::cout << "[" << currentIsoTime() << "] native stdout logging started" << std::endl;
            std::cout << "Log file: " << stdoutLogPath_ << std::endl;
        }

        if (stderrFile_ && stderrFile_->is_open()) {
            originalCerr_ = std::cerr.rdbuf();
            cerrTee_ = std::make_unique<TeeStreamBuf>(originalCerr_, stderrFile_->rdbuf());
            std::cerr.rdbuf(cerrTee_.get());
            std::cerr << "[" << currentIsoTime() << "] native stderr logging started" << std::endl;
            std::cerr << "Log file: " << stderrLogPath_ << std::endl;
        }
    }

    static std::string getExecutableDirectory() {
        char path[MAX_PATH] = {};
        DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (length == 0 || length == MAX_PATH) {
            return ".";
        }

        std::string fullPath(path, length);
        size_t slash = fullPath.find_last_of("\\/");
        return slash == std::string::npos ? "." : fullPath.substr(0, slash);
    }

    static std::string makeTimestamp() {
        auto now = std::chrono::system_clock::now();
        std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm localTime = {};
        localtime_s(&localTime, &nowTime);

        std::ostringstream stream;
        stream << std::put_time(&localTime, "%Y%m%d-%H%M%S");
        return stream.str();
    }

    static std::string currentIsoTime() {
        auto now = std::chrono::system_clock::now();
        std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm localTime = {};
        localtime_s(&localTime, &nowTime);

        std::ostringstream stream;
        stream << std::put_time(&localTime, "%Y-%m-%dT%H:%M:%S");
        return stream.str();
    }

    std::string logDirectory_;
    std::string stdoutLogPath_;
    std::string stderrLogPath_;
    std::unique_ptr<std::ofstream> stdoutFile_;
    std::unique_ptr<std::ofstream> stderrFile_;
    std::streambuf* originalCout_ = nullptr;
    std::streambuf* originalCerr_ = nullptr;
    std::unique_ptr<TeeStreamBuf> coutTee_;
    std::unique_ptr<TeeStreamBuf> cerrTee_;
};

} // namespace ia64
