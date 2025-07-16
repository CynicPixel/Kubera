#pragma once

#include <atomic>
#include <array>
#include "core/orderbook/orderbook.h"

namespace kubera {
namespace models {

// ✅ Shared market data cache structure
struct SharedMarketCache {
    std::atomic<uint32_t> ref_count{1};
    
    // Core market data
    double midPrice;
    double spread;
    double volatility;
    double imbalance;
    double bidDepth;
    double askDepth;
    double totalDepth;
    double effectiveDepth;
    
    // HFT-compatible level arrays
    std::array<double, 5> askPrices{};
    std::array<double, 5> askQuantities{};
    std::array<double, 5> bidPrices{};
    std::array<double, 5> bidQuantities{};
    
    // Cache metadata
    uint64_t orderbook_version;
    uint64_t creation_time_ns;
};

class SharedCacheManager {
public:
    static constexpr uint64_t CACHE_EXPIRY_NS = 100000000; // 100ms
    static constexpr size_t MAX_DEPTH_LEVELS = 10;

    // ✅ RAII cache snapshot
    class CacheSnapshot {
    public:
        SharedMarketCache* data;
        uint64_t version;
        uint64_t time_ns;
        
        explicit CacheSnapshot(SharedMarketCache* d = nullptr);
        ~CacheSnapshot();
        
        // Move semantics only
        CacheSnapshot(const CacheSnapshot&) = delete;
        CacheSnapshot& operator=(const CacheSnapshot&) = delete;
        CacheSnapshot(CacheSnapshot&& other) noexcept;
        CacheSnapshot& operator=(CacheSnapshot&& other) noexcept;
        
        bool is_valid(uint64_t ob_version, uint64_t current_time) const;
    };

    struct CacheStatistics {
        uint64_t age_ns;
        uint32_t ref_count;
        uint64_t version;
        bool is_valid;
    };

private:
    std::atomic<SharedMarketCache*> current_cache_;
    std::atomic<SharedMarketCache*> pending_delete_;
    
    double calculate_effective_depth_from_levels(const std::vector<orderbook::PriceLevel>& levels) const;

public:
    SharedCacheManager();
    ~SharedCacheManager();
    
    // Cache access
    CacheSnapshot get_snapshot() const;
    
    // Cache updates
    void update_from_orderbook(const orderbook::OrderBook& order_book);
    void update_from_snapshot(const kubera::orderbook::OrderBookSnapshot& snapshot);
    
    // Cache management
    void cleanup_old_cache();
    void cleanup_all();
    void invalidate_cache();
    
    // Utility methods
    bool is_cache_valid(uint64_t ob_version) const;
    void force_update(const orderbook::OrderBook& order_book);
    CacheStatistics get_statistics() const;
};

// ✅ Global shared cache instance
extern SharedCacheManager g_shared_cache;

} // namespace models
} // namespace kubera
