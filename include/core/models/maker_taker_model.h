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

class MakerTakerModel {
public:
    static constexpr size_t MAX_FEATURES = 6;
    static constexpr size_t MAX_HISTORY = 1000;

    struct TradeData {
        std::array<double, MAX_FEATURES> features;
        double makerProportion;
        uint64_t timestamp;
    };

private:
    utils::MemoryPool<64, 1024>& memory_pool_;
    logging::Logger& logger_;
    SharedCacheManager& shared_cache_;
    
    // Model coefficients
    mutable std::shared_mutex coefficients_mutex_;
    std::vector<double> coefficients_;
    
    // Training data management
    mutable std::mutex history_mutex_;
    mutable std::deque<TradeData> historical_data_;
    mutable moodycamel::ConcurrentQueue<TradeData> new_trade_records_;
    mutable std::atomic<size_t> history_size_;
    mutable std::atomic<size_t> pending_records_count_;
    
    // ✅ NEW: Zero-allocation feature extraction
    void extract_features_zero_alloc(
        std::array<double, MAX_FEATURES>& features,
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        double total_depth,
        bool is_buy,
        double urgency_factor) const;
    
    // Cache-optimized extraction
    void extract_features_from_shared_cache(
        std::array<double, MAX_FEATURES>& features,
        const SharedMarketCache* cache,
        double quantity,
        bool is_buy,
        double urgency_factor) const;
    
    // Thread-safe calculations
    double calculate_logit_safe(const std::array<double, MAX_FEATURES>& features) const;
    double apply_market_adjustments_direct(double base_proportion, double mid_price, double spread, double volatility) const;
    
    // Background processing
    void process_new_trade_records() const;
    void process_trade_records_batch() const;
    void calibrateModel() const;

public:
    MakerTakerModel(
        logging::Logger& logger,
        utils::MemoryPool<64, 1024>& memory_pool,
        SharedCacheManager& shared_cache
    );
    
    // ✅ NEW: HFT optimized prediction
    double predict_proportion_hft(
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        const std::array<double, 5>& ask_quantities,
        const std::array<double, 5>& bid_quantities,
        bool is_buy,
        double urgency_factor) const;
    
    // ✅ Keep original methods for backwards compatibility
    double predict_maker_proportion(double quantity, const orderbook::OrderBook& order_book, bool is_buy, double urgency_factor = 0.5) const;
    double predict_maker_proportion_fast(double quantity, const orderbook::OrderBook& order_book, bool is_buy, double urgency_factor = 0.5) const;
    double predictMakerProportion(double quantity, const orderbook::OrderBook& orderBook, bool isBuy) const;
    
    // Atomic calculation alias
    double calculate_proportion_from_atomics(
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        const std::array<double, 5>& ask_quantities,
        const std::array<double, 5>& bid_quantities,
        bool is_buy,
        double urgency_factor) const;
    
    // Model management
    void setCoefficients(const std::vector<double>& coefficients);
    std::vector<double> getCoefficients() const;
    
    // Training
    void updateModel(double quantity, const orderbook::OrderBook& orderBook, bool isBuy, double actualMakerProportion);
    void triggerBackgroundProcessing();
    
    // Cache management
    void invalidate_cache();
    void force_cache_update(const orderbook::OrderBook& order_book);
};

} // namespace models
} // namespace kubera
