#include "core/hft/hft_model_manager.h"
#include <chrono>
#include <unordered_map>

namespace kubera {
namespace hft {

HFTModelManager::HFTModelManager(
    models::SlippageModel& slippageModel,
    models::MarketImpactModel& impactModel,
    models::MakerTakerModel& makerTakerModel,
    models::FeeCalculator& feeCalculator,
    logging::Logger& logger,
    orderbook::OrderBook& orderBook
) : slippage_model_(slippageModel),
    impact_model_(impactModel),
    maker_taker_model_(makerTakerModel),
    fee_calculator_(feeCalculator),
    logger_(logger),
    orderBook_(orderBook) {
    
    // Initialize cache cleanup timer
    last_cache_clear_ = std::chrono::steady_clock::now();
    logger_.info("HFTModelManager initialized - lock-free architecture active");
}

void HFTModelManager::updateMarketData(const orderbook::OrderBook& orderBook) {
    auto start = std::chrono::high_resolution_clock::now();
    
    // Direct atomic update - replaces snapshot creation and queuing
    model_inputs_.updateFromOrderBook(orderBook);
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0;
    
    // Update performance counters
    total_updates_.fetch_add(1, std::memory_order_relaxed);
    total_update_time_us_.store(
        (total_update_time_us_.load() + duration_us) / total_updates_.load(),
        std::memory_order_relaxed
    );
    
    logger_.debug("Market data updated in {:.2f}μs", duration_us);
}

// ✅ NEW: Common feature extraction for all models
HFTModelManager::CommonFeatures HFTModelManager::extract_common_features(
    const AtomicSnapshot& snapshot, 
    double quantity, 
    bool is_buy) const {
    
    CommonFeatures features;
    
    // Pre-calculate common values
    double total_ask_depth = 0.0, total_bid_depth = 0.0;
    for (size_t i = 0; i < 5; ++i) {
        total_ask_depth += snapshot.ask_quantities[i];
        total_bid_depth += snapshot.bid_quantities[i];
    }
    
    features.normalized_spread = snapshot.spread / std::max(snapshot.mid_price, 1.0);
    features.relative_size = quantity / std::max(total_ask_depth + total_bid_depth, 1.0);
    features.bounded_volatility = std::min(snapshot.volatility, 1.0);
    features.directional_imbalance = is_buy ? snapshot.imbalance : -snapshot.imbalance;
    features.log_quantity = std::log1p(quantity / 100000.0);
    features.effective_depth = is_buy ? total_ask_depth : total_bid_depth;
    features.depth_ratio = total_ask_depth / std::max(total_bid_depth, 1.0);
    features.urgency_factor = 0.5; // Default
    
    return features;
}

// ✅ NEW: Result caching implementation
bool HFTModelManager::check_cache_and_update_results(
    const AtomicSnapshot& snapshot,
    double quantity,
    bool is_buy,
    int fee_tier) {
    
    // Create cache key
    CacheKey key{snapshot.mid_price, snapshot.spread, 
                snapshot.volatility, snapshot.imbalance, 
                snapshot.sequence, quantity, is_buy, fee_tier};
    
    // Check cache first
    auto cache_it = result_cache_.find(key);
    if (cache_it != result_cache_.end()) {
        // Cache hit - update atomic results
        const auto& cached = cache_it->second;
        results_.expected_slippage.store(cached.expected_slippage, std::memory_order_release);
        results_.expected_impact.store(cached.expected_impact, std::memory_order_release);
        results_.maker_proportion.store(cached.maker_proportion, std::memory_order_release);
        results_.expected_fees.store(cached.expected_fees, std::memory_order_release);
        results_.net_cost.store(cached.net_cost, std::memory_order_release);
        results_.calculation_count.fetch_add(1, std::memory_order_release);
        results_.last_update_time.store(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count(),
            std::memory_order_release
        );
        return true; // Cache hit
    }
    
    return false; // Cache miss
}

void HFTModelManager::store_results_in_cache(
    const CacheKey& key,
    double slippage,
    double impact,
    double maker_proportion,
    double fees,
    double net_cost) {
    
    // Store in cache
    CachedResult cached_result{slippage, impact, maker_proportion, fees, net_cost};
    result_cache_[key] = cached_result;
    
    // Periodic cache cleanup (every 1000 calculations)
    if (result_cache_.size() > 1000) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_cache_clear_ > std::chrono::milliseconds(100)) {
            result_cache_.clear();
            last_cache_clear_ = now;
        }
    }
}

