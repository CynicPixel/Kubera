#include "core/utils/latency_tracker.h"
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <sstream>

namespace kubera {
namespace utils {

LatencyTracker::LatencyTracker(logging::Logger& logger)
    : logger_(logger) {
    logger_.info("Latency tracker initialized");
}

void LatencyTracker::startMeasurement(const std::string& operation) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Record start time
    auto now = std::chrono::high_resolution_clock::now();
    activeMeasurements_[operation] = {now, true};
    
    logger_.debug("Started measurement for operation '{}'", operation);
}

double LatencyTracker::endMeasurement(const std::string& operation) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Find active measurement
    auto it = activeMeasurements_.find(operation);
    if (it == activeMeasurements_.end() || !it->second.active) {
        logger_.warning("No active measurement found for operation '{}'", operation);
        return 0.0;
    }
    
    // Calculate duration
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(now - it->second.startTime).count();
    double durationMicros = static_cast<double>(duration) / 1000.0;
    
    // Mark as inactive
    it->second.active = false;
    
    // Store measurement
    Measurement measurement;
    measurement.operation = operation;
    measurement.durationMicros = durationMicros;
    measurement.timestamp = now;
    
    // Add to measurements queue
    measurements_.enqueue(measurement);
    
    // Update statistics
    updateStatistics(operation, durationMicros);
    
    logger_.debug("Ended measurement for operation '{}': {:.3f} µs", operation, durationMicros);
    
    return durationMicros;
}

void LatencyTracker::processMeasurements() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Process all measurements in the queue
    Measurement measurement;
    while (measurements_.try_dequeue(measurement)) {
        // Update statistics
        updateStatistics(measurement.operation, measurement.durationMicros);
    }
}

LatencyTracker::Statistics LatencyTracker::getStatistics(const std::string& operation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = statistics_.find(operation);
    if (it != statistics_.end()) {
        return it->second;
    }
    
    // Return default statistics if not found
    return Statistics{};
}

std::unordered_map<std::string, LatencyTracker::Statistics> LatencyTracker::getAllStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return statistics_;
}

std::string LatencyTracker::generateReport() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::stringstream report;
    report << "Latency Report:\n";
    report << "----------------\n";
    
    for (const auto& [operation, stats] : statistics_) {
        report << "Operation: " << operation << "\n";
        report << "  Min: " << std::fixed << std::setprecision(3) << stats.minMicros << " µs\n";
        report << "  Max: " << std::fixed << std::setprecision(3) << stats.maxMicros << " µs\n";
        report << "  Avg: " << std::fixed << std::setprecision(3) << stats.avgMicros << " µs\n";
        report << "  P99: " << std::fixed << std::setprecision(3) << stats.p99Micros << " µs\n";
        report << "  Count: " << stats.count << "\n";
        report << "\n";
    }
    
    return report.str();
}

void LatencyTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Clear all measurements and statistics
    Measurement measurement;
    while (measurements_.try_dequeue(measurement)) {
        // Discard measurement
    }
    
    statistics_.clear();
    activeMeasurements_.clear();
    
    logger_.info("Latency tracker reset");
}

void LatencyTracker::updateStatistics(const std::string& operation, double durationMicros) {
    // Get existing statistics or create new ones
    auto& stats = statistics_[operation];
    
    // Update count
    stats.count++;
    
    // Update min/max
    if (stats.count == 1 || durationMicros < stats.minMicros) {
        stats.minMicros = durationMicros;
    }
    if (stats.count == 1 || durationMicros > stats.maxMicros) {
        stats.maxMicros = durationMicros;
    }
    
    // Update average (using weighted approach)
    stats.avgMicros = ((stats.avgMicros * (stats.count - 1)) + durationMicros) / stats.count;
    
    // Update percentiles
    // This is a simplified approach; in a real system, we would use a more sophisticated algorithm
    // like a circular buffer or reservoir sampling to calculate percentiles accurately
    
    // Add to history
    stats.history.push_back(durationMicros);
    
    // Limit history size
    if (stats.history.size() > 1000) {
        stats.history.pop_front();
    }
    
    // Calculate p99
    if (stats.history.size() >= 100) {
        // Create a copy of the history for sorting
        std::vector<double> sortedHistory(stats.history.begin(), stats.history.end());
        std::sort(sortedHistory.begin(), sortedHistory.end());
        
        // Calculate p99 index
        size_t p99Index = static_cast<size_t>(sortedHistory.size() * 0.99);
        if (p99Index >= sortedHistory.size()) {
            p99Index = sortedHistory.size() - 1;
        }
        
        // Update p99
        stats.p99Micros = sortedHistory[p99Index];
    } else {
        // Not enough data for p99, use max
        stats.p99Micros = stats.maxMicros;
    }
}

} // namespace utils
} // namespace kubera
