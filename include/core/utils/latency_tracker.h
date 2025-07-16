#pragma once

#include <string>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <deque>
#include "concurrentqueue.h"
#include "logging/logger.h"

namespace kubera {
namespace utils {

/**
 * @class LatencyTracker
 * @brief Tracks latency metrics for various operations
 */
class LatencyTracker {
public:
    /**
     * @brief Constructor
     * @param logger The logger instance
     */
    LatencyTracker(logging::Logger& logger);
    
    /**
     * @brief Start a latency measurement
     * @param operation The operation name
     */
    void startMeasurement(const std::string& operation);
    
    /**
     * @brief End a latency measurement
     * @param operation The operation name
     * @return The duration in microseconds
     */
    double endMeasurement(const std::string& operation);
    
    /**
     * @brief Process all pending measurements
     */
    void processMeasurements();
    
    /**
     * @brief Statistics structure
     */
    struct Statistics {
        double minMicros = 0.0;
        double maxMicros = 0.0;
        double avgMicros = 0.0;
        double p99Micros = 0.0;
        uint64_t count = 0;
        std::deque<double> history;
    };
    
    /**
     * @brief Get statistics for an operation
     * @param operation The operation name
     * @return The statistics
     */
    Statistics getStatistics(const std::string& operation) const;
    
    /**
     * @brief Get all statistics
     * @return All statistics
     */
    std::unordered_map<std::string, Statistics> getAllStatistics() const;
    
    /**
     * @brief Generate a latency report
     * @return The report as a string
     */
    std::string generateReport() const;
    
    /**
     * @brief Reset all measurements and statistics
     */
    void reset();
    
private:
    // Measurement structure
    struct Measurement {
        std::string operation;
        double durationMicros;
        std::chrono::high_resolution_clock::time_point timestamp;
    };
    
    // Active measurement structure
    struct ActiveMeasurement {
        std::chrono::high_resolution_clock::time_point startTime;
        bool active;
    };
    
    // Measurements queue
    moodycamel::ConcurrentQueue<Measurement> measurements_;
    
    // Active measurements
    std::unordered_map<std::string, ActiveMeasurement> activeMeasurements_;
    
    // Statistics
    std::unordered_map<std::string, Statistics> statistics_;
    
    // Mutex for thread safety
    mutable std::mutex mutex_;
    
    // Logger
    logging::Logger& logger_;
    
    /**
     * @brief Update statistics for an operation
     * @param operation The operation name
     * @param durationMicros The duration in microseconds
     */
    void updateStatistics(const std::string& operation, double durationMicros);
};

} // namespace utils
} // namespace kubera
