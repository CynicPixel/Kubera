#include "logging/logger.h"

namespace kubera {
namespace logging {

Logger::Logger(const std::string& filename, spdlog::level::level_enum level) {
    try {
        // Create a console sink
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(level);
        
        // Create a file sink
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename, true);
        file_sink->set_level(level);
        
        // Create a logger with both sinks
        logger_ = std::make_shared<spdlog::logger>("kubera", spdlog::sinks_init_list{console_sink, file_sink});
        logger_->set_level(level);
        
        // Set as default logger
        spdlog::set_default_logger(logger_);
        
        // Set pattern
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
        
        logger_->info("Logger initialized");
    }
    catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Logger initialization failed: " << ex.what() << std::endl;
    }
}

void Logger::setLevel(spdlog::level::level_enum level) {
    logger_->set_level(level);
}

} // namespace logging
} // namespace kubera
