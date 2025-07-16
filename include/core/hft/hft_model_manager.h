#pragma once

#include <atomic>
#include <array>
#include <memory>
#include "core/orderbook/orderbook.h"
#include "core/models/slippage_model.h"
#include "core/models/market_impact_model.h"
#include "core/models/maker_taker_model.h"
#include "core/models/fee_calculator.h"
#include "logging/logger.h"

namespace kubera {
namespace hft {

/**
 * @brief Lock-free model inputs structure - replaces snapshot queues
 * Cache-aligned for optimal performance
 */
struct alignas(64) HFTModelInputs {
    // Core price data (atomic updates from OrderBook)
    std::atomic<double> mid_price{0.0};
    std::atomic<double> spread{0.0};
    std::atomic<double> top_ask_price{0.0};
    std::atomic<double> top_bid_price{0.0};
    std::atomic<double> top_ask_qty{0.0};
    std::atomic<double> top_bid_qty{0.0};
    
    // Depth data (fixed-size arrays for cache efficiency)
    std::array<std::atomic<double>, 5> ask_prices;
    std::array<std::atomic<double>, 5> ask_quantities;
    std::array<std::atomic<double>, 5> bid_prices;
    std::array<std::atomic<double>, 5> bid_quantities;
    
    // Market state
    std::atomic<double> volatility{0.0};
    std::atomic<double> imbalance{0.0};
    std::atomic<uint64_t> sequence_number{0};
    
    /**
     * @brief Update from OrderBook (called by WebSocket thread)
     * Sub-microsecond atomic updates - replaces snapshot creation
     */
    void updateFromOrderBook(const orderbook::OrderBook& orderBook) {
        // Direct atomic updates - no locks, no copies
        mid_price.store(orderBook.getMidPrice(), std::memory_order_release);
        spread.store(orderBook.getSpread(), std::memory_order_release);
        volatility.store(orderBook.getVolatility(), std::memory_order_release);
        imbalance.store(orderBook.getImbalance(), std::memory_order_release);
        sequence_number.store(orderBook.getSequenceNumber(), std::memory_order_release);
        
        // Update top 5 levels directly
        const auto asks = orderBook.getAsks();
        const auto bids = orderBook.getBids();
        
        // Update ask levels
        for (size_t i = 0; i < std::min(asks.size(), ask_prices.size()); ++i) {
            ask_prices[i].store(asks[i].price, std::memory_order_release);
            ask_quantities[i].store(asks[i].quantity, std::memory_order_release);
        }
        
        // Clear unused ask levels
        for (size_t i = asks.size(); i < ask_prices.size(); ++i) {
            ask_prices[i].store(0.0, std::memory_order_release);
            ask_quantities[i].store(0.0, std::memory_order_release);
        }
        
        // Update bid levels  
        for (size_t i = 0; i < std::min(bids.size(), bid_prices.size()); ++i) {
            bid_prices[i].store(bids[i].price, std::memory_order_release);
            bid_quantities[i].store(bids[i].quantity, std::memory_order_release);
        }
        
        // Clear unused bid levels
        for (size_t i = bids.size(); i < bid_prices.size(); ++i) {
            bid_prices[i].store(0.0, std::memory_order_release);
            bid_quantities[i].store(0.0, std::memory_order_release);
        }
        
        // Update top level data
        if (!asks.empty()) {
            top_ask_price.store(asks[0].price, std::memory_order_release);
            top_ask_qty.store(asks[0].quantity, std::memory_order_release);
        }
        if (!bids.empty()) {
            top_bid_price.store(bids[0].price, std::memory_order_release);
            top_bid_qty.store(bids[0].quantity, std::memory_order_release);
        }
    }
};

/**
 * @brief Atomic model results structure - replaces result queues
 */
struct alignas(64) HFTModelResults {
    std::atomic<double> expected_slippage{0.0};
    std::atomic<double> expected_impact{0.0};
    std::atomic<double> maker_proportion{0.0};
    std::atomic<double> expected_fees{0.0};
    std::atomic<double> net_cost{0.0};
    std::atomic<uint64_t> calculation_count{0};
    std::atomic<uint64_t> last_update_time{0};
};

/**
 * @brief Non-atomic result structure for UI thread consumption
 */
struct ModelResults {
    double expected_slippage;
    double expected_impact;
    double maker_proportion;
    double expected_fees;
    double net_cost;
    uint64_t calculation_count;
    uint64_t last_update_time;
};

/**
 * @class HFTModelManager
 * @brief Lock-free model management - replaces queue-based architecture
 * Provides sub-20μs end-to-end latency from WebSocket to model results
 */
class HFTModelManager {
public:
    /**
     * @brief Constructor
     * @param slippageModel Reference to slippage model
     * @param impactModel Reference to market impact model  
     * @param makerTakerModel Reference to maker/taker model
     * @param feeCalculator Reference to fee calculator
     * @param logger Logger instance
     * @param orderBook Reference to shared OrderBook instance
     */
    HFTModelManager(
        models::SlippageModel& slippageModel,
        models::MarketImpactModel& impactModel,
        models::MakerTakerModel& makerTakerModel,
        models::FeeCalculator& feeCalculator,
        logging::Logger& logger,
        orderbook::OrderBook& orderBook
    );
    