// ✅ OPTIMIZED: Direct atomic calculations (backwards compatible)
void HFTModelManager::calculateModels(double quantity, bool is_buy, int fee_tier) {
    auto start = std::chrono::high_resolution_clock::now();
    
    try {
        // Load atomic snapshot once for consistency
        auto snapshot = loadAtomicSnapshot();
        
        // Validate we have market data
        if (snapshot.mid_price <= 0.0) {
            logger_.debug("No market data available for calculation");
            // Set default values
            results_.expected_slippage.store(0.0, std::memory_order_release);
            results_.expected_impact.store(0.0, std::memory_order_release);
            results_.maker_proportion.store(0.5, std::memory_order_release);
            results_.expected_fees.store(0.0, std::memory_order_release);
            results_.net_cost.store(0.0, std::memory_order_release);
            return;
        }
        
        // Check cache first
        if (check_cache_and_update_results(snapshot, quantity, is_buy, fee_tier)) {
            return; // Cache hit, results already updated
        }
        
        // Extract common features once
        auto common_features = extract_common_features(snapshot, quantity, is_buy);
        
        // Direct atomic model calculations (no OrderBook needed)
        auto slippage_result = slippage_model_.predict_slippage_hft(
            quantity, snapshot.mid_price, snapshot.spread, snapshot.volatility,
            snapshot.imbalance, snapshot.ask_quantities, snapshot.bid_quantities, is_buy
        );
        double slippage = slippage_result.expected;
        
        auto impact_components = impact_model_.calculate_impact_from_atomics(
            quantity, snapshot.mid_price, snapshot.spread, snapshot.volatility,
            snapshot.imbalance, snapshot.ask_prices, snapshot.ask_quantities,
            snapshot.bid_prices, snapshot.bid_quantities, is_buy
        );
        double total_impact = impact_components.total;
        
        double maker_proportion = maker_taker_model_.predict_proportion_hft(
            quantity, snapshot.mid_price, snapshot.spread, snapshot.volatility,
            snapshot.imbalance, snapshot.ask_quantities, snapshot.bid_quantities,
            is_buy, 0.5
        );
        
        double fees = fee_calculator_.calculateFeesOptimized(
            quantity, snapshot.mid_price, fee_tier, maker_proportion);
        
        double net_cost = slippage + total_impact + fees;
        
        // Store results atomically
        results_.expected_slippage.store(slippage, std::memory_order_release);
        results_.expected_impact.store(total_impact, std::memory_order_release);
        results_.maker_proportion.store(maker_proportion, std::memory_order_release);
        results_.expected_fees.store(fees, std::memory_order_release);
        results_.net_cost.store(net_cost, std::memory_order_release);
        results_.calculation_count.fetch_add(1, std::memory_order_release);
        results_.last_update_time.store(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count(),
            std::memory_order_release
        );
        
        // Cache the results
        CacheKey key{snapshot.mid_price, snapshot.spread, snapshot.volatility,
                    snapshot.imbalance, snapshot.sequence, quantity, is_buy, fee_tier};
        store_results_in_cache(key, slippage, total_impact, maker_proportion, fees, net_cost);
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration_us = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0;
        
        // Update performance counters
        total_calculations_.fetch_add(1, std::memory_order_relaxed);
        total_calc_time_us_.store(
            (total_calc_time_us_.load() + duration_us) / total_calculations_.load(),
            std::memory_order_relaxed
        );
        
        logger_.debug("Models calculated in {:.2f}μs: slippage={:.4f}, impact={:.4f}, fees={:.4f}, net={:.4f}",
                     duration_us, slippage, total_impact, fees, net_cost);
        
    } catch (const std::exception& e) {
        logger_.error("Model calculation failed: {}", e.what());
    }
}

