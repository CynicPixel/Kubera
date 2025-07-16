#include "core/models/maker_taker_model.h"
#include "core/utils/portable_math.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <chrono>

namespace kubera {
namespace models {

MakerTakerModel::MakerTakerModel(
    logging::Logger& logger, 
    utils::MemoryPool<64, 1024>& memory_pool,
    SharedCacheManager& shared_cache
) : memory_pool_(memory_pool), logger_(logger), 
    shared_cache_(shared_cache),  // ✅ SHARED: Initialize shared cache reference
    history_size_(0), pending_records_count_(0) {
    
    // Initialize with industry-calibrated coefficients
    coefficients_ = {0.5, -2.0, 1.5, 0.8, -0.5, -1.2};
    
    // ✅ REMOVED: Individual cache initialization - using shared cache now
    // current_cache_.store(nullptr, std::memory_order_relaxed);
    // pending_delete_.store(nullptr, std::memory_order_relaxed);
    
    utils::PortableMath::initialize();
    logger_.info("Maker/Taker model initialized with {} coefficients", coefficients_.size());
}

MakerTakerModel::~MakerTakerModel() {
    // ✅ REMOVED: Individual cache cleanup - shared cache manages itself
    // No cache cleanup needed
}

// ✅ SHARED: Cache management methods using shared cache
SharedCacheManager::CacheSnapshot MakerTakerModel::get_cache_snapshot() const {
    return shared_cache_.get_snapshot();
}

void MakerTakerModel::update_shared_cache(const orderbook::OrderBook& order_book) const {
    shared_cache_.update_from_orderbook(order_book);
}

void MakerTakerModel::update_shared_cache_from_snapshot(const OrderBookSnapshot& snapshot) const {
    shared_cache_.update_from_snapshot(snapshot);
}

// ✅ FIXED: Thread-safe coefficient access
double MakerTakerModel::calculate_logit_safe(const std::array<double, MAX_FEATURES>& features) const {
    std::shared_lock<std::shared_mutex> lock(coefficients_mutex_);
    
    double logit = 0.0;
    size_t min_size = std::min(features.size(), coefficients_.size());
    
    for (size_t i = 0; i < min_size; ++i) {
        logit += features[i] * coefficients_[i];
    }
    
    return logit;
}

// ✅ SHARED: Extract features from shared cache data
void MakerTakerModel::extract_features_from_shared_cache(
    std::array<double, MAX_FEATURES>& features,
    const SharedMarketCache* cache,
    double quantity,
    bool is_buy,
    double urgency_factor
) const {
    extract_features_direct(features, quantity, cache->midPrice, cache->spread,
                           cache->volatility, cache->imbalance, cache->totalDepth,
                           is_buy, urgency_factor);
}

// ✅ FIXED: Direct feature extraction
void MakerTakerModel::extract_features_direct(
    std::array<double, MAX_FEATURES>& features,
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    double total_depth,
    bool is_buy,
    double urgency_factor
) const {
    features[0] = 1.0; // intercept
    features[1] = std::log1p(quantity / 100000.0); // relative_size
    features[2] = std::min(spread / std::max(mid_price, 1.0), 0.1); // normalized_spread
    features[3] = std::max(-1.0, std::min(1.0, imbalance)); // bounded_imbalance
    features[4] = std::min(volatility, 1.0); // bounded_volatility
    features[5] = std::max(0.0, std::min(1.0, urgency_factor)); // urgency
}

// ✅ FIXED: Market adjustments with direct parameters
double MakerTakerModel::apply_market_adjustments_direct(double base_proportion, double mid_price, double spread, double volatility) const {
    double spread_ratio = spread / std::max(mid_price, 1.0);
    
    // Wider spreads favor maker orders
    if (spread_ratio > 0.001) {
        base_proportion *= (1.0 + spread_ratio * 10.0);
    }
    
    // High volatility reduces maker proportion
    if (volatility > 0.02) {
        base_proportion *= (1.0 - volatility * 5.0);
    }
    
    return std::max(0.05, std::min(0.95, base_proportion));
}

// ✅ SHARED: Main prediction method with shared cache and exception safety
double MakerTakerModel::predict_maker_proportion(
    double quantity,
    const orderbook::OrderBook& order_book,
    bool is_buy,
    double urgency_factor
) const {
    try {
        // Background processing (non-blocking)
        process_new_trade_records();
        
        auto current_time_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        uint64_t ob_version = order_book.getSequenceNumber();
        
        // Get shared cache snapshot to eliminate TOCTOU
        auto cache_snapshot = get_cache_snapshot();
        
        if (cache_snapshot.is_valid(ob_version, current_time_ns)) {
            // Cache hit - use shared cached values
            std::array<double, MAX_FEATURES> features;
            extract_features_from_shared_cache(features, cache_snapshot.data, quantity, is_buy, urgency_factor);
            
            double logit = calculate_logit_safe(features);
            double proportion = 1.0 / (1.0 + std::exp(-logit));
            
            proportion = apply_market_adjustments_direct(proportion, cache_snapshot.data->midPrice,
                                                       cache_snapshot.data->spread, cache_snapshot.data->volatility);
            
            return std::max(0.0, std::min(1.0, proportion));
        }
        
        // Cache miss - update shared cache and calculate
        auto snapshot = order_book.getSnapshot();
        update_shared_cache_from_snapshot(snapshot);
        
        // Use fresh shared cache
        auto fresh_cache = get_cache_snapshot();
        if (fresh_cache.data) {
            std::array<double, MAX_FEATURES> features;
            extract_features_from_shared_cache(features, fresh_cache.data, quantity, is_buy, urgency_factor);
            
            double logit = calculate_logit_safe(features);
            double proportion = 1.0 / (1.0 + std::exp(-logit));
            
            proportion = apply_market_adjustments_direct(proportion, fresh_cache.data->midPrice,
                                                       fresh_cache.data->spread, fresh_cache.data->volatility);
            
            return std::max(0.0, std::min(1.0, proportion));
        }
        
        return 0.5; // Safe default
        
    } catch (const std::exception& e) {
        logger_.error("Failed to predict maker proportion: {}", e.what());
        return 0.5;
    }
}

// ✅ SHARED: Fast prediction with minimal overhead
double MakerTakerModel::predict_maker_proportion_fast(
    double quantity,
    const orderbook::OrderBook& order_book,
    bool is_buy,
    double urgency_factor
) const {
    try {
        auto current_time_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        uint64_t ob_version = order_book.getSequenceNumber();
        
        // Fast path: check shared cache first
        auto cache_snapshot = get_cache_snapshot();
        
        if (cache_snapshot.is_valid(ob_version, current_time_ns)) {
            std::array<double, MAX_FEATURES> features;
            extract_features_from_shared_cache(features, cache_snapshot.data, quantity, is_buy, urgency_factor);
            
            double logit = calculate_logit_safe(features);
            double proportion = 1.0 / (1.0 + std::exp(-logit));
            
            proportion = apply_market_adjustments_direct(proportion, cache_snapshot.data->midPrice,
                                                       cache_snapshot.data->spread, cache_snapshot.data->volatility);
            
            return std::max(0.0, std::min(1.0, proportion));
        }
        
        // ✅ CRITICAL FIX: Use single atomic snapshot instead of multiple OrderBook calls
        auto snapshot = order_book.getSnapshot();
        std::array<double, MAX_FEATURES> features;
        
        // Calculate total depth from snapshot
        double ask_depth = 0.0, bid_depth = 0.0;
        for (const auto& level : snapshot.asks) ask_depth += level.quantity;
        for (const auto& level : snapshot.bids) bid_depth += level.quantity;
        double total_depth = ask_depth + bid_depth;
        
        extract_features_direct(features, quantity, snapshot.midPrice,
                               snapshot.spread, snapshot.volatility,
                               snapshot.imbalance, total_depth,
                               is_buy, urgency_factor);
        
        double logit = calculate_logit_safe(features);
        double proportion = 1.0 / (1.0 + std::exp(-logit));
        
        proportion = apply_market_adjustments_direct(proportion, snapshot.midPrice,
                                                   snapshot.spread, snapshot.volatility);
        
        return std::max(0.0, std::min(1.0, proportion));
        
    } catch (const std::exception& e) {
        logger_.error("Failed to predict maker proportion (fast): {}", e.what());
        return 0.5;
    }
}

// ✅ FIXED: HFT method with direct calculation
double MakerTakerModel::predict_proportion_hft(
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    const std::array<double, 5>& ask_quantities,
    const std::array<double, 5>& bid_quantities,
    bool is_buy,
    double urgency_factor
) const {
    try {
        std::array<double, MAX_FEATURES> features;
        double total_depth = std::accumulate(ask_quantities.begin(), ask_quantities.end(), 0.0) +
                           std::accumulate(bid_quantities.begin(), bid_quantities.end(), 0.0);
        
        extract_features_direct(features, quantity, mid_price, spread, volatility,
                               imbalance, total_depth, is_buy, urgency_factor);
        
        double logit = calculate_logit_safe(features);
        double proportion = 1.0 / (1.0 + std::exp(-logit));
        
        proportion = apply_market_adjustments_direct(proportion, mid_price, spread, volatility);
        
        return std::max(0.05, std::min(0.95, proportion));
        
    } catch (const std::exception& e) {
        logger_.error("Failed to predict proportion (HFT): {}", e.what());
        return 0.5;
    }
}

// ✅ FIXED: Alias for HFT method
double MakerTakerModel::calculate_proportion_from_atomics(
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    const std::array<double, 5>& ask_quantities,
    const std::array<double, 5>& bid_quantities,
    bool is_buy,
    double urgency_factor
) const {
    return predict_proportion_hft(quantity, mid_price, spread, volatility, imbalance,
                                 ask_quantities, bid_quantities, is_buy, urgency_factor);
}

// ✅ FIXED: Thread-safe coefficient management
void MakerTakerModel::setCoefficients(const std::vector<double>& coefficients) {
    std::unique_lock<std::shared_mutex> lock(coefficients_mutex_);
    coefficients_ = coefficients;
    logger_.info("Model coefficients updated, new size: {}", coefficients_.size());
}

std::vector<double> MakerTakerModel::getCoefficients() const {
    std::shared_lock<std::shared_mutex> lock(coefficients_mutex_);
    return coefficients_;
}

// ✅ SHARED: Model update with proper exception safety and atomic snapshots
void MakerTakerModel::updateModel(
    double quantity,
    const orderbook::OrderBook& orderBook,
    bool isBuy,
    double actualMakerProportion
) {
    try {
        // ✅ CRITICAL FIX: Use atomic snapshot instead of multiple calls
        auto snapshot = orderBook.getSnapshot();
        
        TradeData data;
        
        // Calculate total depth from snapshot
        double ask_depth = 0.0, bid_depth = 0.0;
        for (const auto& level : snapshot.asks) ask_depth += level.quantity;
        for (const auto& level : snapshot.bids) bid_depth += level.quantity;
        double total_depth = ask_depth + bid_depth;
        
        extract_features_direct(data.features, quantity, snapshot.midPrice,
                               snapshot.spread, snapshot.volatility,
                               snapshot.imbalance, total_depth,
                               isBuy, 0.5);
        
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

// ✅ FIXED: Background processing with batch optimization
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

// ✅ FIXED: Batch processing for reduced lock contention
void MakerTakerModel::process_trade_records_batch() const {
    std::vector<TradeData> batch;
    batch.reserve(100);
    
    // Drain queue without holding mutex
    TradeData record;
    while (batch.size() < 100 && new_trade_records_.try_dequeue(record)) {
        batch.push_back(record);
    }
    
    if (!batch.empty()) {
        std::lock_guard<std::mutex> lock(history_mutex_);
        
        // Process entire batch under single lock
        for (const auto& data : batch) {
            historical_data_.push_back(data);
            
            if (historical_data_.size() > MAX_HISTORY) {
                historical_data_.pop_front();
            }
        }
        
        history_size_.store(historical_data_.size(), std::memory_order_relaxed);
        pending_records_count_.fetch_sub(batch.size(), std::memory_order_relaxed);
        
        // Trigger calibration if enough new data
        if (history_size_.load() >= 50) {
            calibrateModel();
        }
    }
}

// ✅ FIXED: Model calibration placeholder
void MakerTakerModel::calibrateModel() const {
    std::vector<TradeData> training_data;
    
    // ✅ FIXED: Copy training data under lock, then release before calibration
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
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

// ✅ SHARED: Cache management delegates to shared cache
void MakerTakerModel::invalidate_cache() {
    shared_cache_.invalidate_cache();
}

void MakerTakerModel::force_cache_update(const orderbook::OrderBook& order_book) {
    shared_cache_.update_from_orderbook(order_book);
}

// ✅ BACKWARD COMPATIBILITY: Legacy methods implementation

double MakerTakerModel::predictMakerProportion(double quantity, const orderbook::OrderBook& orderBook, bool isBuy) const {
    return predict_maker_proportion(quantity, orderBook, isBuy, 0.5);
}

std::vector<double> MakerTakerModel::extractFeatures(double quantity, const orderbook::OrderBook& orderBook, bool isBuy) const {
    return extract_features_full(quantity, orderBook, isBuy, 0.5);
}

std::vector<double> MakerTakerModel::extract_features_full(
    double quantity,
    const orderbook::OrderBook& order_book,
    bool is_buy,
    double urgency_factor
) const {
    // ✅ FIXED: Use atomic snapshot instead of multiple calls
    auto snapshot = order_book.getSnapshot();
    
    std::vector<double> features(MAX_FEATURES);
    std::array<double, MAX_FEATURES> features_array;
    
    // Calculate total depth from snapshot
    double ask_depth = 0.0, bid_depth = 0.0;
    for (const auto& level : snapshot.asks) ask_depth += level.quantity;
    for (const auto& level : snapshot.bids) bid_depth += level.quantity;
    double total_depth = ask_depth + bid_depth;
    
    extract_features_direct(features_array, quantity, snapshot.midPrice, snapshot.spread, 
                           snapshot.volatility, snapshot.imbalance, total_depth, is_buy, urgency_factor);
    
    std::copy(features_array.begin(), features_array.end(), features.begin());
    return features;
}

std::vector<double> MakerTakerModel::extract_features_cached(
    double quantity,
    const orderbook::OrderBook& order_book,
    bool is_buy,
    double urgency_factor
) const {
    auto current_time_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t ob_version = order_book.getSequenceNumber();
    
    auto cache_snapshot = get_cache_snapshot();
    
    if (cache_snapshot.is_valid(ob_version, current_time_ns)) {
        std::vector<double> features(MAX_FEATURES);
        std::array<double, MAX_FEATURES> features_array;
        extract_features_from_shared_cache(features_array, cache_snapshot.data, quantity, is_buy, urgency_factor);
        std::copy(features_array.begin(), features_array.end(), features.begin());
        return features;
    }
    
    return extract_features_full(quantity, order_book, is_buy, urgency_factor);
}

void MakerTakerModel::extract_features_cached_array(
    std::array<double, MAX_FEATURES>& features,
    double quantity,
    const orderbook::OrderBook& order_book,
    bool is_buy,
    double urgency_factor
) const {
    auto current_time_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t ob_version = order_book.getSequenceNumber();
    
    auto cache_snapshot = get_cache_snapshot();
    
    if (cache_snapshot.is_valid(ob_version, current_time_ns)) {
        extract_features_from_shared_cache(features, cache_snapshot.data, quantity, is_buy, urgency_factor);
    } else {
        // ✅ FIXED: Use atomic snapshot instead of multiple calls
        auto snapshot = order_book.getSnapshot();
        
        // Calculate total depth from snapshot
        double ask_depth = 0.0, bid_depth = 0.0;
        for (const auto& level : snapshot.asks) ask_depth += level.quantity;
        for (const auto& level : snapshot.bids) bid_depth += level.quantity;
        double total_depth = ask_depth + bid_depth;
        
        extract_features_direct(features, quantity, snapshot.midPrice,
                               snapshot.spread, snapshot.volatility,
                               snapshot.imbalance, total_depth,
                               is_buy, urgency_factor);
    }
}

void MakerTakerModel::extract_features_atomic(
    std::array<double, MAX_FEATURES>& features,
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    double total_depth,
    bool is_buy,
    double urgency_factor
) const {
    extract_features_direct(features, quantity, mid_price, spread, volatility,
                           imbalance, total_depth, is_buy, urgency_factor);
}

double MakerTakerModel::calculate_logit_direct(const std::array<double, MAX_FEATURES>& features) const {
    return calculate_logit_safe(features);
}

double MakerTakerModel::apply_market_adjustments(
    double base_proportion,
    const orderbook::OrderBook& order_book
) const {
    // ✅ FIXED: Use atomic snapshot instead of multiple calls
    auto snapshot = order_book.getSnapshot();
    return apply_market_adjustments_direct(base_proportion, snapshot.midPrice,
                                         snapshot.spread, snapshot.volatility);
}

double MakerTakerModel::apply_market_adjustments_cached(double base_proportion) const {
    auto cache_snapshot = get_cache_snapshot();
    if (cache_snapshot.data) {
        return apply_market_adjustments_direct(base_proportion, cache_snapshot.data->midPrice,
                                             cache_snapshot.data->spread, cache_snapshot.data->volatility);
    }
    return base_proportion;
}

bool MakerTakerModel::is_cache_valid(uint64_t ob_version) const {
    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    auto cache_snapshot = get_cache_snapshot();
    return cache_snapshot.is_valid(ob_version, now_ns);
}

void MakerTakerModel::update_cache_atomic(const orderbook::OrderBook& order_book, uint64_t version, uint64_t expiry_time) const {
    shared_cache_.update_from_orderbook(order_book);
}

double MakerTakerModel::predict_from_cache(double quantity, bool is_buy, double urgency_factor) const {
    auto cache_snapshot = get_cache_snapshot();
    if (cache_snapshot.data) {
        std::array<double, MAX_FEATURES> features;
        extract_features_from_shared_cache(features, cache_snapshot.data, quantity, is_buy, urgency_factor);
        
        double logit = calculate_logit_safe(features);
        double proportion = 1.0 / (1.0 + std::exp(-logit));
        
        proportion = apply_market_adjustments_direct(proportion, cache_snapshot.data->midPrice,
                                                   cache_snapshot.data->spread, cache_snapshot.data->volatility);
        
        return std::max(0.0, std::min(1.0, proportion));
    }
    return 0.5;
}

double MakerTakerModel::sigmoid(double x) const {
    return utils::PortableMath::fast_sigmoid(static_cast<float>(x));
}

} // namespace models
} // namespace kubera