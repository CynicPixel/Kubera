#pragma once

#include <string>
#include <unordered_map>
#include <variant>
#include <mutex>
#include <shared_mutex>
#include <fstream>
#include <simdjson.h>
#include "logging/logger.h"

namespace kubera {
namespace utils {

/**
 * @class ConfigurationManager
 * @brief Manages configuration settings for the application
 */
class ConfigurationManager {
public:
    /**
     * @brief Constructor
     * @param filename The configuration file name
     * @param logger The logger instance
     */
    ConfigurationManager(const std::string& filename, logging::Logger& logger);
    
    /**
     * @brief Load configuration from file
     * @return True if successful, false otherwise
     */
    bool loadConfig();
    
    /**
     * @brief Save configuration to file
     * @return True if successful, false otherwise
     */
    bool saveConfig();
    
    /**
     * @brief Get a configuration value
     * @tparam T The value type
     * @param key The configuration key
     * @param defaultValue The default value if key not found
     * @return The configuration value
     */
    template<typename T>
    T get(const std::string& key, const T& defaultValue = T()) const {
        std::shared_lock<std::shared_mutex> lock(configMutex_);
        
        try {
            auto it = config_.find(key);
            if (it != config_.end()) {
                return std::get<T>(it->second);
            } else {
                auto defaultIt = defaults_.find(key);
                if (defaultIt != defaults_.end()) {
                    return std::get<T>(defaultIt->second);
                }
            }
        } catch (const std::exception& e) {
            logger_.error("Failed to get configuration value for key {}: {}", key, e.what());
        }
        
        return defaultValue;
    }
    
    /**
     * @brief Set a configuration value
     * @tparam T The value type
     * @param key The configuration key
     * @param value The configuration value
     */
    template<typename T>
    void set(const std::string& key, const T& value) {
        std::unique_lock<std::shared_mutex> lock(configMutex_);
        config_[key] = value;
    }
    
private:
    // Configuration values
    std::unordered_map<std::string, std::variant<int, double, std::string, bool>> config_;
    
    // Default values
    std::unordered_map<std::string, std::variant<int, double, std::string, bool>> defaults_;
    
    // File path
    std::string configFile_;
    
    // Thread safety
    mutable std::shared_mutex configMutex_;
    
    // Logger
    logging::Logger& logger_;
    
    // Initialize default values
    void initDefaults();
};

} // namespace utils
} // namespace kubera