// ✅ OPTIMIZED: HFT method with enhanced performance
void HFTModelManager::calculateModelsHFT(double quantity, bool is_buy, int fee_tier) {
    auto start = std::chrono::high_resolution_clock::now();
    
    // STACK PROTECTION: Prevent recursive calls
    static thread_local int recursion_depth = 0;
    if (recursion_depth > 0) {
        logger_.warning("Preventing recursive call to calculateModelsHFT (depth={})", recursion_depth);
        return;
    }
    
    recursion_depth++;
    
    try {
        // Load atomic inputs once for consistency (zero-copy)
        auto snapshot = loadAtomicSnapshot();
        
        // Validate we have market data
        if (snapshot.mid_price <= 0.0) {
            logger_.debug("No market data available for HFT calculation");
            recursion_depth--;
            return;
        }
        
        // Use optimized calculateModels with caching
        static std::atomic<bool> calculating{false};
        bool expected = false;
        if (!calculating.compare_exchange_weak(expected, true)) {
            recursion_depth--;
            return; // Already calculating, skip
        }
        
        // RAII guard to ensure calculating is reset
        auto guard = [&]() { calculating.store(false); };
        
        // Use the optimized calculateModels method
        calculateModels(quantity, is_buy, fee_tier);
        
        guard(); // Reset calculating flag
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration_us = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0;
        
        logger_.debug("HFT models calculated in {:.2f}μs", duration_us);
        
    } catch (const std::exception& e) {
        logger_.error("HFT model calculation failed: {}", e.what());
    }
    
    recursion_depth--;
}

HFTModelManager::AtomicSnapshot HFTModelManager::loadAtomicSnapshot() const {
    AtomicSnapshot snapshot;
    
    // Load all atomic values consistently
    snapshot.sequence = model_inputs_.sequence_number.load(std::memory_order_acquire);
    snapshot.mid_price = model_inputs_.mid_price.load(std::memory_order_acquire);
    snapshot.spread = model_inputs_.spread.load(std::memory_order_acquire);
    snapshot.volatility = model_inputs_.volatility.load(std::memory_order_acquire);
    snapshot.imbalance = model_inputs_.imbalance.load(std::memory_order_acquire);
    
    // Load price/quantity arrays
    for (size_t i = 0; i < 5; ++i) {
        snapshot.ask_prices[i] = model_inputs_.ask_prices[i].load(std::memory_order_acquire);
        snapshot.ask_quantities[i] = model_inputs_.ask_quantities[i].load(std::memory_order_acquire);
        snapshot.bid_prices[i] = model_inputs_.bid_prices[i].load(std::memory_order_acquire);
        snapshot.bid_quantities[i] = model_inputs_.bid_quantities[i].load(std::memory_order_acquire);
    }
    
    return snapshot;
}

kubera::hft::HFTModelManager::ModelResults kubera::hft::HFTModelManager::getResults() const {
    kubera::hft::HFTModelManager::ModelResults results;
    results.expected_slippage = results_.expected_slippage.load(std::memory_order_acquire);
    results.expected_impact = results_.expected_impact.load(std::memory_order_acquire);
    results.maker_proportion = results_.maker_proportion.load(std::memory_order_acquire);
    results.expected_fees = results_.expected_fees.load(std::memory_order_acquire);
    results.net_cost = results_.net_cost.load(std::memory_order_acquire);
    results.calculation_count = results_.calculation_count.load(std::memory_order_acquire);
    results.last_update_time = results_.last_update_time.load(std::memory_order_acquire);
    return results;
}

HFTModelManager::PerformanceStats HFTModelManager::getPerformanceStats() const {
    return {
        total_calculations_.load(std::memory_order_acquire),
        total_updates_.load(std::memory_order_acquire),
        total_calc_time_us_.load(std::memory_order_acquire),
        total_update_time_us_.load(std::memory_order_acquire)
    };
}

} // namespace hft
} // namespace kubera
