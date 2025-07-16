#pragma once

#include <atomic>
#include <array>
#include <mutex>
#include <memory>
#include "core/orderbook/orderbook.h"
#include "logging/logger.h"

namespace kubera {
namespace models {

// ✅ UNIFIED: Shared cache structure for all models
struct alignas(128) SharedMarketCache {
    // Core market data
    alignas(64) double midPrice;
    alignas(64) double spread;
    alignas(64) double volatility;
    alignas(64) double imbalance;
    
    // Depth data (for MarketImpactModel compatibility)
    alignas(64) double totalDepth;
    alignas(64) double bidDepth;
    alignas(64) double askDepth;
    alignas(64) double effectiveDepth;
    
    // Level data for HFT (atomic array compatibility)
    alignas(64) std::array<double, 5> askPrices;
    alignas(64) std::array<double, 5> askQuantities;
    alignas(64) std::array<double, 5> bidPrices;
    alignas(64) std::array<double, 5> bidQuantities;
    
    // Version and timing
    alignas(64) uint64_t orderbook_version;
    alignas(64) uint64_t creation_time_ns;
    alignas(64) std::atomic<uint32_t> ref_count;
    
    SharedMarketCache() : midPrice(0.0), spread(0.0), volatility(0.0), imbalance(0.0),
                         totalDepth(0.0), bidDepth(0.0), askDepth(0.0), effectiveDepth(0.0),
                         orderbook_version(0), creation_time_ns(0), ref_count(1) {
        askPrices.fill(0.0);
        askQuantities.fill(0.0);
        bidPrices.fill(0.0);
        bidQuantities.fill(0.0);
    }
};

// Type alias for OrderBook compatibility
using OrderBookSnapshot = orderbook::OrderBook::OrderBookSnapshot;

// ✅ UNIFIED: Shared cache manager with RCU pattern
class SharedCacheManager {
private:
    static constexpr uint64_t CACHE_EXPIRY_NS = 500'000; // 500μs
    static constexpr size_t MAX_DEPTH_LEVELS = 20;
    
    mutable std::atomic<SharedMarketCache*> current_cache_;
    mutable std::atomic<SharedMarketCache*> pending_delete_;
    
    // Depth calculation helper
    double calculate_effective_depth_from_levels(
        const std::vector<orderbook::PriceLevel>& levels
    ) const;
    
public:
    SharedCacheManager();
    ~SharedCacheManager();
    
    // Cache snapshot with RAII
    struct CacheSnapshot {
        SharedMarketCache* data;
        uint64_t version;
        uint64_t time_ns;
        
        CacheSnapshot(SharedMarketCache* d = nullptr);
        ~CacheSnapshot();
        
        // Move semantics
        CacheSnapshot(CacheSnapshot&& other) noexcept;
        CacheSnapshot& operator=(CacheSnapshot&& other) noexcept;
        
        // Delete copy operations
        CacheSnapshot(const CacheSnapshot&) = delete;
        CacheSnapshot& operator=(const CacheSnapshot&) = delete;
        
        bool is_valid(uint64_t ob_version, uint64_t current_time) const;
    };
    
    // Public interface
    CacheSnapshot get_snapshot() const;
    void update_from_orderbook(const orderbook::OrderBook& order_book);
    void update_from_snapshot(const OrderBookSnapshot& snapshot);
    void cleanup_old_cache() const;
    void cleanup_all();
    void invalidate_cache();
};

// ✅ GLOBAL: Single shared instance
extern SharedCacheManager g_shared_cache;

} // namespace models
} // namespace kubera