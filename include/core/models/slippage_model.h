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

class SlippageModel {
public:
    static constexpr size_t MAX_FEATURES = 8;
    static constexpr size_t QUANTILE_LEVELS = 3;
    static constexpr size_t MAX_HISTORY = 1000;

    struct SlippageResult {
        double expected;    // Median prediction
        double lower_bound; // 25th percentile
        double upper_bound; // 75th percentile
        double confidence;  // Model confidence
    };

    struct TradeData {
        std::array<double, MAX_FEATURES> features;
        double actualSlippage;
        uint64_t timestamp;
    };

private:
    utils::MemoryPool<64, 1024>& memory_pool_;
    logging::Logger& logger_;
    SharedCacheManager& shared_cache_;
    
    // Model coefficients (one set per quantile)
    mutable std::shared_mutex coefficients_mutex_;
    std::array<std::array<double, MAX_FEATURES>, QUANTILE_LEVELS> coefficients_;
    
    // Training data management
    mutable std::mutex history_mutex_;
    std::deque<TradeData> historical_data_;
    moodycamel::ConcurrentQueue<TradeData> new_trade_records_;
    std::atomic<size_t> history_size_;
    
    // Cache management
    std::atomic<bool> cache_valid_;
    
     void extract_features_zero_alloc(
        std::array<double, MAX_FEATURES>& features,
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        double total_depth,
        bool is_buy) const;
    
    // Cache-optimized extraction
    void extract_features_from_cache(double* features, double quantity, const SharedMarketCache* cache, bool is_buy) const;
    //void extract_features_from_snapshot(double* features, double quantity, const orderbook::OrderBookSnapshot& snapshot, bool is_buy) const;
    

    void extract_features_from_snapshot(double* features, double quantity, const kubera::orderbook::OrderBookSnapshot& snapshot, bool is_buy) const;
    // Background processing
    void process_new_trade_records();
    void calibrateModel();
    
    // Memory management
    double* allocateFeatureArray() const {
        return reinterpret_cast<double*>(memory_pool_.allocate());
    }

public:
    SlippageModel(logging::Logger& logger, utils::MemoryPool<64, 1024>& memory_pool, SharedCacheManager& shared_cache);
    
    // ✅ NEW: HFT optimized prediction
    SlippageResult predict_slippage_hft(
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        const std::array<double, 5>& ask_quantities,
        const std::array<double, 5>& bid_quantities,
        bool is_buy) const;
    
    // ✅ Keep original methods for backwards compatibility
    SlippageResult predictSlippage(double quantity, const orderbook::OrderBook& orderBook, bool isBuy) const;
    double* extractFeatures(double quantity, const orderbook::OrderBook& orderBook, bool isBuy);
    
    // Enhanced feature extraction
    double* extract_features_full(double quantity, const orderbook::OrderBook& order_book, bool is_buy) const;
    
    // Model management
    void setCoefficients(size_t quantile_index, const std::vector<double>& coefficients);
    std::vector<double> getCoefficients(size_t quantile_index) const;
    
    // Training
    void updateModel(double quantity, const orderbook::OrderBook& orderBook, bool isBuy, double actualSlippage);
    void triggerBackgroundProcessing();
};

} // namespace models
} // namespace kubera
