#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
#include <chrono>
#include <shared_mutex>
#include <memory>
#include "core/orderbook/orderbook.h"
#include "core/utils/memory_pool.h"
#include "core/models/shared_cache.h"
#include "logging/logger.h"
#include "concurrentqueue.h"

namespace kubera {
namespace models {

class MarketImpactModel {
public:
    struct ImpactComponents {
        double immediate;
        double permanent;
        double temporary;
        double total;
        
        ImpactComponents(double imm = 0.0, double perm = 0.0, double temp = 0.0)
            : immediate(imm), permanent(perm), temporary(temp), total(imm + perm + temp) {}
    };
    
    struct ModelParameters {
        double volatility;
        double permanent_impact_factor;
        double temporary_impact_factor;
        double liquidity_factor;
        double price_impact_decay;
        
        ModelParameters(double vol = 0.02, double perm = 0.1, double temp = 0.05, 
                       double liq = 1.0, double decay = 0.95)
            : volatility(vol), permanent_impact_factor(perm), temporary_impact_factor(temp),
              liquidity_factor(liq), price_impact_decay(decay) {}
    };

private:
    static constexpr size_t MAX_DEPTH_LEVELS = 20;
    static constexpr size_t CACHE_SIZE = 1024;
    static constexpr size_t POWER_CACHE_SIZE = 1000;
    static constexpr size_t MAX_HISTORY = 1000;
    static constexpr uint64_t CACHE_EXPIRY_NS = 500'000; // 500μs cache expiry
    
    // ✅ REMOVED: Individual cache members
    // mutable std::atomic<CacheData*> current_cache_;
    // mutable std::atomic<CacheData*> pending_delete_;
    
    // ✅ ADDED: Shared cache reference
    SharedCacheManager& shared_cache_;
    
    // Thread-safe parameter management
    mutable std::shared_mutex parameters_mutex_;
    ModelParameters parameters_;
    
    // Pre-computed curves for performance
    mutable std::array<double, CACHE_SIZE> permanent_impact_curve_;
    mutable std::array<double, CACHE_SIZE> temporary_impact_curve_;
    mutable std::atomic<bool> curves_valid_;
    
    // Power caches for efficiency
    std::vector<double> power_cache_025_;
    std::vector<double> power_cache_050_;
    std::vector<double> power_cache_075_;
    
    // Historical data for calibration
    struct ImpactData {
        std::array<double, 8> market_features; // mid, spread, depth, vol, imb, bid_depth, ask_depth, quantity
        ImpactComponents actual_impact;
        uint64_t timestamp;
    };
    
    mutable moodycamel::ConcurrentQueue<ImpactData> new_impact_records_;
    mutable std::atomic<size_t> pending_records_count_;
    mutable std::deque<ImpactData> historical_data_;
    mutable std::atomic<size_t> history_size_;
    mutable std::mutex history_mutex_;
    
    // External resources
    utils::MemoryPool<1024, 1024>& memory_pool_;
    logging::Logger& logger_;
    
    // ✅ UPDATED: Private methods using shared cache
    SharedCacheManager::CacheSnapshot get_cache_snapshot() const;
    void update_shared_cache(const orderbook::OrderBook& order_book) const;
    void update_shared_cache_from_snapshot(const OrderBookSnapshot& snapshot) const;
    
    // ✅ UPDATED: Impact calculation methods
    ImpactComponents calculate_impact_from_shared_cache(
        double quantity,
        const SharedMarketCache* cache,  // ✅ CHANGED: SharedMarketCache instead of CacheData
        bool is_buy
    ) const;
    
    ImpactComponents calculate_impact_direct(
        double quantity,
        double mid_price,
        double spread,
        double effective_depth,
        double volatility,
        double imbalance,
        double bid_depth,
        double ask_depth,
        bool is_buy
    ) const;
    
    ImpactComponents calculate_impact_from_snapshot(
        double quantity,
        const OrderBookSnapshot& snapshot,
        bool is_buy
    ) const;
    
    // ✅ UPDATED: Depth calculation methods
    double calculate_effective_depth_from_shared_cache(const SharedMarketCache* cache, bool is_buy) const;
    double calculate_effective_depth_from_snapshot(
        const OrderBookSnapshot& snapshot,
        bool is_buy
    ) const;
    double calculate_atomic_depth(
        const std::array<double, 5>& quantities,
        bool is_buy
    ) const;
    
    // Power and curve methods
    void initialize_power_caches();
    void initialize_impact_curves() const;
    double lookup_power(double value, double exponent, const std::vector<double>& cache) const;
    double calculate_power_direct(double value, double exponent) const;
    
    // Background processing
    void process_new_impact_records() const;
    void process_impact_records_batch() const;
    void calibrateModel() const;
    
    // Parameter update helpers
    void update_parameters_internal(const ModelParameters& new_params);

public:
    // ✅ UPDATED: Constructor with shared cache parameter
    MarketImpactModel(
        logging::Logger& logger,
        utils::MemoryPool<1024, 1024>& memory_pool,
        SharedCacheManager& shared_cache = g_shared_cache,  // ✅ ADDED: shared cache parameter
        const ModelParameters& params = ModelParameters()
    );
    ~MarketImpactModel();
    
    // Main prediction methods (const, optimized)
    ImpactComponents calculateImpact(
        double quantity,
        const orderbook::OrderBook& order_book,
        bool is_buy
    ) const;
    
    ImpactComponents calculate_impact_fast(
        double quantity,
        const orderbook::OrderBook& order_book,
        bool is_buy
    ) const;
    
    // HFT optimized method (zero allocation)
    ImpactComponents calculate_impact_hft(
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        const std::array<double, 5>& ask_prices,
        const std::array<double, 5>& ask_quantities,
        const std::array<double, 5>& bid_prices,
        const std::array<double, 5>& bid_quantities,
        bool is_buy
    ) const;
    
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
        bool is_buy
    ) const;
    
    // Learning methods (non-const)
    void updateModel(
        double quantity,
        const orderbook::OrderBook& orderBook,
        bool isBuy,
        const ImpactComponents& actualImpact
    );
    
    // Thread-safe parameter management
    void update_parameters(const ModelParameters& new_params);
    ModelParameters get_parameters() const;
    
    // Individual parameter setters (thread-safe)
    void setVolatility(double volatility);
    void setPermanentImpactFactor(double factor);
    void setTemporaryImpactFactor(double factor);
    void setLiquidityFactor(double factor);
    void setPriceImpactDecay(double decay);
    
    // Individual parameter getters (thread-safe)
    double getVolatility() const;
    double getPermanentImpactFactor() const;
    double getTemporaryImpactFactor() const;
    double getLiquidityFactor() const;
    double getPriceImpactDecay() const;
    
    // Background processing
    void triggerBackgroundProcessing();
    
    // ✅ UPDATED: Cache management (delegates to shared cache)
    void invalidate_cache();
    void force_cache_update(const orderbook::OrderBook& order_book);
};

} // namespace models
} // namespace kubera