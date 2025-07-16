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

class MakerTakerModel {
private:
    static constexpr size_t MAX_FEATURES = 6;
    static constexpr size_t MAX_HISTORY = 1000;
    static constexpr uint64_t CACHE_EXPIRY_NS = 500'000; // 500μs cache expiry
    
    // ✅ REMOVED: Individual cache members
    // mutable std::atomic<CacheData*> current_cache_;
    // mutable std::atomic<CacheData*> pending_delete_;
    
    // ✅ ADDED: Shared cache reference
    SharedCacheManager& shared_cache_;
    
    // Thread-safe coefficient management
    mutable std::shared_mutex coefficients_mutex_;
    std::vector<double> coefficients_;
    
    // Historical data for online learning
    struct TradeData {
        std::array<double, MAX_FEATURES> features;
        double makerProportion;
        uint64_t timestamp;
    };
    
    // Lock-free learning with batch processing
    mutable moodycamel::ConcurrentQueue<TradeData> new_trade_records_;
    mutable std::atomic<size_t> pending_records_count_;
    mutable std::deque<TradeData> historical_data_;
    mutable std::atomic<size_t> history_size_;
    mutable std::mutex history_mutex_;
    
    // External resources
    utils::MemoryPool<64, 1024>& memory_pool_;
    logging::Logger& logger_;
    
    // ✅ UPDATED: Private methods using shared cache
    SharedCacheManager::CacheSnapshot get_cache_snapshot() const;
    void update_shared_cache(const orderbook::OrderBook& order_book) const;
    void update_shared_cache_from_snapshot(const OrderBookSnapshot& snapshot) const;
    
    // ✅ UPDATED: Feature extraction methods
    void extract_features_from_shared_cache(
        std::array<double, MAX_FEATURES>& features,
        const SharedMarketCache* cache,  // ✅ CHANGED: SharedMarketCache instead of CacheData
        double quantity,
        bool is_buy,
        double urgency_factor
    ) const;
    
    void extract_features_direct(
        std::array<double, MAX_FEATURES>& features,
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        double total_depth,
        bool is_buy,
        double urgency_factor
    ) const;
    
    // Calculation methods
    double apply_market_adjustments_direct(double base_proportion, double mid_price, double spread, double volatility) const;
    double calculate_logit_safe(const std::array<double, MAX_FEATURES>& features) const;
    double sigmoid(double x) const;
    
    // Background processing
    void process_new_trade_records() const;
    void process_trade_records_batch() const;
    void calibrateModel() const;
    
    // Legacy methods for backward compatibility (updated to use shared cache)
    std::vector<double> extractFeatures(double quantity, const orderbook::OrderBook& orderBook, bool isBuy) const;
    std::vector<double> extract_features_full(double quantity, const orderbook::OrderBook& order_book, bool is_buy, double urgency_factor) const;
    std::vector<double> extract_features_cached(double quantity, const orderbook::OrderBook& order_book, bool is_buy, double urgency_factor) const;
    void extract_features_cached_array(std::array<double, MAX_FEATURES>& features, double quantity, const orderbook::OrderBook& order_book, bool is_buy, double urgency_factor) const;
    void extract_features_atomic(std::array<double, MAX_FEATURES>& features, double quantity, double mid_price, double spread, double volatility, double imbalance, double total_depth, bool is_buy, double urgency_factor) const;
    double calculate_logit_direct(const std::array<double, MAX_FEATURES>& features) const;
    double apply_market_adjustments(double base_proportion, const orderbook::OrderBook& order_book) const;
    double apply_market_adjustments_cached(double base_proportion) const;
    bool is_cache_valid(uint64_t ob_version) const;
    void update_cache_atomic(const orderbook::OrderBook& order_book, uint64_t version, uint64_t expiry_time) const;
    double predict_from_cache(double quantity, bool is_buy, double urgency_factor) const;

public:
    // ✅ UPDATED: Constructor with shared cache parameter
    MakerTakerModel(
        logging::Logger& logger, 
        utils::MemoryPool<64, 1024>& memory_pool,
        SharedCacheManager& shared_cache = g_shared_cache  // ✅ ADDED: shared cache parameter
    );
    ~MakerTakerModel();
    
    // Main prediction methods
    double predict_maker_proportion(
        double quantity,
        const orderbook::OrderBook& order_book,
        bool is_buy,
        double urgency_factor = 0.5
    ) const;
    
    double predict_maker_proportion_fast(
        double quantity,
        const orderbook::OrderBook& order_book,
        bool is_buy,
        double urgency_factor = 0.5
    ) const;
    
    // HFT optimized method
    double predict_proportion_hft(
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        const std::array<double, 5>& ask_quantities,
        const std::array<double, 5>& bid_quantities,
        bool is_buy,
        double urgency_factor = 0.5
    ) const;
    
    double calculate_proportion_from_atomics(
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        const std::array<double, 5>& ask_quantities,
        const std::array<double, 5>& bid_quantities,
        bool is_buy,
        double urgency_factor
    ) const;
    
    // Learning methods
    void updateModel(double quantity, const orderbook::OrderBook& orderBook, bool isBuy, double actualMakerProportion);
    
    // Coefficient management
    void setCoefficients(const std::vector<double>& coefficients);
    std::vector<double> getCoefficients() const;
    
    // Legacy compatibility
    double predictMakerProportion(double quantity, const orderbook::OrderBook& orderBook, bool isBuy) const;
    
    // Background processing
    void triggerBackgroundProcessing();
    
    // ✅ UPDATED: Cache management (delegates to shared cache)
    void invalidate_cache();
    void force_cache_update(const orderbook::OrderBook& order_book);
};

} // namespace models
} // namespace kubera