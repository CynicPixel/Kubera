// fee_calculator.h
#pragma once

#include <array>
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

private:
    // Pre-computed fee lookup table for common scenarios
    static constexpr size_t TIER_COUNT = 10;
    static constexpr size_t PROPORTION_STEPS = 101; // 0% to 100% in 1% steps
    
    std::unordered_map<int, FeeTier> feeTiers_;
    std::array<std::array<double, PROPORTION_STEPS>, TIER_COUNT> fee_lookup_table_;
    
    logging::Logger& logger_;
    
    // Private methods
    void initialize_fee_tiers();
    void initialize_lookup_table();
    void update_lookup_table_for_tier(int tier);

public:
    explicit FeeCalculator(logging::Logger& logger);
    ~FeeCalculator() = default;
    
    // Main calculation methods
    double calculateFees(double quantity, double price, int feeTier, double makerProportion) const;
    FeeBreakdown calculate_fees_detailed(double quantity, double price, int fee_tier, double maker_proportion);
    
    // Tier management
    FeeTier getFeeTier(int tier) const;
    void setFeeTier(int tier, double makerFee, double takerFee);
    int determine_fee_tier(double monthly_volume_usd);
    
    // Fee optimization
    struct FeeOptimization {
        double optimal_maker_proportion;
        double potential_savings;
        int recommended_tier;
    };
    
    FeeOptimization suggest_optimization(
        double quantity,
        double price,
        int current_tier,
        double current_maker_proportion,
        double monthly_volume
    );
    
    // Statistics
    struct FeeStatistics {
        double min_effective_rate;
        double max_effective_rate;
        double avg_effective_rate;
        int best_tier_for_volume;
    };
    
    FeeStatistics get_fee_statistics(double monthly_volume);
};

} // namespace models
} // namespace kubera
