#include "core/utils/configuration_manager.h"
#include <iostream>
#include <string_view>

namespace kubera {
namespace utils {

ConfigurationManager::ConfigurationManager(const std::string& filename, logging::Logger& logger)
    : configFile_(filename), logger_(logger) {
    initDefaults();
}

bool ConfigurationManager::loadConfig() {
    std::unique_lock<std::shared_mutex> lock(configMutex_);
    
    try {
        // Start with defaults
        config_ = defaults_;
        
        // Load from file if it exists
        std::ifstream file(configFile_);
        if (file.is_open()) {
            // Read file content into a string
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            
            // Parse JSON using simdjson
            simdjson::dom::parser parser;
            simdjson::dom::element json = parser.parse(content);
            
            // Parse JSON into config map
            for (auto [key, value] : json.get_object()) {
                std::string_view keyStr = key;
                std::string keyString(keyStr);
                
                // Handle different value types
                switch (value.type()) {
                    case simdjson::dom::element_type::INT64:
                        config_[keyString] = static_cast<int>(value.get_int64().value());
                        break;
                    case simdjson::dom::element_type::DOUBLE:
                        config_[keyString] = value.get_double().value();
                        break;
                    case simdjson::dom::element_type::STRING:
                        config_[keyString] = std::string(value.get_string().value());
                        break;
                    case simdjson::dom::element_type::BOOL:
                        config_[keyString] = value.get_bool().value();
                        break;
                    default:
                        logger_.warning("Unsupported JSON type for key: {}", keyString);
                        break;
                }
            }
            
            logger_.info("Configuration loaded from {}", configFile_);
            return true;
        } else {
            logger_.info("Configuration file not found, using defaults");
            return false;
        }
    } catch (const std::exception& e) {
        logger_.error("Failed to load configuration: {}", e.what());
        config_ = defaults_;
        return false;
    }
}

bool ConfigurationManager::saveConfig() {
    std::shared_lock<std::shared_mutex> lock(configMutex_);
    
    try {
        // Build JSON string manually since simdjson is primarily a parser
        std::stringstream json;
        json << "{\n";
        
        size_t count = 0;
        for (const auto& [key, value] : config_) {
            json << "    \"" << key << "\": ";
            
            std::visit([&json](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    json << "\"" << arg << "\"";
                } else if constexpr (std::is_same_v<T, bool>) {
                    json << (arg ? "true" : "false");
                } else {
                    json << arg;
                }
            }, value);
            
            if (++count < config_.size()) {
                json << ",\n";
            } else {
                json << "\n";
            }
        }
        
        json << "}\n";
        
        // Write to file
        std::ofstream file(configFile_);
        file << json.str();
        
        logger_.info("Configuration saved to {}", configFile_);
        return true;
    } catch (const std::exception& e) {
        logger_.error("Failed to save configuration: {}", e.what());
        return false;
    }
}

void ConfigurationManager::initDefaults() {
    // WebSocket settings
    defaults_["websocket.endpoint"] = std::string("wss://stream.binance.com:9443/ws/btcusdt@depth20@100ms");
    defaults_["websocket.reconnect_attempts"] = 10;
    defaults_["websocket.reconnect_delay_ms"] = 1000;
    
    // OrderBook settings
    defaults_["orderbook.max_levels"] = 100;
    
    // Model settings
    defaults_["model.volatility"] = 0.15;
    defaults_["model.permanent_impact_factor"] = 0.1;
    defaults_["model.temporary_impact_factor"] = 0.3;
    
    // UI settings
    defaults_["ui.refresh_rate_ms"] = 16;  // ~60 FPS
    
    // Thread settings
    defaults_["thread.market_data_core"] = 1;
    defaults_["thread.calculation_core"] = 2;
    defaults_["thread.ui_core"] = 3;
    
    // Memory settings
    defaults_["memory.pool_size"] = 1024;
    
    // Fee settings
    defaults_["fee.tier1.maker"] = 0.0008;
    defaults_["fee.tier1.taker"] = 0.0010;
    defaults_["fee.tier2.maker"] = 0.0006;
    defaults_["fee.tier2.taker"] = 0.0008;
    defaults_["fee.tier3.maker"] = 0.0004;
    defaults_["fee.tier3.taker"] = 0.0006;
    defaults_["fee.tier4.maker"] = 0.0002;
    defaults_["fee.tier4.taker"] = 0.0004;
    defaults_["fee.tier5.maker"] = 0.0000;
    defaults_["fee.tier5.taker"] = 0.0002;
}

} // namespace utils
} // namespace kubera