    /**
     * @brief Update market data from OrderBook (called by WebSocket thread)
     * Replaces snapshot queue with direct atomic updates
     * @param orderBook Current OrderBook state
     */
    void updateMarketData(const orderbook::OrderBook& orderBook);
    
    /**
     * @brief Calculate all models (called by model thread)
     * Lock-free calculation using atomic inputs
     * @param quantity Trade quantity
     * @param is_buy Trade direction
     * @param fee_tier Fee tier for calculation
     */
    void calculateModels(double quantity, bool is_buy, int fee_tier);
    
    /**
     * @brief Calculate all models using direct atomic inputs (HFT optimized)
     * Zero allocation, sub-10μs calculation time
     * @param quantity Trade quantity
     * @param is_buy Trade direction
     * @param fee_tier Fee tier for calculation
     */
    void calculateModelsHFT(double quantity, bool is_buy, int fee_tier);

    /**
     * @brief Get current results (called by UI thread)
     * Atomic reads for consistent results
     * @return Current model results
     */
    ModelResults getResults() const;
    
    /**
     * @brief Get performance statistics
     * @return Performance metrics
     */
    struct PerformanceStats {
        uint64_t total_calculations{0};
        uint64_t total_updates{0};
        double avg_calc_time_us{0.0};
        double avg_update_time_us{0.0};
    };
    PerformanceStats getPerformanceStats() const;

private:
    // Lock-free shared state
    HFTModelInputs model_inputs_;
    HFTModelResults results_;
    
    // Model references
    models::SlippageModel& slippage_model_;
    models::MarketImpactModel& impact_model_;
    models::MakerTakerModel& maker_taker_model_;
    models::FeeCalculator& fee_calculator_;
    
    // Logger
    logging::Logger& logger_;

    // Shared OrderBook reference
    orderbook::OrderBook& orderBook_;
    
    // Performance tracking
    std::atomic<uint64_t> total_calculations_{0};
    std::atomic<uint64_t> total_updates_{0};
    std::atomic<double> total_calc_time_us_{0.0};
    std::atomic<double> total_update_time_us_{0.0};

    // NEW: Helper method to load atomic data consistently
    struct AtomicSnapshot {
        double mid_price;
        double spread;
        double volatility;
        double imbalance;
        uint64_t sequence;
        std::array<double, 5> ask_prices;
        std::array<double, 5> ask_quantities;
        std::array<double, 5> bid_prices;
        std::array<double, 5> bid_quantities;
    };
    
    AtomicSnapshot loadAtomicSnapshot() const;
};

} // namespace hft
} // namespace kubera
