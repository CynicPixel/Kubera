#pragma once

#include <atomic>
#include <chrono>
#include <unordered_map>
#include <array>
#include "core/models/slippage_model.h"
#include "core/models/market_impact_model.h"
#include "core/models/maker_taker_model.h"
#include "core/models/fee_calculator.h"
#include "core/orderbook/orderbook.h"
#include "logging/logger.h"

namespace kubera {
namespace hft {

class HFTModelManager {
public:
    struct ModelResults {
        double expected_slippage;
        double expected_impact;
        double maker_proportion;
        double expected_fees;
        double net_cost;
        uint64_t calculation_count;
        uint64_t last_update_time;
    };
    
    struct PerformanceStats {
        uint64_t total_calculations;
        uint64_t total_updates;
        double total_calc_time_us;
        double total_update_time_us;
    };
    
    struct AtomicSnapshot {
        uint64_t sequence;
        double mid_price;
        double spread;
        double volatility;
        double imbalance;
        std::array<double, 5> ask_prices;
        std::array<double, 5> ask_quantities;
        std::array<double, 5> bid_prices;
        std::array<double, 5> bid_quantities;
    };
    
    // ✅ NEW: Common features for all models
    struct CommonFeatures {
        double normalized_spread;
        double relative_size;
        double bounded_volatility;
        double directional_imbalance;
        double log_quantity;
        double effective_depth;
        double depth_ratio;
        double urgency_factor;
    };
    
    // ✅ NEW: Cache key for result caching
    struct CacheKey {
        double mid_price;
        double spread;
        double volatility;
        double imbalance;
        uint64_t sequence;
        double quantity;
        bool is_buy;
        int fee_tier;
        
        bool operator==(const CacheKey& other) const {
            return mid_price == other.mid_price && 
                   spread == other.spread && 
                   volatility == other.volatility && 
                   imbalance == other.imbalance &&
                   sequence == other.sequence &&
                   quantity == other.quantity &&
                   is_buy == other.is_buy &&
                   fee_tier == other.fee_tier;
        }
    };
    
    struct CacheKeyHash {
        size_t operator()(const CacheKey& key) const {
            size_t h1 = std::hash<double>{}(key.mid_price);
            size_t h2 = std::hash<double>{}(key.spread);
            size_t h3 = std::hash<uint64_t>{}(key.sequence);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };
    
    struct CachedResult {
        double expected_slippage;
        double expected_impact;
        double maker_proportion;
        double expected_fees;
        double net_cost;
    };

private:
    // Model references
    models::SlippageModel& slippage_model_;
    models::MarketImpactModel& impact_model_;
    models::MakerTakerModel& maker_taker_model_;
    models::FeeCalculator& fee_calculator_;
    logging::Logger& logger_;
    orderbook::OrderBook& orderBook_;
    
    // HFT model inputs (atomic)
    struct HFTModelInputs {
        std::atomic<uint64_t> sequence_number{0};
        std::atomic<double> mid_price{0.0};
        std::atomic<double> spread{0.0};
        std::atomic<double> volatility{0.0};
        std::atomic<double> imbalance{0.0};
        std::array<std::atomic<double>, 5> ask_prices{};
        std::array<std::atomic<double>, 5> ask_quantities{};
        std::array<std::atomic<double>, 5> bid_prices{};
        std::array<std::atomic<double>, 5> bid_quantities{};
        
        void updateFromOrderBook(const orderbook::OrderBook& orderBook) {
            auto snapshot = orderBook.getSnapshot();
            sequence_number.store(snapshot.sequenceNumber, std::memory_order_release);
            mid_price.store(snapshot.midPrice, std::memory_order_release);
            spread.store(snapshot.spread, std::memory_order_release);
            volatility.store(snapshot.volatility, std::memory_order_release);
            imbalance.store(snapshot.imbalance, std::memory_order_release);
            
            // Update price/quantity arrays
            for (size_t i = 0; i < 5; ++i) {
                if (i < snapshot.asks.size()) {
                    ask_prices[i].store(snapshot.asks[i].price, std::memory_order_release);
                    ask_quantities[i].store(snapshot.asks[i].quantity, std::memory_order_release);
                }
                if (i < snapshot.bids.size()) {
                    bid_prices[i].store(snapshot.bids[i].price, std::memory_order_release);
                    bid_quantities[i].store(snapshot.bids[i].quantity, std::memory_order_release);
                }
            }
        }
    } model_inputs_;
    
    // Results (atomic)
    struct HFTModelResults {
        std::atomic<double> expected_slippage{0.0};
        std::atomic<double> expected_impact{0.0};
        std::atomic<double> maker_proportion{0.5};
        std::atomic<double> expected_fees{0.0};
        std::atomic<double> net_cost{0.0};
        std::atomic<uint64_t> calculation_count{0};
        std::atomic<uint64_t> last_update_time{0};
    } results_;
    
    // Performance counters
    std::atomic<uint64_t> total_calculations_{0};
    std::atomic<uint64_t> total_updates_{0};
    std::atomic<double> total_calc_time_us_{0.0};
    std::atomic<double> total_update_time_us_{0.0};
    
    // ✅ NEW: Result caching
    mutable std::unordered_map<CacheKey, CachedResult, CacheKeyHash> result_cache_;
    mutable std::chrono::steady_clock::time_point last_cache_clear_;
    
    // ✅ NEW: Helper methods
    CommonFeatures extract_common_features(const AtomicSnapshot& snapshot, double quantity, bool is_buy) const;
    bool check_cache_and_update_results(const AtomicSnapshot& snapshot, double quantity, bool is_buy, int fee_tier);
    void store_results_in_cache(const CacheKey& key, double slippage, double impact, double maker_proportion, double fees, double net_cost);

public:
    HFTModelManager(
        models::SlippageModel& slippageModel,
        models::MarketImpactModel& impactModel,
        models::MakerTakerModel& makerTakerModel,
        models::FeeCalculator& feeCalculator,
        logging::Logger& logger,
        orderbook::OrderBook& orderBook
    );
    
    // Market data updates
    void updateMarketData(const orderbook::OrderBook& orderBook);
    
    // Model calculations
    void calculateModels(double quantity, bool is_buy, int fee_tier);
    void calculateModelsHFT(double quantity, bool is_buy, int fee_tier);
    
    // Results access
    ModelResults getResults() const;
    PerformanceStats getPerformanceStats() const;
    
    // Utility methods
    AtomicSnapshot loadAtomicSnapshot() const;
};

} // namespace hft
} // namespace kubera
