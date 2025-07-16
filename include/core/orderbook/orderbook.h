//orderbook.h
#pragma once

#include <vector>
#include <array>
#include <unordered_map>
#include <atomic>
#include <string>
#include <chrono>
#include <bitset>
#include "logging/logger.h"
#include "core/utils/memory_pool.h"

namespace kubera {
namespace orderbook {

/**
 * @brief Price level structure - MOVED TO NAMESPACE LEVEL for WebSocket compatibility
 */
struct PriceLevel {
    double price;
    double quantity;
    
    PriceLevel() : price(0.0), quantity(0.0) {}
    PriceLevel(double p, double q) : price(p), quantity(q) {}
};

/**
 * @brief Order book update structure - MOVED TO NAMESPACE LEVEL for WebSocket compatibility
 */
struct OrderBookUpdate {
    std::string timestamp;
    std::string exchange;
    std::string symbol;
    std::vector<PriceLevel> asks;
    std::vector<PriceLevel> bids;
};

/**
 * @brief Update type enum
 */
enum class UpdateType {
    SNAPSHOT,
    DELTA
};

/**
 * @class OrderBook
 * @brief HFT-optimized order book with price-indexed arrays and sub-10μs updates
 */
class OrderBook {
public:
    using PriceLevelPool = kubera::utils::MemoryPool<sizeof(PriceLevel), 2048>;
    /**
     * @brief Constructor
     * @param logger The logger instance
     * @param maxLevels The maximum number of price levels to track
     */
    OrderBook(logging::Logger& logger, size_t maxLevels = 100, PriceLevelPool* priceLevelPool = nullptr);
    
    bool update(const OrderBookUpdate& update, UpdateType type = UpdateType::SNAPSHOT);
    double getMidPrice() const;
    double getSpread() const;
    double getDepth(size_t levels) const;
    double getImbalance() const;
    double getVolatility() const;
    
    uint64_t getSequenceNumber() const {
        return sequenceNumber_.load(std::memory_order_acquire);
    }
    
    std::vector<PriceLevel> getAsks() const;
    std::vector<PriceLevel> getBids() const;
    
    /**
     * @brief Atomic snapshot for consistent reads
     */
    struct OrderBookSnapshot {
        std::vector<PriceLevel> asks;
        std::vector<PriceLevel> bids;
        double midPrice;
        double spread;
        double imbalance;
        double volatility;
        uint64_t sequenceNumber;
        std::string timestamp;
        std::string exchange;
        std::string symbol;
    };
    
    OrderBookSnapshot getSnapshot() const;

private:
    // Price indexing configuration
    static constexpr double DEFAULT_TICK_SIZE = 0.01;
    static constexpr size_t PRICE_LEVELS = 10000;  // Covers wide price range
    static constexpr double BASE_PRICE = 1000.0;   // Starting price for indexing
    
    // Cache-aligned price level structure for internal use
    struct alignas(64) CachePriceLevel {
        double price;
        double quantity;
        std::atomic<uint64_t> updateCount;
        
        CachePriceLevel() : price(0.0), quantity(0.0), updateCount(0) {}
        
        // Delete copy constructor and assignment operator
        CachePriceLevel(const CachePriceLevel&) = delete;
        CachePriceLevel& operator=(const CachePriceLevel&) = delete;
        
        // Add move constructor and assignment operator
        CachePriceLevel(CachePriceLevel&& other) noexcept
            : price(other.price), quantity(other.quantity) {
            updateCount.store(other.updateCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        
        CachePriceLevel& operator=(CachePriceLevel&& other) noexcept {
            if (this != &other) {
                price = other.price;
                quantity = other.quantity;
                updateCount.store(other.updateCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            return *this;
        }
    };
    
    // Lock-free synchronization using atomic operations only
    mutable std::atomic<bool> updating_{false};
    
    // Configuration
    size_t maxLevels_;
    double tickSize_;
    double basePrice_;
    
    // Price-indexed arrays for O(1) access
    std::array<CachePriceLevel, PRICE_LEVELS> askLevels_;
    std::array<CachePriceLevel, PRICE_LEVELS> bidLevels_;
    
    // Bitsets to track which price levels are active (cache-friendly)
    std::bitset<PRICE_LEVELS> askActiveLevels_;
    std::bitset<PRICE_LEVELS> bidActiveLevels_;
    
    // Sorted indices for best bid/ask tracking (maintained automatically)
    mutable std::atomic<size_t> bestAskIndex_{SIZE_MAX};
    mutable std::atomic<size_t> bestBidIndex_{SIZE_MAX};
    mutable std::atomic<bool> indicesValid_{false};
    
    // Fallback hash maps for prices outside indexed range
    std::unordered_map<double, CachePriceLevel> askOverflow_;
    std::unordered_map<double, CachePriceLevel> bidOverflow_;
    
    // Sequence number for synchronization
    std::atomic<uint64_t> sequenceNumber_{0};
    
    // Recent mid prices for volatility calculation
    static constexpr size_t VOLATILITY_WINDOW = 100;
    std::array<std::atomic<double>, VOLATILITY_WINDOW> recentMidPrices_;
    std::atomic<size_t> midPriceIndex_{0};
    
    // Atomic cached statistics (lock-free access)
    mutable std::atomic<double> cachedMidPrice_{0.0};
    mutable std::atomic<double> cachedSpread_{0.0};
    mutable std::atomic<double> cachedImbalance_{0.0};
    mutable std::atomic<double> cachedVolatility_{0.0};
    
    // Logger
    logging::Logger& logger_;
    PriceLevelPool* priceLevelPool_ = nullptr;
    
    // Price indexing helpers
    inline size_t priceToIndex(double price) const {
        if (price < basePrice_) return SIZE_MAX;  // Out of range
        size_t index = static_cast<size_t>((price - basePrice_) / tickSize_);
        return (index < PRICE_LEVELS) ? index : SIZE_MAX;
    }
    
    inline double indexToPrice(size_t index) const {
        return basePrice_ + (index * tickSize_);
    }
    
    inline bool isValidIndex(size_t index) const {
        return index != SIZE_MAX && index < PRICE_LEVELS;
    }
    
    // Lock-free price finding and statistics calculation
    void updateBestPricesAtomic() const;
    
    // Lock-free atomic methods for direct access
    double getMidPriceAtomic() const;
    double getSpreadAtomic() const;
    double getImbalanceAtomic() const;
    double getVolatilityAtomic() const;
    std::vector<PriceLevel> getAsksAtomic() const;
    std::vector<PriceLevel> getBidsAtomic() const;
    void updateStatisticsAtomic();
    
    // Lock-free update methods
    void updateLevelsAtomic(
        const std::vector<PriceLevel>& levels,
        std::array<CachePriceLevel, PRICE_LEVELS>& bookLevels,
        std::bitset<PRICE_LEVELS>& activeLevels,
        std::unordered_map<double, CachePriceLevel>& overflow,
        bool isAsk,
        UpdateType type
    );
    
    void updateVolatilityAtomic(double midPrice);
    
    // Configuration helpers
    void autoDetectTickSize(const std::vector<PriceLevel>& levels);
    void adjustBasePrice(double newBasePrice);
    
    PriceLevel* allocatePriceLevel(double price, double quantity) const;
    void clearPriceLevelPool() const;
};

class UserInterface {
    // ...existing code...
    orderbook::OrderBook::PriceLevelPool priceLevelPool_;
    // ...existing code...
};

} // namespace orderbook
} // namespace kubera