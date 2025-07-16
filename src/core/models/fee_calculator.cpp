#include "core/models/fee_calculator.h"
#include <algorithm>
#include <cmath>

namespace kubera {
namespace models {

FeeCalculator::FeeCalculator(logging::Logger& logger) : logger_(logger) {
    initialize_fee_tiers();
    initialize_lookup_table();
    initialize_fast_lookup();
    logger_.info("Fee calculator initialized with {} tiers", feeTiers_.size());
}

void FeeCalculator::initialize_fee_tiers() {
    // OKX-style fee structure with extended tiers
    feeTiers_ = {
        {1, {0.0008, 0.0010}}, // Tier 1: 0.08% maker, 0.10% taker
        {2, {0.0006, 0.0008}}, // Tier 2: 0.06% maker, 0.08% taker
        {3, {0.0004, 0.0006}}, // Tier 3: 0.04% maker, 0.06% taker
        {4, {0.0002, 0.0004}}, // Tier 4: 0.02% maker, 0.04% taker
        {5, {0.0000, 0.0002}}, // Tier 5: 0.00% maker, 0.02% taker
        {6, {-0.0001, 0.0001}}, // Tier 6: -0.01% maker (rebate), 0.01% taker
        {7, {-0.0002, 0.0001}}, // Tier 7: -0.02% maker (rebate), 0.01% taker
        {8, {-0.0003, 0.0001}}, // Tier 8: -0.03% maker (rebate), 0.01% taker
        {9, {-0.0004, 0.0001}}, // Tier 9: -0.04% maker (rebate), 0.01% taker
        {10, {-0.0005, 0.0001}} // Tier 10: -0.05% maker (rebate), 0.01% taker
    };
}

void FeeCalculator::initialize_lookup_table() {
    for (const auto& [tier_num, tier_info] : feeTiers_) {
        if (tier_num <= static_cast<int>(TIER_COUNT)) {
            size_t tier_index = static_cast<size_t>(tier_num - 1);
            
            for (size_t prop = 0; prop < PROPORTION_STEPS; ++prop) {
                double maker_proportion = static_cast<double>(prop) / 100.0;
                double taker_proportion = 1.0 - maker_proportion;
                
                // Pre-compute effective fee rate
                fee_lookup_table_[tier_index][prop] = 
                    tier_info.makerFee * maker_proportion + 
                    tier_info.takerFee * taker_proportion;
            }
        }
    }
}

// ✅ NEW: Initialize fast lookup for common proportions
void FeeCalculator::initialize_fast_lookup() {
    // Pre-compute for common maker proportions (0%, 25%, 50%, 75%, 100%)
    for (size_t tier = 0; tier < TIER_COUNT; ++tier) {
        for (size_t prop_idx = 0; prop_idx < FAST_LOOKUP_SIZE; ++prop_idx) {
            double maker_proportion = static_cast<double>(prop_idx) / (FAST_LOOKUP_SIZE - 1);
            size_t lookup_idx = static_cast<size_t>(maker_proportion * 100.0);
            lookup_idx = std::min(lookup_idx, PROPORTION_STEPS - 1);
            
            fast_lookup_table_[tier][prop_idx] = fee_lookup_table_[tier][lookup_idx];
        }
    }
}

// ✅ OPTIMIZED: Inline critical calculations
double FeeCalculator::calculateFeesOptimized(
    double quantity, 
    double price, 
    int fee_tier, 
    double maker_proportion) const {
    
    // Fast path for common cases
    if (fee_tier > 0 && fee_tier <= static_cast<int>(TIER_COUNT) && 
        maker_proportion >= 0.0 && maker_proportion <= 1.0) {
        
        size_t tier_index = static_cast<size_t>(fee_tier - 1);
        
        // Check if it's a common proportion for fast lookup
        if (maker_proportion == 0.0) {
            return quantity * price * fast_lookup_table_[tier_index][0];
        } else if (maker_proportion == 0.25) {
            return quantity * price * fast_lookup_table_[tier_index][1];
        } else if (maker_proportion == 0.5) {
            return quantity * price * fast_lookup_table_[tier_index][2];
        } else if (maker_proportion == 0.75) {
            return quantity * price * fast_lookup_table_[tier_index][3];
        } else if (maker_proportion == 1.0) {
            return quantity * price * fast_lookup_table_[tier_index][4];
        }
        
        // Use regular lookup table for other proportions
        size_t prop_index = static_cast<size_t>(maker_proportion * 100.0);
        prop_index = std::min(prop_index, PROPORTION_STEPS - 1);
        
        return quantity * price * fee_lookup_table_[tier_index][prop_index];
    }
    
    // Fallback to full calculation
    return calculateFees(quantity, price, fee_tier, maker_proportion);
}

// ✅ Keep original method for backwards compatibility
double FeeCalculator::calculateFees(double quantity, double price, int feeTier, double makerProportion) const {
    try {
        // Ensure tier is valid
        if (feeTiers_.find(feeTier) == feeTiers_.end()) {
            logger_.warning("Invalid fee tier: {}, using tier 1", feeTier);
            feeTier = 1;
        }
        
        // Ensure maker proportion is within bounds
        makerProportion = std::max(0.0, std::min(1.0, makerProportion));
        
        // Use lookup table for ultra-fast calculation if tier is within range
        if (feeTier <= static_cast<int>(TIER_COUNT)) {
            size_t tier_index = static_cast<size_t>(feeTier - 1);
            size_t prop_index = static_cast<size_t>(makerProportion * 100.0);
            prop_index = std::min(prop_index, PROPORTION_STEPS - 1);
            
            double total_value = quantity * price;
            double effective_rate = fee_lookup_table_[tier_index][prop_index];
            double total_fees = total_value * effective_rate;
            
            logger_.debug("Fees calculated using lookup table: total={} for quantity={}, price={}, tier={}, makerProportion={}",
                         total_fees, quantity, price, feeTier, makerProportion);
            return total_fees;
        }
        
        // Fallback to direct calculation
        const auto& tier = feeTiers_.at(feeTier);
        double totalValue = quantity * price;
        double makerFee = totalValue * makerProportion * tier.makerFee;
        double takerFee = totalValue * (1.0 - makerProportion) * tier.takerFee;
        double totalFees = makerFee + takerFee;
        
        logger_.debug("Fees calculated: maker={}, taker={}, total={} for quantity={}, price={}, tier={}, makerProportion={}",
                     makerFee, takerFee, totalFees, quantity, price, feeTier, makerProportion);
        return totalFees;
        
    } catch (const std::exception& e) {
        logger_.error("Failed to calculate fees: {}", e.what());
        return 0.0;
    }
}

// ✅ NEW: Batch fee calculation for multiple scenarios
void FeeCalculator::calculateFeesBatch(
    const std::vector<FeeRequest>& requests,
    std::vector<double>& results) const {
    
    results.resize(requests.size());
    
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& req = requests[i];
        results[i] = calculateFeesOptimized(req.quantity, req.price, req.fee_tier, req.maker_proportion);
    }
}

// Rest of the methods remain the same for backwards compatibility...
void FeeCalculator::update_lookup_table_for_tier(int tier) {
    if (feeTiers_.find(tier) == feeTiers_.end() || tier > static_cast<int>(TIER_COUNT)) {
        return;
    }
    
    size_t tier_index = static_cast<size_t>(tier - 1);
    const auto& tier_info = feeTiers_.at(tier);
    
    for (size_t prop = 0; prop < PROPORTION_STEPS; ++prop) {
        double maker_proportion = static_cast<double>(prop) / 100.0;
        double taker_proportion = 1.0 - maker_proportion;
        
        fee_lookup_table_[tier_index][prop] = 
            tier_info.makerFee * maker_proportion + 
            tier_info.takerFee * taker_proportion;
    }
}

FeeCalculator::FeeBreakdown FeeCalculator::calculate_fees_detailed(
    double quantity,
    double price,
    int fee_tier,
    double maker_proportion) {
    
    // Validate inputs
    if (feeTiers_.find(fee_tier) == feeTiers_.end()) {
        logger_.warning("Invalid fee tier: {}, using tier 1", fee_tier);
        fee_tier = 1;
    }
    
    maker_proportion = std::max(0.0, std::min(1.0, maker_proportion));
    double total_value = quantity * price;
    const auto& tier_info = feeTiers_.at(fee_tier);
    
    double maker_fees = total_value * maker_proportion * tier_info.makerFee;
    double taker_fees = total_value * (1.0 - maker_proportion) * tier_info.takerFee;
    double total_fees = maker_fees + taker_fees;
    double effective_rate = total_fees / total_value;
    
    return {maker_fees, taker_fees, total_fees, effective_rate};
}

FeeCalculator::FeeTier FeeCalculator::getFeeTier(int tier) const {
    try {
        if (feeTiers_.find(tier) == feeTiers_.end()) {
            logger_.warning("Invalid fee tier: {}, using tier 1", tier);
            tier = 1;
        }
        
        return feeTiers_.at(tier);
    } catch (const std::exception& e) {
        logger_.error("Failed to get fee tier: {}", e.what());
        return {0.0010, 0.0020}; // Default to higher fees on error
    }
}

void FeeCalculator::setFeeTier(int tier, double makerFee, double takerFee) {
    try {
        // Ensure fees are reasonable (allow negative for rebates)
        makerFee = std::max(-0.001, std::min(0.01, makerFee)); // -0.1% to 1%
        takerFee = std::max(0.0, std::min(0.01, takerFee)); // 0% to 1%
        
        // Update fee tier
        feeTiers_[tier] = {makerFee, takerFee};
        
        // Update lookup table for this tier
        update_lookup_table_for_tier(tier);
        
        logger_.info("Fee tier {} updated: maker={}, taker={}", tier, makerFee, takerFee);
    } catch (const std::exception& e) {
        logger_.error("Failed to set fee tier: {}", e.what());
    }
}

int FeeCalculator::determine_fee_tier(double monthly_volume_usd) {
    // Volume thresholds for tier qualification
    static const std::vector<std::pair<double, int>> volume_tiers = {
        {500000000.0, 10}, // $500M+ -> Tier 10
        {100000000.0, 9},  // $100M+ -> Tier 9
        {50000000.0, 8},   // $50M+ -> Tier 8
        {20000000.0, 7},   // $20M+ -> Tier 7
        {5000000.0, 6},    // $5M+ -> Tier 6
        {1000000.0, 5},    // $1M+ -> Tier 5
        {200000.0, 4},     // $200K+ -> Tier 4
        {50000.0, 3},      // $50K+ -> Tier 3
        {10000.0, 2},      // $10K+ -> Tier 2
        {0.0, 1}           // Default -> Tier 1
    };
    
    for (const auto& [threshold, tier] : volume_tiers) {
        if (monthly_volume_usd >= threshold) {
            return tier;
        }
    }
    
    return 1; // Default to tier 1
}

FeeCalculator::FeeOptimization FeeCalculator::suggest_optimization(
    double quantity,
    double price,
    int current_tier,
    double current_maker_proportion,
    double monthly_volume) {
    
    // Current fees
    auto current_fees = calculate_fees_detailed(quantity, price, current_tier, current_maker_proportion);
    
    // Find optimal maker proportion for current tier
    double best_maker_prop = 0.0;
    double lowest_fees = current_fees.total_fees;
    
    for (int prop = 0; prop <= 100; prop += 5) {
        double test_prop = static_cast<double>(prop) / 100.0;
        auto test_fees = calculate_fees_detailed(quantity, price, current_tier, test_prop);
        if (test_fees.total_fees < lowest_fees) {
            lowest_fees = test_fees.total_fees;
            best_maker_prop = test_prop;
        }
    }
    
    // Check if higher tier would be beneficial
    int optimal_tier = determine_fee_tier(monthly_volume);
    if (optimal_tier > current_tier) {
        auto tier_fees = calculate_fees_detailed(quantity, price, optimal_tier, best_maker_prop);
        if (tier_fees.total_fees < lowest_fees) {
            lowest_fees = tier_fees.total_fees;
        }
    }
    
    double savings = current_fees.total_fees - lowest_fees;
    
    return {best_maker_prop, savings, optimal_tier};
}

FeeCalculator::FeeStatistics FeeCalculator::get_fee_statistics(double monthly_volume) {
    int best_tier = determine_fee_tier(monthly_volume);
    if (best_tier > static_cast<int>(TIER_COUNT) || feeTiers_.find(best_tier) == feeTiers_.end()) {
        best_tier = 1;
    }
    
    size_t tier_index = static_cast<size_t>(best_tier - 1);
    
    double min_rate = fee_lookup_table_[tier_index][0];
    double max_rate = fee_lookup_table_[tier_index][0];
    double sum_rate = 0.0;
    
    for (size_t prop = 0; prop < PROPORTION_STEPS; ++prop) {
        double rate = fee_lookup_table_[tier_index][prop];
        min_rate = std::min(min_rate, rate);
        max_rate = std::max(max_rate, rate);
        sum_rate += rate;
    }
    
    return {
        min_rate,
        max_rate,
        sum_rate / static_cast<double>(PROPORTION_STEPS),
        best_tier
    };
}

} // namespace models
} // namespace kubera
