#include "core/models/maker_taker_model.h"
#include "core/utils/portable_math.h"
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace kubera {
namespace models {

MakerTakerModel::MakerTakerModel(
    logging::Logger& logger,
    utils::MemoryPool<64, 1024>& memory_pool,
    SharedCacheManager& shared_cache
) : memory_pool_(memory_pool), logger_(logger),
    shared_cache_(shared_cache),
    history_size_(0), pending_records_count_(0) {
    
    // Initialize with industry-calibrated coefficients
    coefficients_ = {0.5, -2.0, 1.5, 0.8, -0.5, -1.2};
    
    utils::PortableMath::initialize();
    logger_.info("Maker/Taker model initialized with {} coefficients", coefficients_.size());
}

// ✅ NEW: HFT optimized prediction method
double MakerTakerModel::predict_proportion_hft(
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    const std::array<double, 5>& ask_quantities,
    const std::array<double, 5>& bid_quantities,
    bool is_buy,
    double urgency_factor) const {
    
    try {
        // Use thread-local buffer (zero allocation)
        thread_local static std::array<double, MAX_FEATURES> feature_buffer;
        
        double ask_depth = std::accumulate(ask_quantities.begin(), ask_quantities.end(), 0.0);
        double bid_depth = std::accumulate(bid_quantities.begin(), bid_quantities.end(), 0.0);
        double total_depth = ask_depth + bid_depth;
        
        extract_features_zero_alloc(feature_buffer, quantity, mid_price, spread, 
                                   volatility, imbalance, total_depth, is_buy, urgency_factor);
        
        double logit = calculate_logit_safe(feature_buffer);
        double proportion = utils::PortableMath::fast_sigmoid(logit);
        proportion = apply_market_adjustments_direct(proportion, mid_price, spread, volatility);
        
        return std::max(0.05, std::min(0.95, proportion));
        
    } catch (const std::exception& e) {
        logger_.error("Failed to predict proportion (HFT): {}", e.what());
        return 0.5;
    }
}

// ✅ NEW: Zero-allocation feature extraction
void MakerTakerModel::extract_features_zero_alloc(
    std::array<double, MAX_FEATURES>& features,
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    double total_depth,
    bool is_buy,
    double urgency_factor) const {
    
    features[0] = 1.0; // intercept
    features[1] = std::log1p(quantity / 100000.0); // relative_size
    features[2] = std::min(spread / std::max(mid_price, 1.0), 0.1); // normalized_spread
    features[3] = std::max(-1.0, std::min(1.0, is_buy ? imbalance : -imbalance)); // directional_imbalance
    features[4] = std::min(volatility, 1.0); // bounded_volatility
    features[5] = std::max(0.0, std::min(1.0, urgency_factor)); // urgency
}

// ✅ Thread-safe coefficient access
double MakerTakerModel::calculate_logit_safe(const std::array<double, MAX_FEATURES>& features) const {
    std::shared_lock lock(coefficients_mutex_);
    
    double logit = 0.0;
    size_t min_size = std::min(features.size(), coefficients_.size());
    for (size_t i = 0; i < min_size; ++i) {
        logit += features[i] * coefficients_[i];
    }
    
    return logit;
}

// ✅ Market adjustments with direct parameters
double MakerTakerModel::apply_market_adjustments_direct(
    double base_proportion, 
    double mid_price, 
    double spread, 
    double volatility) const {
    
    double spread_ratio = spread / std::max(mid_price, 1.0);
    
    // Wider spreads favor maker orders
    if (spread_ratio > 0.001) {
        base_proportion *= (1.0 + spread_ratio * 10.0);
    }
    
    // High volatility reduces maker proportion
    if (volatility > 0.02) {
        base_proportion *= (1.0 - std::min(volatility, 1.0) * 5.0);
    }
    
    return std::max(0.05, std::min(0.95, base_proportion));
}

// ✅ Keep original method for backwards compatibility
double MakerTakerModel::predict_maker_proportion(
    double quantity,
    const orderbook::OrderBook& order_book,
    bool is_buy,
    double urgency_factor) const {
    
    try {
        auto snapshot = order_book.getSnapshot();
        
        // Calculate depths from snapshot
        double ask_depth = 0.0, bid_depth = 0.0;
        for (const auto& level : snapshot.asks) ask_depth += level.quantity;
        for (const auto& level : snapshot.bids) bid_depth += level.quantity;
        
        // Convert to HFT arrays
        std::array<double, 5> ask_quantities{}, bid_quantities{};
        for (size_t i = 0; i < 5 && i < snapshot.asks.size(); ++i) {
            ask_quantities[i] = snapshot.asks[i].quantity;
        }
        for (size_t i = 0; i < 5 && i < snapshot.bids.size(); ++i) {
            bid_quantities[i] = snapshot.bids[i].quantity;
        }
        
        return predict_proportion_hft(quantity, snapshot.midPrice, snapshot.spread,
                                    snapshot.volatility, snapshot.imbalance,
                                    ask_quantities, bid_quantities, is_buy, urgency_factor);
        
    } catch (const std::exception& e) {
        logger_.error("Failed to predict maker proportion: {}", e.what());
        return 0.5;
    }
}

// ✅ Fast prediction with minimal overhead
double MakerTakerModel::predict_maker_proportion_fast(
    double quantity,
    const orderbook::OrderBook& order_book,
    bool is_buy,
    double urgency_factor) const {
    
    try {
        auto current_time_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        uint64_t ob_version = order_book.getSequenceNumber();
        
        // Fast path: check shared cache first
        auto cache_snapshot = shared_cache_.get_snapshot();
        if (cache_snapshot.is_valid(ob_version, current_time_ns)) {
            thread_local static std::array<double, MAX_FEATURES> features;
            extract_features_from_shared_cache(features, cache_snapshot.data, quantity, is_buy, urgency_factor);
            
            double logit = calculate_logit_safe(features);
            double proportion = utils::PortableMath::fast_sigmoid(logit);
            proportion = apply_market_adjustments_direct(proportion, cache_snapshot.data->midPrice,
                                                       cache_snapshot.data->spread, cache_snapshot.data->volatility);
            return std::max(0.0, std::min(1.0, proportion));
        }
        
        // Fallback to direct calculation
        return predict_maker_proportion(quantity, order_book, is_buy, urgency_factor);
        
    } catch (const std::exception& e) {
        logger_.error("Failed to predict maker proportion (fast): {}", e.what());
        return 0.5;
    }
}

// ✅ Extract features from shared cache data
void MakerTakerModel::extract_features_from_shared_cache(
    std::array<double, MAX_FEATURES>& features,
    const SharedMarketCache* cache,
    double quantity,
    bool is_buy,
    double urgency_factor) const {
    
    extract_features_zero_alloc(features, quantity, cache->midPrice, cache->spread,
                               cache->volatility, cache->imbalance, cache->totalDepth,
                               is_buy, urgency_factor);
}

// ✅ Thread-safe coefficient management
void MakerTakerModel::setCoefficients(const std::vector<double>& coefficients) {
    std::unique_lock lock(coefficients_mutex_);
    coefficients_ = coefficients;
    logger_.info("Model coefficients updated, new size: {}", coefficients_.size());
}

std::vector<double> MakerTakerModel::getCoefficients() const {
    std::shared_lock lock(coefficients_mutex_);
    return coefficients_;
}

// ✅ Model update with proper exception safety and atomic snapshots
void MakerTakerModel::updateModel(
    double quantity,
    const orderbook::OrderBook& orderBook,
    bool isBuy,
    double actualMakerProportion) {
    
    try {
        auto snapshot = orderBook.getSnapshot();
        TradeData data;
        
        // Calculate total depth from snapshot
        double ask_depth = 0.0, bid_depth = 0.0;
        for (const auto& level : snapshot.asks) ask_depth += level.quantity;
        for (const auto& level : snapshot.bids) bid_depth += level.quantity;
        double total_depth = ask_depth + bid_depth;
        
        std::array<double, MAX_FEATURES> features_array;
        extract_features_zero_alloc(features_array, quantity, snapshot.midPrice,
                                   snapshot.spread, snapshot.volatility,
                                   snapshot.imbalance, total_depth, isBuy, 0.5);
        
        // Copy to TradeData
        for (size_t i = 0; i < MAX_FEATURES; ++i) {
            data.features[i] = features_array[i];
        }
        
        data.makerProportion = actualMakerProportion;
        data.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        new_trade_records_.enqueue(data);
        pending_records_count_.fetch_add(1, std::memory_order_relaxed);
        
        logger_.debug("Model updated with new trade data");
        
    } catch (const std::exception& e) {
        logger_.error("Failed to update maker/taker model: {}", e.what());
    }
}

// ✅ Background processing with batch optimization
void MakerTakerModel::process_new_trade_records() const {
    try {
        // Check if we have enough records to process
        if (pending_records_count_.load() < 10) {
            return;
        }
        
        process_trade_records_batch();
    } catch (const std::exception& e) {
        logger_.error("Failed to process trade records: {}", e.what());
    }
}

void MakerTakerModel::process_trade_records_batch() const {
    std::vector<TradeData> batch;
    batch.reserve(100);
    
    // Drain queue without holding mutex
    TradeData record;
    while (batch.size() < 100 && new_trade_records_.try_dequeue(record)) {
        batch.push_back(record);
    }
    
    if (!batch.empty()) {
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            // Process entire batch under single lock
            for (const auto& data : batch) {
                historical_data_.push_back(data);
                if (historical_data_.size() > MAX_HISTORY) {
                    historical_data_.pop_front();
                }
            }
            
            history_size_.store(static_cast<size_t>(historical_data_.size()), std::memory_order_relaxed);
            pending_records_count_.fetch_sub(static_cast<size_t>(batch.size()), std::memory_order_relaxed);
        }
        
        // Trigger calibration if enough new data
        if (history_size_.load() >= 50) {
            calibrateModel();
        }
    }
}

// ✅ Model calibration
void MakerTakerModel::calibrateModel() const {
    std::vector<TradeData> training_data;
    
    {
        std::lock_guard lock(history_mutex_);
        if (historical_data_.size() < 10) return;
        training_data.assign(historical_data_.begin(), historical_data_.end());
    }
    
    // Calibrate without holding lock (prevents deadlock)
    logger_.info("Calibrating model with {} samples", training_data.size());
    // TODO: Implement proper logistic regression calibration
}

void MakerTakerModel::triggerBackgroundProcessing() {
    process_new_trade_records();
}

// ✅ Cache management delegates to shared cache
void MakerTakerModel::invalidate_cache() {
    shared_cache_.invalidate_cache();
}

void MakerTakerModel::force_cache_update(const orderbook::OrderBook& order_book) {
    shared_cache_.update_from_orderbook(order_book);
}

// ✅ Backward compatibility aliases
double MakerTakerModel::predictMakerProportion(double quantity, const orderbook::OrderBook& orderBook, bool isBuy) const {
    return predict_maker_proportion(quantity, orderBook, isBuy, 0.5);
}

double MakerTakerModel::calculate_proportion_from_atomics(
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    const std::array<double, 5>& ask_quantities,
    const std::array<double, 5>& bid_quantities,
    bool is_buy,
    double urgency_factor) const {
    
    return predict_proportion_hft(quantity, mid_price, spread, volatility, imbalance,
                                 ask_quantities, bid_quantities, is_buy, urgency_factor);
}

} // namespace models
} // namespace kubera
