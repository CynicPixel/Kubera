#pragma once

#include <array>
#include <vector>
#include <deque>
#include <shared_mutex>
#include <atomic>
#include "core/orderbook/orderbook.h"
#include "core/utils/memory_pool.h"
#include "core/models/shared_cache.h"
#include "logging/logger.h"
#include "concurrentqueue.h"


namespace kubera {
namespace models {

class MarketImpactModel {
public:
    static constexpr size_t CACHE_SIZE = 1024;
    static constexpr size_t POWER_CACHE_SIZE = 1000;
    static constexpr size_t MAX_DEPTH_LEVELS = 10;
    static constexpr size_t MAX_FEATURES = 8;
    static constexpr size_t MAX_HISTORY = 1000;

    struct ImpactComponents {
        double immediate;  // Immediate impact (spread crossing)
        double permanent;  // Permanent price impact
        double temporary;  // Temporary impact that decays
        double total;      // Sum of all components
        
        ImpactComponents(double imm = 0.0, double perm = 0.0, double temp = 0.0)
            : immediate(imm), permanent(perm), temporary(temp), total(imm + perm + temp) {}
    };

    struct ModelParameters {
        double volatility;
        double permanent_impact_factor;
        double temporary_impact_factor;
        double liquidity_factor;
        double price_impact_decay;
    };

    struct ImpactData {
        std::array<double, MAX_FEATURES> market_features;
        ImpactComponents actual_impact;
        uint64_t timestamp;
    };

private:
    ModelParameters parameters_;
    utils::MemoryPool<1024, 1024>& memory_pool_;
    logging::Logger& logger_;
    SharedCacheManager& shared_cache_;
    
    // Power caches for efficient computation
    std::vector<double> power_cache_025_;
    std::vector<double> power_cache_050_;
    std::vector<double> power_cache_075_;
    
    // Pre-computed impact curves
    mutable std::array<double, CACHE_SIZE> permanent_impact_curve_;
    mutable std::array<double, CACHE_SIZE> temporary_impact_curve_;
    mutable std::atomic<bool> curves_valid_;
    
    // Thread safety
    mutable std::shared_mutex parameters_mutex_;
    mutable std::mutex history_mutex_;
    
    // Training data management
    mutable std::deque<ImpactData> historical_data_;
    mutable moodycamel::ConcurrentQueue<ImpactData> new_impact_records_;
    mutable std::atomic<size_t> history_size_;
    mutable std::atomic<size_t> pending_records_count_;
    
    // Internal methods
    void initialize_power_caches();
    void initialize_impact_curves() const;
    double lookup_power(double value, double exponent, const std::vector<double>& cache) const;
    double calculate_power_direct(double value, double exponent) const;
    double calculate_atomic_depth(const std::array<double, 5>& quantities, bool is_buy) const;
    double calculate_effective_depth_from_snapshot(const kubera::orderbook::OrderBookSnapshot& snapshot, bool is_buy) const;
    
    // Background processing
    void process_new_impact_records() const;
    void process_impact_records_batch() const;
    void calibrateModel() const;

public:
    MarketImpactModel(
        logging::Logger& logger,
        utils::MemoryPool<1024, 1024>& memory_pool,
        SharedCacheManager& shared_cache,
        const ModelParameters& params = ModelParameters{}
    );
    
    // ✅ NEW: HFT optimized calculation
    ImpactComponents calculate_impact_from_atomics(
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        const std::array<double, 5>& ask_prices,
        const std::array<double, 5>& ask_quantities,
        const std::array<double, 5>& bid_prices,
        const std::array<double, 5>& bid_quantities,
        bool is_buy) const;
    
    // ✅ Direct calculation method
    ImpactComponents calculate_impact_direct(
        double quantity,
        double mid_price,
        double spread,
        double effective_depth,
        double volatility,
        double imbalance,
        double bid_depth,
        double ask_depth,
        bool is_buy) const;
    
    // ✅ Keep original method for backwards compatibility
    ImpactComponents calculateImpact(double quantity, const orderbook::OrderBook& order_book, bool is_buy) const;
    
    // Parameter management
    void update_parameters(const ModelParameters& new_params);
    ModelParameters get_parameters() const;
    
    // Individual parameter setters/getters
    void setVolatility(double volatility);
    void setPermanentImpactFactor(double factor);
    void setTemporaryImpactFactor(double factor);
    
    double getVolatility() const;
    double getPermanentImpactFactor() const;
    double getTemporaryImpactFactor() const;
    
    // Model training
    void updateModel(double quantity, const orderbook::OrderBook& orderBook, bool isBuy, const ImpactComponents& actualImpact);
    void triggerBackgroundProcessing();
    
    // Cache management
    void invalidate_cache();
    void force_cache_update(const orderbook::OrderBook& order_book);
};

} // namespace models
} // namespace kubera
