// core/models/slippage_model.h
#pragma once

#include <array>
#include <atomic>
#include <vector>
#include <deque>
#include <mutex>
#include "core/orderbook/orderbook.h"
#include "core/utils/memory_pool.h"
#include "logging/logger.h"
#include "concurrentqueue.h"

namespace kubera {
namespace models {

class SlippageModel {
public:
    struct SlippageResult {
        double expected;
        double lower_bound;
        double upper_bound;
        double prediction_quality;
    };

private:
    static constexpr size_t MAX_FEATURES = 8;
    static constexpr size_t QUANTILE_LEVELS = 3;
    static constexpr size_t MAX_HISTORY = 1000;
    static constexpr size_t CACHE_SIZE = 512;

    // Feature cache for performance
    struct FeatureCache {
        double midPrice{0.0};
        double spread{0.0};
        double depth{0.0};
        double volatility{0.0};
        double imbalance{0.0};
        uint64_t updateCount{0};
    };

    // Trade data for model updates
    struct TradeData {
        std::array<double, MAX_FEATURES> features;  // Fixed-size array instead of vector
        double actualSlippage;
        uint64_t timestamp;
    };

    // Thread-safe components
    std::atomic<bool> cache_valid_;
    FeatureCache feature_cache_;
    
    // Model coefficients
    std::array<std::array<double, MAX_FEATURES>, QUANTILE_LEVELS> coefficients_;
    
    // Historical data for learning
    std::deque<TradeData> historical_data_;
    moodycamel::ConcurrentQueue<TradeData> new_trade_records_;
    std::mutex history_mutex_;
    std::atomic<size_t> history_size_;
    
    utils::MemoryPool<64, 1024>& memory_pool_;
    logging::Logger& logger_;

    // Private methods
    std::vector<double> extract_features_cached(double quantity, const orderbook::OrderBook& order_book, bool is_buy);
    std::vector<double> extract_features_full(double quantity, const orderbook::OrderBook& order_book, bool is_buy) const;
    void process_new_trade_records();
    void calibrateModel();
    
    // NEW: HFT atomic calculation methods
    void extract_features_atomic(
        std::array<double, MAX_FEATURES>& features,
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        double total_depth,
        bool is_buy
    );

public:
    SlippageModel(logging::Logger& logger, utils::MemoryPool<64, 1024>& memory_pool);
    ~SlippageModel() = default;

    // Main prediction method
    SlippageResult predictSlippage(double quantity, const orderbook::OrderBook& orderBook, bool isBuy) const;
    
    std::vector<double> extractFeatures(double quantity, const orderbook::OrderBook& orderBook, bool isBuy);
    void updateModel(double quantity, const orderbook::OrderBook& orderBook, bool isBuy, double actualSlippage);
    void setCoefficients(const std::array<std::array<double, MAX_FEATURES>, QUANTILE_LEVELS>& coefficients);
    std::array<std::array<double, MAX_FEATURES>, QUANTILE_LEVELS> getCoefficients() const;

    // NEW: HFT optimized prediction method
    SlippageResult predict_slippage_hft(
        double quantity,
        double mid_price,
        double spread,
        double volatility,
        double imbalance,
        const std::array<double, 5>& ask_quantities,
        const std::array<double, 5>& bid_quantities,
        bool is_buy
    );
};

} // namespace models
} // namespace kubera
