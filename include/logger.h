#pragma once

#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdint>

namespace ia64 {

enum class LogLevel {
    ERROR = 0,
    WARN = 1,
    INFO = 2,
    DEBUG = 3,
    TRACE = 4
};

class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void setLogLevel(LogLevel level) {
        logLevel_ = level;
    }

    LogLevel getLogLevel() const {
        return logLevel_;
    }

    void log(LogLevel level, const std::string& message) {
        if (level <= logLevel_) {
            std::cout << "[" << getLevelString(level) << "] " << message << "\n";
        }
    }

    void logError(const std::string& message) {
        log(LogLevel::ERROR, message);
    }

    void logWarn(const std::string& message) {
        log(LogLevel::WARN, message);
    }

    void logInfo(const std::string& message) {
        log(LogLevel::INFO, message);
    }

    void logDebug(const std::string& message) {
        log(LogLevel::DEBUG, message);
    }

    void logTrace(const std::string& message) {
        log(LogLevel::TRACE, message);
    }

    void logInstruction(uint64_t address, const std::string& disassembly) {
        if (logLevel_ >= LogLevel::DEBUG) {
            std::stringstream ss;
            ss << "EXEC @ 0x" << std::hex << std::setw(16) << std::setfill('0') 
               << address << std::dec << ": " << disassembly;
            log(LogLevel::DEBUG, ss.str());
        }
    }

    void logRegisterChange(const std::string& regName, uint64_t oldValue, uint64_t newValue) {
        if (logLevel_ >= LogLevel::TRACE && oldValue != newValue) {
            std::stringstream ss;
            ss << "REG " << regName << ": 0x" << std::hex << std::setw(16) << std::setfill('0') 
               << oldValue << " -> 0x" << std::setw(16) << std::setfill('0') << newValue << std::dec;
            log(LogLevel::TRACE, ss.str());
        }
    }

    void logMemoryRead(uint64_t address, uint64_t value, size_t size) {
        if (logLevel_ >= LogLevel::TRACE) {
            std::stringstream ss;
            ss << "MEM READ  @ 0x" << std::hex << std::setw(16) << std::setfill('0') 
               << address << std::dec << " [" << size << " bytes]: 0x" 
               << std::hex << value << std::dec;
            log(LogLevel::TRACE, ss.str());
        }
    }

    void logMemoryWrite(uint64_t address, uint64_t value, size_t size) {
        if (logLevel_ >= LogLevel::TRACE) {
            std::stringstream ss;
            ss << "MEM WRITE @ 0x" << std::hex << std::setw(16) << std::setfill('0') 
               << address << std::dec << " [" << size << " bytes]: 0x" 
               << std::hex << value << std::dec;
            log(LogLevel::TRACE, ss.str());
        }
    }

    void logBundleFetch(uint64_t address, uint8_t templateType) {
        if (logLevel_ >= LogLevel::DEBUG) {
            std::stringstream ss;
            ss << "FETCH @ 0x" << std::hex << std::setw(16) << std::setfill('0') 
               << address << std::dec << " [Template: 0x" << std::hex 
               << static_cast<int>(templateType) << std::dec << "]";
            log(LogLevel::DEBUG, ss.str());
        }
    }

private:
    Logger() : logLevel_(LogLevel::INFO) {}
    
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    const char* getLevelString(LogLevel level) const {
        switch (level) {
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::TRACE: return "TRACE";
            default:              return "?????";
        }
    }

    LogLevel logLevel_;
};

inline void LOG_ERROR(const std::string& msg) { Logger::getInstance().logError(msg); }
inline void LOG_WARN(const std::string& msg)  { Logger::getInstance().logWarn(msg); }
inline void LOG_INFO(const std::string& msg)  { Logger::getInstance().logInfo(msg); }
inline void LOG_DEBUG(const std::string& msg) { Logger::getInstance().logDebug(msg); }
inline void LOG_TRACE(const std::string& msg) { Logger::getInstance().logTrace(msg); }

} // namespace ia64
