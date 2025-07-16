#include "core/models/market_impact_model.h"
#include "core/utils/portable_math.h"
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace kubera {
namespace models {

MarketImpactModel::MarketImpactModel(
    logging::Logger& logger,
    utils::MemoryPool<1024, 1024>& memory_pool,
    SharedCacheManager& shared_cache,
    const ModelParameters& params
) : parameters_(params), memory_pool_(memory_pool), logger_(logger),
    shared_cache_(shared_cache),
    history_size_(0), pending_records_count_(0), curves_valid_(false) {
    
    // Initialize power caches and impact curves
    initialize_power_caches();
    initialize_impact_curves();
    utils::PortableMath::initialize();
    
    logger_.info("Market impact model initialized with volatility={}, permanent_factor={}, temporary_factor={}",
                parameters_.volatility, parameters_.permanent_impact_factor, parameters_.temporary_impact_factor);
}

// ✅ NEW: HFT optimized method (zero allocation)
MarketImpactModel::ImpactComponents MarketImpactModel::calculate_impact_from_atomics(
    double quantity,
    double mid_price,
    double spread,
    double volatility,
    double imbalance,
    const std::array<double, 5>& ask_prices,
    const std::array<double, 5>& ask_quantities,
    const std::array<double, 5>& bid_prices,
    const std::array<double, 5>& bid_quantities,
    bool is_buy) const {
    
    try {
        quantity = std::abs(quantity);
        
        // Calculate effective depth from atomic data
        double ask_depth = calculate_atomic_depth(ask_quantities, true);
        double bid_depth = calculate_atomic_depth(bid_quantities, false);
        
        return calculate_impact_direct(
            quantity, mid_price, spread,
            is_buy ? ask_depth : bid_depth,
            volatility, imbalance, bid_depth, ask_depth, is_buy
        );
        
    } catch (const std::exception& e) {
        logger_.error("Failed to calculate impact from atomics: {}", e.what());
        return ImpactComponents(0.0, 0.0, 0.0);
    }
}

// ✅ Direct impact calculation with optimized math
MarketImpactModel::ImpactComponents MarketImpactModel::calculate_impact_direct(
    double quantity,
    double mid_price,
    double spread,
    double effective_depth,
    double volatility,
    double imbalance,
    double bid_depth,
    double ask_depth,
    bool is_buy) const {
    
    quantity = std::abs(quantity);
    
    // Select appropriate depth and adjust for imbalance
    double market_depth = is_buy ? ask_depth : bid_depth;
    market_depth = std::max(market_depth, 1.0); // Prevent division by zero
    
    double depth_adjustment = 1.0 + (is_buy ? -imbalance : imbalance) * 0.5;
    market_depth *= std::max(0.1, depth_adjustment);
    
    double immediate = spread * 0.5;
    
    // Get current parameters safely
    std::shared_lock lock(parameters_mutex_);
    double vol = parameters_.volatility;
    double perm_factor = parameters_.permanent_impact_factor;
    double temp_factor = parameters_.temporary_impact_factor;
    lock.unlock();
    
    // Use cached power values for efficiency
    double q_pow_025 = lookup_power(quantity, 0.25, power_cache_025_);
    double q_pow_075 = lookup_power(quantity, 0.75, power_cache_075_);
    
    // Almgren-Chriss permanent impact
    double permanent = vol * perm_factor * q_pow_025 * mid_price / std::sqrt(market_depth);
    
    // Temporary impact
    double temporary = vol * temp_factor * q_pow_075 * mid_price / std::pow(market_depth, 0.375);
    
    if (!is_buy) {
        permanent = -permanent;
        temporary = -temporary;
    }
    
    return ImpactComponents(immediate, permanent, temporary);
}

// ✅ Optimized atomic depth calculation
double MarketImpactModel::calculate_atomic_depth(
    const std::array<double, 5>& quantities,
    bool is_buy) const {
    
    double total_depth = 0.0;
    double weighted_depth = 0.0;
    
    for (size_t i = 0; i < quantities.size(); ++i) {
        double quantity = quantities[i];
        if (quantity <= 0.0) break;
        
        double weight = 1.0 / (1.0 + static_cast<double>(i) * 0.1);
        total_depth += quantity;
        weighted_depth += quantity * weight;
    }
    
    return weighted_depth > 0 ? weighted_depth : std::max(total_depth, 1000.0);
}

// ✅ Power cache initialization
void MarketImpactModel::initialize_power_caches() {
    power_cache_025_.resize(POWER_CACHE_SIZE);
    power_cache_050_.resize(POWER_CACHE_SIZE);
    power_cache_075_.resize(POWER_CACHE_SIZE);
    
    for (size_t i = 0; i < POWER_CACHE_SIZE; ++i) {
        double value = static_cast<double>(i) / 100.0; // 0.00 to 9.99
        power_cache_025_[i] = std::pow(value, 0.25);
        power_cache_050_[i] = std::pow(value, 0.50);
        power_cache_075_[i] = std::pow(value, 0.75);
    }
    
    logger_.debug("Power caches initialized with {} entries", POWER_CACHE_SIZE);
}

// ✅ Impact curves initialization
void MarketImpactModel::initialize_impact_curves() const {
    std::shared_lock lock(parameters_mutex_);
    
    for (size_t i = 0; i < CACHE_SIZE; ++i) {
        double quantity = static_cast<double>(i) / 100.0; // 0.00 to 10.23
        
        // Almgren-Chriss permanent impact: volatility * factor * quantity^0.25
        double q_pow_025 = lookup_power(quantity, 0.25, power_cache_025_);
        permanent_impact_curve_[i] = parameters_.volatility * 
                                   parameters_.permanent_impact_factor * 
                                   q_pow_025;
        
        // Temporary impact: volatility * factor * quantity^0.75
        double q_pow_075 = lookup_power(quantity, 0.75, power_cache_075_);
        temporary_impact_curve_[i] = parameters_.volatility * 
                                   parameters_.temporary_impact_factor * 
                                   q_pow_075;
    }
    
    curves_valid_.store(true, std::memory_order_release);
    logger_.debug("Impact curves initialized for {} quantity levels", CACHE_SIZE);
}

// ✅ Power lookup with caching
double MarketImpactModel::lookup_power(double value, double exponent, const std::vector<double>& cache) const {
    // Use cache for values in range [0, 10)
    if (value >= 0.0 && value < 10.0) {
        size_t index = static_cast<size_t>(value * 100.0);
        if (index < cache.size()) {
            return cache[index];
        }
    }
    
    // Fall back to direct calculation for large values
    return calculate_power_direct(value, exponent);
}

double MarketImpactModel::calculate_power_direct(double value, double exponent) const {
    if (std::abs(exponent - 0.25) < 1e-9) {
        return utils::PortableMath::fast_pow025(static_cast<float>(value));
    } else if (std::abs(exponent - 0.50) < 1e-9) {
        return std::sqrt(value);
    } else if (std::abs(exponent - 0.75) < 1e-9) {
        return utils::PortableMath::fast_pow075(static_cast<float>(value));
    }
    
    return std::pow(value, exponent);
}

// ✅ Keep original method for backwards compatibility
MarketImpactModel::ImpactComponents MarketImpactModel::calculateImpact(
    double quantity,
    const orderbook::OrderBook& order_book,
    bool is_buy) const {
    
    try {
        auto snapshot = order_book.getSnapshot();
        
        // Convert to atomic arrays for HFT method
        std::array<double, 5> ask_prices{}, ask_quantities{}, bid_prices{}, bid_quantities{};
        
        for (size_t i = 0; i < 5 && i < snapshot.asks.size(); ++i) {
            ask_prices[i] = snapshot.asks[i].price;
            ask_quantities[i] = snapshot.asks[i].quantity;
        }
        
        for (size_t i = 0; i < 5 && i < snapshot.bids.size(); ++i) {
            bid_prices[i] = snapshot.bids[i].price;
            bid_quantities[i] = snapshot.bids[i].quantity;
        }
        
        return calculate_impact_from_atomics(
            quantity, snapshot.midPrice, snapshot.spread, snapshot.volatility,
            snapshot.imbalance, ask_prices, ask_quantities, bid_prices, bid_quantities, is_buy
        );
        
    } catch (const std::exception& e) {
        logger_.error("Failed to calculate market impact: {}", e.what());
        return ImpactComponents(0.0, 0.0, 0.0);
    }
}

// ✅ Thread-safe parameter management
void MarketImpactModel::update_parameters(const ModelParameters& new_params) {
    {
        std::unique_lock lock(parameters_mutex_);
        parameters_ = new_params;
    }
    
    // Reinitialize impact curves with new parameters
    initialize_impact_curves();
    
    logger_.info("Impact model parameters updated: vol={}, perm={}, temp={}",
                new_params.volatility, new_params.permanent_impact_factor, new_params.temporary_impact_factor);
}

MarketImpactModel::ModelParameters MarketImpactModel::get_parameters() const {
    std::shared_lock lock(parameters_mutex_);
    return parameters_;
}

// ✅ Individual parameter setters
void MarketImpactModel::setVolatility(double volatility) {
    ModelParameters new_params = get_parameters();
    new_params.volatility = volatility;
    update_parameters(new_params);
}

void MarketImpactModel::setPermanentImpactFactor(double factor) {
    ModelParameters new_params = get_parameters();
    new_params.permanent_impact_factor = factor;
    update_parameters(new_params);
}

void MarketImpactModel::setTemporaryImpactFactor(double factor) {
    ModelParameters new_params = get_parameters();
    new_params.temporary_impact_factor = factor;
    update_parameters(new_params);
}

// ✅ Individual parameter getters
double MarketImpactModel::getVolatility() const {
    std::shared_lock lock(parameters_mutex_);
    return parameters_.volatility;
}

double MarketImpactModel::getPermanentImpactFactor() const {
    std::shared_lock lock(parameters_mutex_);
    return parameters_.permanent_impact_factor;
}

double MarketImpactModel::getTemporaryImpactFactor() const {
    std::shared_lock lock(parameters_mutex_);
    return parameters_.temporary_impact_factor;
}

// ✅ Model training and learning
void MarketImpactModel::updateModel(
    double quantity,
    const orderbook::OrderBook& orderBook,
    bool isBuy,
    const ImpactComponents& actualImpact) {
    
    try {
        auto snapshot = orderBook.getSnapshot();
        ImpactData data;
        
        data.market_features[0] = snapshot.midPrice;
        data.market_features[1] = snapshot.spread;
        data.market_features[2] = calculate_effective_depth_from_snapshot(snapshot, isBuy);
        data.market_features[3] = snapshot.volatility;
        data.market_features[4] = snapshot.imbalance;
        data.market_features[5] = calculate_effective_depth_from_snapshot(snapshot, false); // bid depth
        data.market_features[6] = calculate_effective_depth_from_snapshot(snapshot, true);  // ask depth
        data.market_features[7] = quantity;
        data.actual_impact = actualImpact;
        data.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        new_impact_records_.enqueue(data);
        pending_records_count_.fetch_add(1, std::memory_order_relaxed);
        
        logger_.debug("Impact model updated with new data");
        
    } catch (const std::exception& e) {
        logger_.error("Failed to update impact model: {}", e.what());
    }
}

double MarketImpactModel::calculate_effective_depth_from_snapshot(
    const kubera::orderbook::OrderBookSnapshot& snapshot,
    bool is_buy) const {
    
    const auto& levels = is_buy ? snapshot.asks : snapshot.bids;
    if (levels.empty()) return 1000.0; // Default depth
    
    double total_depth = 0.0;
    double weighted_depth = 0.0;
    size_t max_levels = std::min(levels.size(), static_cast<size_t>(MAX_DEPTH_LEVELS));
    
    for (size_t i = 0; i < max_levels; ++i) {
        double quantity = levels[i].quantity;
        double weight = 1.0 / (1.0 + static_cast<double>(i) * 0.1); // Distance decay
        total_depth += quantity;
        weighted_depth += quantity * weight;
    }
    
    return weighted_depth > 0 ? weighted_depth : total_depth;
}

// ✅ Background processing methods
void MarketImpactModel::process_new_impact_records() const {
    try {
        if (pending_records_count_.load() < 10) {
            return;
        }
        
        process_impact_records_batch();
    } catch (const std::exception& e) {
        logger_.error("Failed to process impact records: {}", e.what());
    }
}

void MarketImpactModel::process_impact_records_batch() const {
    std::vector<ImpactData> batch;
    batch.reserve(100);
    
    // Drain queue without holding mutex
    ImpactData record;
    while (batch.size() < 100 && new_impact_records_.try_dequeue(record)) {
        batch.push_back(record);
    }
    
    if (!batch.empty()) {
        {
            std::lock_guard lock(history_mutex_);
            for (const auto& data : batch) {
                historical_data_.push_back(data);
                if (historical_data_.size() > MAX_HISTORY) {
                    historical_data_.pop_front();
                }
            }
            
            history_size_.store(historical_data_.size(), std::memory_order_relaxed);
            pending_records_count_.fetch_sub(batch.size(), std::memory_order_relaxed);
        }
        
        // Calibrate outside of lock to prevent deadlock
        if (history_size_.load() >= 50) {
            calibrateModel();
        }
    }
}

void MarketImpactModel::calibrateModel() const {
    std::vector<ImpactData> training_data;
    
    {
        std::lock_guard lock(history_mutex_);
        if (historical_data_.size() < 10) return;
        training_data.assign(historical_data_.begin(), historical_data_.end());
    }
    
    // Calibrate without holding lock (prevents deadlock)
    logger_.info("Calibrating impact model with {} samples", training_data.size());
    // TODO: Implement proper impact model calibration (least squares regression)
}

void MarketImpactModel::triggerBackgroundProcessing() {
    process_new_impact_records();
}

// ✅ Cache management delegates to shared cache
void MarketImpactModel::invalidate_cache() {
    shared_cache_.invalidate_cache();
}

void MarketImpactModel::force_cache_update(const orderbook::OrderBook& order_book) {
    shared_cache_.update_from_orderbook(order_book);
}

} // namespace models
} // namespace kubera
