#pragma once

#include <iostream>
#include <string>
#include <fstream>
#include <mutex>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace kubera {
namespace logging {

/**
 * @class Logger
 * @brief A thread-safe logging class that wraps spdlog functionality
 */
class Logger {
public:
    /**
     * @brief Constructor
     * @param filename The log file name
     * @param level The log level (default: info)
     */
    Logger(const std::string& filename = "kubera.log", spdlog::level::level_enum level = spdlog::level::info);
    
    /**
     * @brief Destructor
     */
    ~Logger() = default;
    
    /**
     * @brief Log a debug message
     * @param format The message format string
     * @param args The format arguments
     */
    template<typename... Args>
    void debug(const std::string& format, Args&&... args) {
        logger_->debug(format, std::forward<Args>(args)...);
    }
    
    /**
     * @brief Log an info message
     * @param format The message format string
     * @param args The format arguments
     */
    template<typename... Args>
    void info(const std::string& format, Args&&... args) {
        logger_->info(format, std::forward<Args>(args)...);
    }
    
    /**
     * @brief Log a warning message
     * @param format The message format string
     * @param args The format arguments
     */
    template<typename... Args>
    void warning(const std::string& format, Args&&... args) {
        logger_->warn(format, std::forward<Args>(args)...);
    }
    
    /**
     * @brief Log an error message
     * @param format The message format string
     * @param args The format arguments
     */
    template<typename... Args>
    void error(const std::string& format, Args&&... args) {
        logger_->error(format, std::forward<Args>(args)...);
    }
    
    /**
     * @brief Log a critical message
     * @param format The message format string
     * @param args The format arguments
     */
    template<typename... Args>
    void critical(const std::string& format, Args&&... args) {
        logger_->critical(format, std::forward<Args>(args)...);
    }
    
    /**
     * @brief Set the log level
     * @param level The new log level
     */
    void setLevel(spdlog::level::level_enum level);
    
    /**
     * @brief Get the underlying spdlog logger
     * @return The spdlog logger
     */
    std::shared_ptr<spdlog::logger> getLogger() const {
        return logger_;
    }
    
private:
    std::shared_ptr<spdlog::logger> logger_;
};

} // namespace logging
} // namespace kubera
