#pragma once

#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <ctime>
#include <sstream>

class ConsoleLogger
{
public:
    enum class LogLevel { VERBOSE, INFO, WARNING, ERR };
    
    // Set singleton instance
    static ConsoleLogger& getInstance()
    {
        static ConsoleLogger instance;
        return instance;
    }

    void log(LogLevel level, const std::string& message);

    void setLogLevel(LogLevel level);

    bool shouldLog(LogLevel level);

private:
    ConsoleLogger() : currentLogLevel_(LogLevel::INFO) {}
    ~ConsoleLogger() = default;

    ConsoleLogger(const ConsoleLogger&) = delete;
    ConsoleLogger& operator=(const ConsoleLogger&) = delete;

    std::mutex mutex_;
    LogLevel currentLogLevel_;

    std::string logLevelToString(LogLevel level);
    std::string getCurrentTimestamp();
};
