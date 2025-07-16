#include "core/models/shared_cache.h"
#include <chrono>
#include <algorithm>

namespace kubera {
namespace models {

// ✅ Global shared cache instance
SharedCacheManager g_shared_cache;

SharedCacheManager::SharedCacheManager() {
    current_cache_.store(nullptr, std::memory_order_relaxed);
    pending_delete_.store(nullptr, std::memory_order_relaxed);
}

SharedCacheManager::~SharedCacheManager() {
    cleanup_all();
}

// ✅ CacheSnapshot implementation with RAII
SharedCacheManager::CacheSnapshot::CacheSnapshot(SharedMarketCache* d) : data(d) {
    if (data) {
        version = data->orderbook_version;
        time_ns = data->creation_time_ns;
        data->ref_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        version = 0;
        time_ns = 0;
    }
}

SharedCacheManager::CacheSnapshot::~CacheSnapshot() {
    if (data && data->ref_count.fetch_sub(1, std::memory_order_relaxed) == 1) {
        delete data;
    }
}

SharedCacheManager::CacheSnapshot::CacheSnapshot(CacheSnapshot&& other) noexcept
    : data(other.data), version(other.version), time_ns(other.time_ns) {
    other.data = nullptr;
}

SharedCacheManager::CacheSnapshot& SharedCacheManager::CacheSnapshot::operator=(CacheSnapshot&& other) noexcept {
    if (this != &other) {
        if (data && data->ref_count.fetch_sub(1, std::memory_order_relaxed) == 1) {
            delete data;
        }
        
        data = other.data;
        version = other.version;
        time_ns = other.time_ns;
        other.data = nullptr;
    }
    
    return *this;
}

bool SharedCacheManager::CacheSnapshot::is_valid(uint64_t ob_version, uint64_t current_time) const {
    return data && version == ob_version && 
           (current_time - time_ns) < CACHE_EXPIRY_NS;
}

// ✅ Main cache management methods
SharedCacheManager::CacheSnapshot SharedCacheManager::get_snapshot() const {
    SharedMarketCache* cache = current_cache_.load(std::memory_order_acquire);
    return CacheSnapshot(cache);
}

void SharedCacheManager::update_from_orderbook(const orderbook::OrderBook& order_book) {
    auto snapshot = order_book.getSnapshot();
    update_from_snapshot(snapshot);
}

void SharedCacheManager::update_from_snapshot(const kubera::orderbook::OrderBookSnapshot& snapshot) {
    try {
        // Create new cache data from consistent snapshot
        SharedMarketCache* new_cache = new SharedMarketCache();
        
        new_cache->midPrice = snapshot.midPrice;
        new_cache->spread = snapshot.spread;
        new_cache->volatility = snapshot.volatility;
        new_cache->imbalance = snapshot.imbalance;
        
        // Calculate depths efficiently
        new_cache->bidDepth = calculate_effective_depth_from_levels(snapshot.bids);
        new_cache->askDepth = calculate_effective_depth_from_levels(snapshot.asks);
        new_cache->totalDepth = new_cache->bidDepth + new_cache->askDepth;
        new_cache->effectiveDepth = new_cache->totalDepth / 2.0;
        
        // Fill level arrays for HFT compatibility
        for (size_t i = 0; i < 5 && i < snapshot.asks.size(); ++i) {
            new_cache->askPrices[i] = snapshot.asks[i].price;
            new_cache->askQuantities[i] = snapshot.asks[i].quantity;
        }
        
        for (size_t i = 0; i < 5 && i < snapshot.bids.size(); ++i) {
            new_cache->bidPrices[i] = snapshot.bids[i].price;
            new_cache->bidQuantities[i] = snapshot.bids[i].quantity;
        }
        
        new_cache->orderbook_version = snapshot.sequenceNumber;
        new_cache->creation_time_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        
        // Atomic swap with release semantics
        SharedMarketCache* old_current = current_cache_.exchange(new_cache, std::memory_order_release);
        
        // Store old cache for cleanup
        SharedMarketCache* previous_pending = pending_delete_.exchange(old_current, std::memory_order_relaxed);
        if (previous_pending && previous_pending->ref_count.fetch_sub(1, std::memory_order_relaxed) == 1) {
            delete previous_pending;
        }
        
    } catch (const std::exception& e) {
        // Handle error appropriately - could log error
    }
}

// ✅ Optimized depth calculation
double SharedCacheManager::calculate_effective_depth_from_levels(
    const std::vector<kubera::orderbook::PriceLevel>& levels) const {
    
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

// ✅ Cache cleanup methods
void SharedCacheManager::cleanup_old_cache() {
    SharedMarketCache* pending = pending_delete_.exchange(nullptr, std::memory_order_relaxed);
    if (pending && pending->ref_count.fetch_sub(1, std::memory_order_relaxed) == 1) {
        delete pending;
    }
}

void SharedCacheManager::cleanup_all() {
    SharedMarketCache* current = current_cache_.load(std::memory_order_acquire);
    SharedMarketCache* pending = pending_delete_.load(std::memory_order_acquire);
    
    if (current && current->ref_count.fetch_sub(1, std::memory_order_relaxed) == 1) {
        delete current;
    }
    
    if (pending && pending->ref_count.fetch_sub(1, std::memory_order_relaxed) == 1) {
        delete pending;
    }
}

void SharedCacheManager::invalidate_cache() {
    SharedMarketCache* current = current_cache_.exchange(nullptr, std::memory_order_relaxed);
    if (current && current->ref_count.fetch_sub(1, std::memory_order_relaxed) == 1) {
        delete current;
    }
}

// ✅ Utility methods
bool SharedCacheManager::is_cache_valid(uint64_t ob_version) const {
    auto snapshot = get_snapshot();
    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    return snapshot.is_valid(ob_version, now_ns);
}

void SharedCacheManager::force_update(const orderbook::OrderBook& order_book) {
    update_from_orderbook(order_book);
}

SharedCacheManager::CacheStatistics SharedCacheManager::get_statistics() const {
    CacheStatistics stats{};
    
    auto snapshot = get_snapshot();
    if (snapshot.data) {
        auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        stats.age_ns = now_ns - snapshot.time_ns;
        stats.ref_count = snapshot.data->ref_count.load();
        stats.version = snapshot.version;
        stats.is_valid = snapshot.is_valid(snapshot.version, now_ns);
    }
    
    return stats;
}

} // namespace models
} // namespace kubera
