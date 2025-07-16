#pragma once

#include <array>
#include <vector>
#include <unordered_map>
#include "logging/logger.h"

namespace kubera {
namespace models {

class FeeCalculator {
public:
    struct FeeTier {
        double makerFee;
        double takerFee;
    };
    
    struct FeeBreakdown {
        double maker_fees;
        double taker_fees;
        double total_fees;
        double effective_rate;
    };
    
    struct FeeOptimization {
        double optimal_maker_proportion;
        double potential_savings;
        int recommended_tier;
    };
    
    struct FeeStatistics {
        double min_rate;
        double max_rate;
        double avg_rate;
        int best_tier;
    };
    
    // ✅ NEW: Batch processing structure
    struct FeeRequest {
        double quantity;
        double price;
        int fee_tier;
        double maker_proportion;
    };

private:
    static constexpr size_t TIER_COUNT = 10;
    static constexpr size_t PROPORTION_STEPS = 101; // 0% to 100%
    static constexpr size_t FAST_LOOKUP_SIZE = 5;   // Common proportions
    
    std::unordered_map<int, FeeTier> feeTiers_;
    logging::Logger& logger_;
    
    // Lookup tables for optimization
    std::array<std::array<double, PROPORTION_STEPS>, TIER_COUNT> fee_lookup_table_;
    
    // ✅ NEW: Fast lookup for common proportions
    std::array<std::array<double, FAST_LOOKUP_SIZE>, TIER_COUNT> fast_lookup_table_;
    
    void initialize_fee_tiers();
    void initialize_lookup_table();
    void initialize_fast_lookup();
    void update_lookup_table_for_tier(int tier);

public:
    explicit FeeCalculator(logging::Logger& logger);
    
    // ✅ OPTIMIZED: Inline method for best performance
    double calculateFeesOptimized(double quantity, double price, int fee_tier, double maker_proportion) const;
    
    // ✅ Keep original method for backwards compatibility
    double calculateFees(double quantity, double price, int feeTier, double makerProportion) const;
    
    // ✅ NEW: Batch processing
    void calculateFeesBatch(const std::vector<FeeRequest>& requests, std::vector<double>& results) const;
    
    // Detailed analysis
    FeeBreakdown calculate_fees_detailed(double quantity, double price, int fee_tier, double maker_proportion);
    
    // Tier management
    FeeTier getFeeTier(int tier) const;
    void setFeeTier(int tier, double makerFee, double takerFee);
    int determine_fee_tier(double monthly_volume_usd);
    
    // Optimization
    FeeOptimization suggest_optimization(double quantity, double price, int current_tier, 
                                       double current_maker_proportion, double monthly_volume);
    
    // Statistics
    FeeStatistics get_fee_statistics(double monthly_volume);
};

} // namespace models
} // namespace kubera
