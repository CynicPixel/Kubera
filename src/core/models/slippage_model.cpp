// slippage_model.cpp
#include "core/models/slippage_model.h"
#include "core/utils/portable_math.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <chrono>

namespace kubera {
namespace models {

SlippageModel::SlippageModel(logging::Logger& logger, utils::MemoryPool<64, 1024>& memory_pool)
    : memory_pool_(memory_pool), logger_(logger), cache_valid_(false), history_size_(0) {
    
    // Initialize coefficients with industry-standard values
    // [intercept, spread, relative_size, volatility, imbalance, depth_ratio, price_momentum, volume_momentum]
    coefficients_[0] = {0.001, 0.5, 0.05, 0.02, 0.01, 0.005, 0.001, 0.001}; // 25th percentile
    coefficients_[1] = {0.002, 1.0, 0.1, 0.04, 0.02, 0.01, 0.002, 0.002}; // 50th percentile (median)
    coefficients_[2] = {0.004, 1.5, 0.15, 0.06, 0.03, 0.015, 0.003, 0.003}; // 75th percentile
    
    logger_.info("Slippage model initialized with {} quantile levels", QUANTILE_LEVELS);
}

SlippageModel::SlippageResult SlippageModel::predictSlippage(
    double quantity,
    const orderbook::OrderBook& orderBook,
    bool isBuy
) const {
    try {
        // Process any pending updates - remove since const method can't modify state
        // process_new_trade_records();
        
        // Use cached features for performance - simplified for const method
        auto features = extract_features_full(quantity, orderBook, isBuy);
        
        // Calculate quantile predictions
        std::array<double, QUANTILE_LEVELS> predictions;
        
        for (size_t q = 0; q < QUANTILE_LEVELS; ++q) {
            predictions[q] = 0.0;
            for (size_t f = 0; f < std::min(features.size(), coefficients_[q].size()); ++f) {
                predictions[q] += coefficients_[q][f] * features[f];
            }
        }
        
        // Ensure minimum slippage (half spread)
        double spread = orderBook.getSpread();
        double min_slippage = spread * 0.5;
        
        for (auto& pred : predictions) {
            pred = std::max(pred, min_slippage);
        }
        
        // Adjust for direction
        if (!isBuy) {
            for (auto& pred : predictions) {
                pred = -pred;
            }
        }
        
        return {
            predictions[1], // Median as expected
            predictions[0], // 25th percentile as lower bound
            predictions[2], // 75th percentile as upper bound
            0.85 // Default prediction quality
        };
        
    } catch (const std::exception& e) {
        logger_.error("Failed to predict slippage: {}", e.what());
        return {0.0, 0.0, 0.0, 0.0};
    }
}

std::vector<double> SlippageModel::extract_features_full(
    double quantity,
    const orderbook::OrderBook& order_book,
    bool is_buy
) const {
    double midPrice = order_book.getMidPrice();
    double spread = order_book.getSpread();
    double depth = order_book.getDepth(5);
    double volatility = order_book.getVolatility();
    double imbalance = order_book.getImbalance();
    
    // Note: Can't update cache in const method
    
    return {
        1.0, // Intercept
        spread / std::max(midPrice, 1.0), // Normalized spread
        quantity / std::max(depth, 1.0), // Relative size
        volatility, // Market volatility
        is_buy ? imbalance : -imbalance, // Directional imbalance
        0.5, // Default depth ratio
        std::tanh(volatility * 10.0), // Price momentum
        std::tanh(imbalance * 5.0) // Volume momentum
    };
}

std::vector<double> SlippageModel::extractFeatures(double quantity, const orderbook::OrderBook& orderBook, bool isBuy) {
    return extract_features_full(quantity, orderBook, isBuy);
}

void SlippageModel::process_new_trade_records() {
    TradeData record;
    while (new_trade_records_.try_dequeue(record)) {
        std::lock_guard<std::mutex> lock(history_mutex_);
        historical_data_.push_back(record);
        
        if (historical_data_.size() > MAX_HISTORY) {
            historical_data_.pop_front();
        }
        
        history_size_.store(historical_data_.size(), std::memory_order_relaxed);
    }
    
    // Trigger model recalibration if enough new data
    if (history_size_.load() >= 50) {
        calibrateModel();
    }
}

void SlippageModel::updateModel(
    double quantity,
    const orderbook::OrderBook& orderBook,
    bool isBuy,
    double actualSlippage
) {
    try {
        // Extract features for this trade
        auto features_vec = extractFeatures(quantity, orderBook, isBuy);
        
        TradeData data;
        // Convert vector to fixed-size array
        for (size_t i = 0; i < std::min(features_vec.size(), data.features.size()); ++i) {
            data.features[i] = features_vec[i];
        }
        data.actualSlippage = actualSlippage;
        data.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        new_trade_records_.enqueue(data);
        
        logger_.debug("Model updated with new trade data, historical data size: {}", history_size_.load());
    } catch (const std::exception& e) {
        logger_.error("Failed to update slippage model: {}", e.what());
    }
}

void SlippageModel::calibrateModel() {
    std::lock_guard<std::mutex> lock(history_mutex_);
    
    if (historical_data_.size() < 20) return;
    
    // Simple gradient descent for quantile regression
    const double learning_rate = 0.01;
    const int iterations = 100;
    
    for (size_t q = 0; q < QUANTILE_LEVELS; ++q) {
        double quantile_level = 0.25 + q * 0.25; // 0.25, 0.5, 0.75
        
        for (int iter = 0; iter < iterations; ++iter) {
            std::array<double, MAX_FEATURES> gradients{};
            
            for (const auto& record : historical_data_) {
                // Calculate prediction
                double prediction = 0.0;
                for (size_t f = 0; f < MAX_FEATURES; ++f) {
                    prediction += coefficients_[q][f] * record.features[f];
                }
                
                // Quantile loss gradient
                double error = record.actualSlippage - prediction;
                double loss_gradient = (error > 0) ? quantile_level : (quantile_level - 1.0);
                
                // Accumulate gradients
                for (size_t f = 0; f < MAX_FEATURES; ++f) {
                    gradients[f] += loss_gradient * record.features[f];
                }
            }
            
            // Apply gradients
            double inv_size = 1.0 / static_cast<double>(historical_data_.size());
            for (size_t f = 0; f < MAX_FEATURES; ++f) {
                gradients[f] *= inv_size;
                coefficients_[q][f] -= learning_rate * gradients[f];
            }
        }
    }
    
    logger_.info("Slippage model recalibrated with {} samples", historical_data_.size());
}

void SlippageModel::setCoefficients(const std::array<std::array<double, MAX_FEATURES>, QUANTILE_LEVELS>& coefficients) {
    coefficients_ = coefficients;
    logger_.info("Slippage model coefficients updated");
}

std::array<std::array<double, SlippageModel::MAX_FEATURES>, SlippageModel::QUANTILE_LEVELS> SlippageModel::getCoefficients() const {
    return coefficients_;
}

SlippageModel::SlippageResult SlippageModel::predict_slippage_hft(
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    const std::array<double, 5>& ask_quantities,
    const std::array<double, 5>& bid_quantities,
    bool is_buy
) {
    try {
        // Calculate total depth from atomic data
        double total_depth = 0.0;
        const auto& quantities = is_buy ? ask_quantities : bid_quantities;
        for (size_t i = 0; i < quantities.size(); ++i) {
            total_depth += quantities[i];
        }
        
        // Use stack-allocated feature array (zero allocation)
        std::array<double, MAX_FEATURES> features;
        extract_features_atomic(features, quantity, mid_price, spread,
                              volatility, imbalance, total_depth, is_buy);
        
        // Calculate quantile predictions
        std::array<double, QUANTILE_LEVELS> predictions;
        
        for (size_t q = 0; q < QUANTILE_LEVELS; ++q) {
            predictions[q] = 0.0;
            for (size_t f = 0; f < MAX_FEATURES; ++f) {
                predictions[q] += coefficients_[q][f] * features[f];
            }
        }
        
        // Ensure minimum slippage (half spread)
        double min_slippage = spread * 0.5;
        for (auto& pred : predictions) {
            pred = std::max(pred, min_slippage);
        }
        
        // Adjust for direction
        if (!is_buy) {
            for (auto& pred : predictions) {
                pred = -pred;
            }
        }
        
        return {
            predictions[1], // Median as expected
            predictions[0], // 25th percentile as lower bound
            predictions[2], // 75th percentile as upper bound
            0.85 // Default prediction quality
        };
        
    } catch (const std::exception& e) {
        logger_.error("Failed to predict slippage from atomics: {}", e.what());
        return {0.0, 0.0, 0.0, 0.0};
    }
}

void SlippageModel::extract_features_atomic(
    std::array<double, MAX_FEATURES>& features,
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    double total_depth,
    bool is_buy
) {
    features[0] = 1.0; // Intercept
    features[1] = spread / std::max(mid_price, 1.0); // Normalized spread
    features[2] = quantity / std::max(total_depth, 1.0); // Relative size
    features[3] = volatility;
    features[4] = is_buy ? imbalance : -imbalance; // Directional imbalance
    features[5] = 0.5; // Default depth ratio
    features[6] = std::tanh(volatility * 10.0); // Price momentum
    features[7] = std::tanh(imbalance * 5.0); // Volume momentum
}

} // namespace models
} // namespace kubera
