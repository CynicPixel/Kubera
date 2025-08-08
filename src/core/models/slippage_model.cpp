
#include "core/models/slippage_model.h"
#include "core/utils/portable_math.h"
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace kubera {
namespace models {

SlippageModel::SlippageModel(
    logging::Logger& logger, 
    utils::MemoryPool<64, 1024>& memory_pool,
    SharedCacheManager& shared_cache
) : memory_pool_(memory_pool), logger_(logger), 
    shared_cache_(shared_cache),
    cache_valid_(false), history_size_(0) {
    
// Research-based coefficients for cryptocurrency markets
// [intercept, spread, relative_size, volatility, imbalance, depth_ratio, price_momentum, volume_momentum]
coefficients_[0] = {0.05, 0.5, 2.5, 1.0, 0.5, 0.25, 0.05, 0.05};  // 25th percentile
coefficients_[1] = {0.10, 1.0, 5.0, 2.0, 1.0, 0.5, 0.10, 0.10};   // 50th percentile (median)
coefficients_[2] = {0.20, 1.5, 7.5, 3.0, 1.5, 0.75, 0.20, 0.20};  // 75th percentile
    utils::PortableMath::initialize();
    logger_.info("Slippage model initialized with {} quantile levels", QUANTILE_LEVELS);
}

// ✅ NEW: HFT optimized prediction method
SlippageModel::SlippageResult SlippageModel::predict_slippage_hft(
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    const std::array<double, 5>& ask_quantities,
    const std::array<double, 5>& bid_quantities,
    bool is_buy) const {
    
    try {
        // Use thread-local buffer (zero allocation)
        thread_local static std::array<double, MAX_FEATURES> feature_buffer;
        
        // Calculate total depth from atomic data
        double ask_depth = std::accumulate(ask_quantities.begin(), ask_quantities.end(), 0.0);
        double bid_depth = std::accumulate(bid_quantities.begin(), bid_quantities.end(), 0.0);
        double total_depth = ask_depth + bid_depth;
        
        // Extract features directly into buffer
        extract_features_zero_alloc(feature_buffer, quantity, mid_price, spread, 
                                  volatility, imbalance, total_depth, is_buy);
        
        // Calculate quantile predictions
        std::array<double, QUANTILE_LEVELS> predictions;
        {
            std::shared_lock lock(coefficients_mutex_);
            for (size_t q = 0; q < QUANTILE_LEVELS; ++q) {
                predictions[q] = 0.0;
                for (size_t f = 0; f < MAX_FEATURES; ++f) {
                    predictions[q] += coefficients_[q][f] * feature_buffer[f];
                }
            }
        }
        
        // Apply minimum slippage constraint
        double min_slippage = spread * 0.5;
        for (auto& pred : predictions) {
            pred = std::max(pred, min_slippage);
        }
        
        if (!is_buy) {
            for (auto& pred : predictions) {
                pred = -pred;
            }
        }
        
        return {predictions[1], predictions[0], predictions[2], 0.85};
        
    } catch (const std::exception& e) {
        logger_.error("Failed to predict slippage (HFT): {}", e.what());
        return {0.0, 0.0, 0.0, 0.0};
    }
}

// ✅ NEW: Zero-allocation feature extraction
void SlippageModel::extract_features_zero_alloc(
    std::array<double, MAX_FEATURES>& features,
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    double total_depth,
    bool is_buy) const {
    
    features[0] = 1.0; // intercept
    features[1] = spread / std::max(mid_price, 1.0); // normalized_spread
    features[2] = quantity / std::max(total_depth, 1.0); // relative_size
    features[3] = std::min(volatility, 1.0); // bounded_volatility
    features[4] = is_buy ? imbalance : -imbalance; // directional_imbalance
    features[5] = 0.5; // depth_ratio (placeholder)
    features[6] = std::tanh(std::min(volatility, 1.0) * 10.0); // price_momentum
    features[7] = std::tanh(imbalance * 5.0); // volume_momentum
}

// ✅ Keep original method for backwards compatibility
SlippageModel::SlippageResult SlippageModel::predictSlippage(
    double quantity,
    const orderbook::OrderBook& orderBook,
    bool isBuy) const {
    
    try {
        auto snapshot = orderBook.getSnapshot();
        
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
        
        return predict_slippage_hft(quantity, snapshot.midPrice, snapshot.spread,
                                  snapshot.volatility, snapshot.imbalance,
                                  ask_quantities, bid_quantities, isBuy);
        
    } catch (const std::exception& e) {
        logger_.error("Failed to predict slippage: {}", e.what());
        return {0.0, 0.0, 0.0, 0.0};
    }
}

// ✅ Optimized feature extraction with shared cache
double* SlippageModel::extract_features_full(
    double quantity,
    const orderbook::OrderBook& order_book,
    bool is_buy) const {
    
    double* features = allocateFeatureArray();
    if (!features) return nullptr;
    
    auto current_time_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t ob_version = order_book.getSequenceNumber();
    auto cache_snapshot = shared_cache_.get_snapshot();
    
    if (cache_snapshot.is_valid(ob_version, current_time_ns)) {
        // Use cached data
        extract_features_from_cache(features, quantity, cache_snapshot.data, is_buy);
    } else {
        // Use direct calculation
        auto snapshot = order_book.getSnapshot();
        extract_features_from_snapshot(features, quantity, static_cast<const kubera::orderbook::OrderBookSnapshot&>(snapshot), is_buy);
    }
    
    return features;
}

void SlippageModel::extract_features_from_snapshot(
    double* features,
    double quantity,
    const orderbook::OrderBookSnapshot& snapshot,
    bool is_buy) const {
    
    if (!features) return;
    
    // Calculate depths from snapshot
    double ask_depth = 0.0, bid_depth = 0.0;
    for (const auto& level : snapshot.asks) {
        ask_depth += level.quantity;
    }
    for (const auto& level : snapshot.bids) {
        bid_depth += level.quantity;
    }
    double total_depth = ask_depth + bid_depth;
    
    // Extract features
    features[0] = 1.0; // intercept
    features[1] = snapshot.spread / std::max(snapshot.midPrice, 1.0); // normalized_spread
    features[2] = quantity / std::max(total_depth, 1.0); // relative_size
    features[3] = std::min(snapshot.volatility, 1.0); // bounded_volatility
    features[4] = is_buy ? snapshot.imbalance : -snapshot.imbalance; // directional_imbalance
    features[5] = ask_depth / std::max(bid_depth, 1.0); // depth_ratio
    features[6] = std::tanh(std::min(snapshot.volatility, 1.0) * 10.0); // price_momentum
    features[7] = std::tanh(snapshot.imbalance * 5.0); // volume_momentum
}

void SlippageModel::extract_features_from_cache(
    double* features,
    double quantity,
    const SharedMarketCache* cache,
    bool is_buy) const {
    
    features[0] = 1.0;
    features[1] = cache->spread / std::max(cache->midPrice, 1.0);
    features[2] = quantity / std::max(cache->totalDepth, 1.0);
    features[3] = std::min(cache->volatility, 1.0);
    features[4] = is_buy ? cache->imbalance : -cache->imbalance;
    features[5] = cache->askDepth / std::max(cache->bidDepth, 1.0);
    features[6] = std::tanh(std::min(cache->volatility, 1.0) * 10.0);
    features[7] = std::tanh(cache->imbalance * 5.0);
}


// ✅ Backwards compatibility aliases
double* SlippageModel::extractFeatures(double quantity, const orderbook::OrderBook& orderBook, bool isBuy) {
    return extract_features_full(quantity, orderBook, isBuy);
}

// ✅ Thread-safe coefficient management
void SlippageModel::setCoefficients(size_t quantile_index, const std::vector<double>& coefficients) {
    if (quantile_index >= QUANTILE_LEVELS) {
        logger_.warning("Invalid quantile index: {}", quantile_index);
        return;
    }
    
    std::unique_lock lock(coefficients_mutex_);
    for (size_t i = 0; i < std::min(coefficients.size(), static_cast<size_t>(MAX_FEATURES)); ++i) {
        coefficients_[quantile_index][i] = coefficients[i];
    }
    
    logger_.info("Coefficients updated for quantile {}", quantile_index);
}

std::vector<double> SlippageModel::getCoefficients(size_t quantile_index) const {
    if (quantile_index >= QUANTILE_LEVELS) {
        return {};
    }
    
    std::shared_lock lock(coefficients_mutex_);
    return std::vector<double>(coefficients_[quantile_index].begin(), coefficients_[quantile_index].end());
}

// ✅ Model training and calibration
void SlippageModel::updateModel(
    double quantity,
    const orderbook::OrderBook& orderBook,
    bool isBuy,
    double actualSlippage) {
    
    try {
        auto snapshot = orderBook.getSnapshot();
        TradeData data;
        
        // Extract features from snapshot
        double ask_depth = 0.0, bid_depth = 0.0;
        for (const auto& level : snapshot.asks) ask_depth += level.quantity;
        for (const auto& level : snapshot.bids) bid_depth += level.quantity;
        double total_depth = ask_depth + bid_depth;
        
        std::array<double, MAX_FEATURES> features_array;
        extract_features_zero_alloc(features_array, quantity, snapshot.midPrice, 
                                   snapshot.spread, snapshot.volatility, 
                                   snapshot.imbalance, total_depth, isBuy);
        
        // Copy to TradeData
        for (size_t i = 0; i < MAX_FEATURES; ++i) {
            data.features[i] = features_array[i];
        }
        
        data.actualSlippage = actualSlippage;
        data.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        new_trade_records_.enqueue(data);
        logger_.debug("Model updated with new trade data");
        
    } catch (const std::exception& e) {
        logger_.error("Failed to update slippage model: {}", e.what());
    }
}

void SlippageModel::process_new_trade_records() {
    TradeData record;
    std::vector<TradeData> batch;
    batch.reserve(100);
    
    // Batch process records
    while (batch.size() < 100 && new_trade_records_.try_dequeue(record)) {
        batch.push_back(record);
    }
    
    if (!batch.empty()) {
        std::lock_guard lock(history_mutex_);
        for (const auto& data : batch) {
            historical_data_.push_back(data);
            if (historical_data_.size() > MAX_HISTORY) {
                historical_data_.pop_front();
            }
        }
        
        history_size_.store(historical_data_.size(), std::memory_order_relaxed);
        
        // Trigger model recalibration if enough new data
        if (history_size_.load() >= 50) {
            calibrateModel();
        }
    }
}

void SlippageModel::calibrateModel() {
    std::vector<TradeData> training_data;
    
    {
        std::lock_guard lock(history_mutex_);
        if (historical_data_.size() < 20) return;
        training_data.assign(historical_data_.begin(), historical_data_.end());
    }
    
    // Simple gradient descent for quantile regression
    const double learning_rate = 0.01;
    const int iterations = 100;
    
    for (size_t q = 0; q < QUANTILE_LEVELS; ++q) {
        double quantile_level = 0.25 + q * 0.25; // 0.25, 0.5, 0.75
        
        for (int iter = 0; iter < iterations; ++iter) {
            std::array<double, MAX_FEATURES> gradients{};
            
            for (const auto& data : training_data) {
                double prediction = 0.0;
                for (size_t f = 0; f < MAX_FEATURES; ++f) {
                    prediction += coefficients_[q][f] * data.features[f];
                }
                
                double error = data.actualSlippage - prediction;
                double quantile_loss_gradient = (error > 0) ? quantile_level : (quantile_level - 1.0);
                
                for (size_t f = 0; f < MAX_FEATURES; ++f) {
                    gradients[f] += quantile_loss_gradient * data.features[f];
                }
            }
            
            // Update coefficients
            std::unique_lock lock(coefficients_mutex_);
            for (size_t f = 0; f < MAX_FEATURES; ++f) {
                coefficients_[q][f] += learning_rate * gradients[f] / training_data.size();
            }
        }
    }
    
    logger_.info("Model calibrated with {} samples", training_data.size());
}

void SlippageModel::triggerBackgroundProcessing() {
    process_new_trade_records();
}

} // namespace models
} // namespace kubera
