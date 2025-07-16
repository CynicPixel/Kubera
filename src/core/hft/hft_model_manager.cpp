#include "core/hft/hft_model_manager.h"
#include <chrono>

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
    
    logger_.debug("Market data updated in {:.2f}Î¼s", duration_us);
}

void HFTModelManager::calculateModels(double quantity, bool is_buy, int fee_tier) {
    auto start = std::chrono::high_resolution_clock::now();
    
    try {
        // Load atomic inputs once for consistency
        uint64_t sequence = model_inputs_.sequence_number.load(std::memory_order_acquire);
        double mid_price = model_inputs_.mid_price.load(std::memory_order_acquire);
        double spread = model_inputs_.spread.load(std::memory_order_acquire);
        double volatility = model_inputs_.volatility.load(std::memory_order_acquire);
        double imbalance = model_inputs_.imbalance.load(std::memory_order_acquire);
        
        // Debug logging to see what's happening
        logger_.info("Model calculation inputs: mid_price={}, spread={}, volatility={}, imbalance={}, sequence={}", 
                     mid_price, spread, volatility, imbalance, sequence);
        
        // Validate we have market data (only check mid_price, spread can be 0)
        if (mid_price <= 0.0) {
            logger_.info("No valid market data available for calculation (mid_price={})", mid_price);
            
            // Set default values to prevent crashes
            results_.expected_slippage.store(0.0, std::memory_order_release);
            results_.expected_impact.store(0.0, std::memory_order_release);
            results_.maker_proportion.store(0.5, std::memory_order_release);
            results_.expected_fees.store(0.0, std::memory_order_release);
            results_.net_cost.store(0.0, std::memory_order_release);
            return;
        }
        
        // Use the latest market data from atomic snapshot
        // If models require an OrderBook, use the shared instance updated via updateMarketData
        // For now, assume models can use atomic data directly or you can pass the shared OrderBook
        // TODO: Refactor models to use atomic data directly for true lock-free performance
        // If not possible, pass the shared OrderBook instance (not a temporary one)

        // Validate we have meaningful market data
        // (mid_price already checked above)
        // If you need to check orderbook levels, you can add checks here

        // Calculate models using shared OrderBook
        // You may need to store a pointer/reference to the shared OrderBook in HFTModelManager
        // For now, assume updateMarketData keeps the shared OrderBook up to date
        // If you need to pass the shared OrderBook, add a member variable and set it in updateMarketData

        // Example: auto& orderBook = sharedOrderBook_; // Add sharedOrderBook_ as a member variable

        // Calculate models using existing interfaces
        auto slippage_result = slippage_model_.predictSlippage(quantity, orderBook_, is_buy);
        double slippage = slippage_result.expected;

        auto impact_components = impact_model_.calculateImpact(quantity, orderBook_, is_buy);
        double total_impact = impact_components.total;

        double maker_proportion = maker_taker_model_.predictMakerProportion(quantity, orderBook_, is_buy);

        double fees = fee_calculator_.calculateFees(quantity, mid_price, fee_tier, maker_proportion);

        double net_cost = slippage + total_impact + fees;

        // Atomic result updates
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

        auto end = std::chrono::high_resolution_clock::now();
        auto duration_us = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0;

        // Update performance counters
        total_calculations_.fetch_add(1, std::memory_order_relaxed);
        total_calc_time_us_.store(
            (total_calc_time_us_.load() + duration_us) / total_calculations_.load(),
            std::memory_order_relaxed
        );

        logger_.debug("Models calculated in {:.2f}Î¼s: slippage={:.4f}, impact={:.4f}, fees={:.4f}, net={:.4f}",
                     duration_us, slippage, total_impact, fees, net_cost);

    } catch (const std::exception& e) {
        logger_.error("Model calculation failed: {}", e.what());
    }
}

void HFTModelManager::calculateModelsHFT(double quantity, bool is_buy, int fee_tier) {
    auto start = std::chrono::high_resolution_clock::now();
    
    // STACK PROTECTION: Prevent recursive calls and deep stacks
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
        
        // PHASE 2: Direct atomic calculations (bypasses OrderBook entirely)
        // For now, fall back to existing calculateModels method but with improved guard
        static std::atomic<bool> calculating{false};
        
        // Use compare_exchange to avoid blocking
        bool expected = false;
        if (!calculating.compare_exchange_weak(expected, true)) {
            // Already calculating in another thread, skip to avoid contention
            recursion_depth--;
            return;
        }
        
        // RAII guard to ensure calculating is reset
        auto guard = [&]() { calculating.store(false); };
        
        calculateModels(quantity, is_buy, fee_tier);
        
        guard(); // Reset calculating flag
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration_us = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0;
        
        logger_.debug("HFT models calculated in {:.2f}Î¼s", duration_us);
        
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

ModelResults HFTModelManager::getResults() const {
    // Atomic reads for consistent UI updates
    return {
        results_.expected_slippage.load(std::memory_order_acquire),
        results_.expected_impact.load(std::memory_order_acquire),
        results_.maker_proportion.load(std::memory_order_acquire),
        results_.expected_fees.load(std::memory_order_acquire),
        results_.net_cost.load(std::memory_order_acquire),
        results_.calculation_count.load(std::memory_order_acquire),
        results_.last_update_time.load(std::memory_order_acquire)
    };
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