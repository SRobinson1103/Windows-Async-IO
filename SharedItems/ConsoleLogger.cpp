#include "ConsoleLogger.h"

#include <iomanip>

void ConsoleLogger::log(LogLevel level, const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::string levelStr = logLevelToString(level);
    std::string timestamp = getCurrentTimestamp();

    std::cout << "[" << timestamp << "] [" << levelStr << "] " << message << std::endl;
}

void ConsoleLogger::setLogLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(mutex_);
    currentLogLevel_ = level;
}

bool ConsoleLogger::shouldLog(LogLevel level)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return level >= currentLogLevel_;
}

std::string ConsoleLogger::logLevelToString(LogLevel level)
{
    switch (level)
    {
    case LogLevel::VERBOSE: return "VERBOSE";
    case LogLevel::INFO: return "INFO";
    case LogLevel::WARNING: return "WARNING";
    case LogLevel::ERR: return "ERROR";
    default: return "UNKNOWN";
    }
}

std::string ConsoleLogger::getCurrentTimestamp()
{
    // Get the current time point from the system clock
    auto now = std::chrono::system_clock::now();

    // Convert to time_t to get calendar time
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);

    // Extract nanos from the duration since epoch
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration) % 1000;
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration) % 1000;
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration) % 1000;

    // Convert to local time
    std::tm localTime;
    localtime_s(&localTime, &now_c);

    // Format the timestamp
    std::ostringstream oss;
    oss << (localTime.tm_year + 1900) << "-"
        << std::setw(2) << std::setfill('0') << (localTime.tm_mon + 1) << "-"
        << std::setw(2) << std::setfill('0') << localTime.tm_mday << " "
        << std::setw(2) << std::setfill('0') << localTime.tm_hour << ":"
        << std::setw(2) << std::setfill('0') << localTime.tm_min  << ":"
        << std::setw(2) << std::setfill('0') << localTime.tm_sec << "."
        << std::setw(3) << std::setfill('0') << millis.count()
        << std::setw(3) << std::setfill('0') << micros.count()
        << std::setw(3) << std::setfill('0') << nanos.count();

    return oss.str();
}