//orderbook.cpp
#include "core/orderbook/orderbook.h"
#include "core/utils/memory_pool.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace kubera {
namespace orderbook {

OrderBook::OrderBook(logging::Logger& logger, size_t maxLevels, PriceLevelPool* priceLevelPool)
    : maxLevels_(maxLevels), logger_(logger), tickSize_(DEFAULT_TICK_SIZE), 
      basePrice_(BASE_PRICE), priceLevelPool_(priceLevelPool) {
    
    // Initialize price-indexed arrays (zero-initialized by default)
    askActiveLevels_.reset();
    bidActiveLevels_.reset();
    
    // Initialize atomic recent mid prices to zero
    for (auto& price : recentMidPrices_) {
        price.store(0.0, std::memory_order_relaxed);
    }
    
    // Initialize atomic best price indices
    bestAskIndex_.store(SIZE_MAX, std::memory_order_relaxed);
    bestBidIndex_.store(SIZE_MAX, std::memory_order_relaxed);
    indicesValid_.store(false, std::memory_order_relaxed);
    
    logger_.info("Lock-free OrderBook initialized with {} levels, tick size: {}, base price: {}", 
                maxLevels_, tickSize_, basePrice_);
}

bool OrderBook::update(const OrderBookUpdate& update, UpdateType type) {
    // Use atomic flag for lock-free update synchronization
    bool expected = false;
    if (!updating_.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        // Another thread is updating, skip this update to maintain lock-free nature
        return false;
    }
    
    try {
        // Auto-detect tick size and adjust base price on first snapshot
        if (type == UpdateType::SNAPSHOT && !update.asks.empty()) {
            autoDetectTickSize(update.asks);
            if (!update.asks.empty()) {
                double minPrice = std::min(update.asks.front().price, 
                                         !update.bids.empty() ? update.bids.front().price : update.asks.front().price);
                double maxPrice = std::max(update.asks.back().price, 
                                         !update.bids.empty() ? update.bids.back().price : update.asks.back().price);
                
                // Check if prices are outside the current valid range
                double currentMaxPrice = basePrice_ + (PRICE_LEVELS * tickSize_);
                if (minPrice < basePrice_ || maxPrice > currentMaxPrice) {
                    // Adjust base price to center the price range
                    double centerPrice = (minPrice + maxPrice) / 2.0;
                    double newBasePrice = centerPrice - (PRICE_LEVELS * tickSize_ / 2.0);
                    adjustBasePrice(newBasePrice);
                }
            }
        }
        
        // Update asks and bids using lock-free atomic methods
        updateLevelsAtomic(update.asks, askLevels_, askActiveLevels_, askOverflow_, true, type);
        updateLevelsAtomic(update.bids, bidLevels_, bidActiveLevels_, bidOverflow_, false, type);
        
        // Mark best price indices as invalid (will be recalculated on demand)
        indicesValid_.store(false, std::memory_order_relaxed);
        
        // Update cached statistics atomically
        updateStatisticsAtomic();
        
        // Increment sequence number
        sequenceNumber_.fetch_add(1, std::memory_order_release);
        
        // Release the update lock
        updating_.store(false, std::memory_order_release);
        
        return true;
    } catch (const std::exception& e) {
        logger_.error("OrderBook update failed: {}", e.what());
        // Release the update lock even on error
        updating_.store(false, std::memory_order_release);
        return false;
    }
}

double OrderBook::getMidPrice() const {
    return cachedMidPrice_.load(std::memory_order_acquire);
}

double OrderBook::getSpread() const {
    return cachedSpread_.load(std::memory_order_acquire);
}

double OrderBook::getDepth(size_t levels) const {
    // Lock-free depth calculation using atomic operations
    double askDepth = 0.0;
    double bidDepth = 0.0;
    
    // Fast depth calculation using atomic indices
    updateBestPricesAtomic();
    
    size_t startAskIndex = bestAskIndex_.load(std::memory_order_acquire);
    size_t startBidIndex = bestBidIndex_.load(std::memory_order_acquire);
    
    // Calculate ask depth
    size_t askCount = 0;
    if (isValidIndex(startAskIndex)) {
        for (size_t i = startAskIndex; i < PRICE_LEVELS && askCount < levels; ++i) {
            if (askActiveLevels_[i]) {
                askDepth += askLevels_[i].quantity;
                askCount++;
            }
        }
    }
    
    // Add overflow asks
    for (const auto& [price, level] : askOverflow_) {
        if (askCount >= levels) break;
        if (level.quantity > 0) {
            askDepth += level.quantity;
            askCount++;
        }
    }
    
    // Calculate bid depth (iterate backwards from best bid)
    size_t bidCount = 0;
    if (isValidIndex(startBidIndex)) {
        for (size_t i = startBidIndex; bidCount < levels && i < PRICE_LEVELS; --i) {
            if (bidActiveLevels_[i]) {
                bidDepth += bidLevels_[i].quantity;
                bidCount++;
            }
            if (i == 0) break;  // Prevent underflow
        }
    }
    
    // Add overflow bids
    for (const auto& [price, level] : bidOverflow_) {
        if (bidCount >= levels) break;
        if (level.quantity > 0) {
            bidDepth += level.quantity;
            bidCount++;
        }
    }
    
    return askDepth + bidDepth;
}

double OrderBook::getImbalance() const {
    return cachedImbalance_.load(std::memory_order_acquire);
}

double OrderBook::getVolatility() const {
    return cachedVolatility_.load(std::memory_order_acquire);
}

std::vector<PriceLevel> OrderBook::getAsks() const {
    return getAsksAtomic();
}

std::vector<PriceLevel> OrderBook::getBids() const {
    return getBidsAtomic();
}

kubera::orderbook::OrderBookSnapshot OrderBook::getSnapshot() const {
    // Lock-free snapshot using atomic operations
    kubera::orderbook::OrderBookSnapshot snapshot;
    snapshot.asks = getAsksAtomic();
    snapshot.bids = getBidsAtomic();
    snapshot.midPrice = cachedMidPrice_.load(std::memory_order_acquire);
    snapshot.spread = cachedSpread_.load(std::memory_order_acquire);
    snapshot.imbalance = cachedImbalance_.load(std::memory_order_acquire);
    snapshot.volatility = cachedVolatility_.load(std::memory_order_acquire);
    snapshot.sequenceNumber = sequenceNumber_.load(std::memory_order_acquire);
    snapshot.timestamp = std::to_string(std::time(nullptr));
    snapshot.exchange = "OKX";
    snapshot.symbol = "BTC-USDT-SWAP";
    return snapshot;
}

// Private atomic methods

void OrderBook::updateBestPricesAtomic() const {
    if (indicesValid_.load(std::memory_order_acquire)) return;
    
    // Find best ask (lowest price with quantity > 0)
    size_t newBestAskIndex = SIZE_MAX;
    for (size_t i = 0; i < PRICE_LEVELS; ++i) {
        if (askActiveLevels_[i] && askLevels_[i].quantity > 0) {
            newBestAskIndex = i;
            break;
        }
    }
    
    // Find best bid (highest price with quantity > 0)
    size_t newBestBidIndex = SIZE_MAX;
    for (size_t i = PRICE_LEVELS - 1; i != SIZE_MAX; --i) {
        if (bidActiveLevels_[i] && bidLevels_[i].quantity > 0) {
            newBestBidIndex = i;
            break;
        }
    }
    
    // Update atomic indices
    bestAskIndex_.store(newBestAskIndex, std::memory_order_release);
    bestBidIndex_.store(newBestBidIndex, std::memory_order_release);
    indicesValid_.store(true, std::memory_order_release);
}

double OrderBook::getMidPriceAtomic() const {
    updateBestPricesAtomic();
    
    double bestAsk = 0.0;
    double bestBid = 0.0;
    
    // Get best ask price
    size_t askIndex = bestAskIndex_.load(std::memory_order_acquire);
    if (isValidIndex(askIndex)) {
        bestAsk = indexToPrice(askIndex);
    } else if (!askOverflow_.empty()) {
        // Find minimum price in overflow
        bestAsk = std::min_element(askOverflow_.begin(), askOverflow_.end(),
            [](const auto& a, const auto& b) { 
                return a.first < b.first && a.second.quantity > 0; 
            })->first;
    }
    
    // Get best bid price
    size_t bidIndex = bestBidIndex_.load(std::memory_order_acquire);
    if (isValidIndex(bidIndex)) {
        bestBid = indexToPrice(bidIndex);
    } else if (!bidOverflow_.empty()) {
        // Find maximum price in overflow
        bestBid = std::max_element(bidOverflow_.begin(), bidOverflow_.end(),
            [](const auto& a, const auto& b) { 
                return a.first < b.first && a.second.quantity > 0; 
            })->first;
    }
    
    if (bestAsk > 0 && bestBid > 0) {
        return (bestAsk + bestBid) / 2.0;
    }
    return 0.0;
}

double OrderBook::getSpreadAtomic() const {
    updateBestPricesAtomic();
    
    double bestAsk = 0.0;
    double bestBid = 0.0;
    
    size_t askIndex = bestAskIndex_.load(std::memory_order_acquire);
    size_t bidIndex = bestBidIndex_.load(std::memory_order_acquire);
    
    if (isValidIndex(askIndex)) {
        bestAsk = indexToPrice(askIndex);
    }
    if (isValidIndex(bidIndex)) {
        bestBid = indexToPrice(bidIndex);
    }
    
    if (bestAsk > 0 && bestBid > 0) {
        return bestAsk - bestBid;
    }
    return 0.0;
}

double OrderBook::getImbalanceAtomic() const {
    updateBestPricesAtomic();
    
    double askDepth = 0.0;
    double bidDepth = 0.0;
    
    size_t askIndex = bestAskIndex_.load(std::memory_order_acquire);
    size_t bidIndex = bestBidIndex_.load(std::memory_order_acquire);
    
    // Calculate depths for top 5 levels using fast bitset iteration
    size_t askCount = 0;
    if (isValidIndex(askIndex)) {
        for (size_t i = askIndex; i < PRICE_LEVELS && askCount < 5; ++i) {
            if (askActiveLevels_[i]) {
                askDepth += askLevels_[i].quantity;
                askCount++;
            }
        }
    }
    
    size_t bidCount = 0;
    if (isValidIndex(bidIndex)) {
        for (size_t i = bidIndex; bidCount < 5 && i < PRICE_LEVELS; --i) {
            if (bidActiveLevels_[i]) {
                bidDepth += bidLevels_[i].quantity;
                bidCount++;
            }
            if (i == 0) break;
        }
    }
    
    double totalDepth = askDepth + bidDepth;
    if (totalDepth <= 0) {
        return 0.0;
    }
    
    return (bidDepth - askDepth) / totalDepth;
}

double OrderBook::getVolatilityAtomic() const {
    // Check if we have valid price data
    if (recentMidPrices_[0].load(std::memory_order_acquire) <= 0) {
        return 0.0;
    }
    
    // Calculate sum using atomic loads
    double sum = 0.0;
    for (const auto& price : recentMidPrices_) {
        sum += price.load(std::memory_order_acquire);
    }
    double mean = sum / VOLATILITY_WINDOW;
    
    // Calculate variance using atomic loads
    double variance = 0.0;
    for (const auto& price : recentMidPrices_) {
        double priceValue = price.load(std::memory_order_acquire);
        if (priceValue > 0) {
            double diff = priceValue - mean;
            variance += diff * diff;
        }
    }
    variance /= VOLATILITY_WINDOW;
    
    // Normalize volatility by mean price (returns percent stddev)
    if (mean > 0.0) {
        return std::sqrt(variance) / mean;
    } else {
        return 0.0;
    }
}

std::vector<PriceLevel> OrderBook::getAsksAtomic() const {
    std::vector<PriceLevel> result;
    result.reserve(maxLevels_);
    if (priceLevelPool_) clearPriceLevelPool();
    for (size_t i = 0; i < PRICE_LEVELS && result.size() < maxLevels_; ++i) {
        if (askActiveLevels_[i] && askLevels_[i].quantity > 0) {
            if (priceLevelPool_) {
                PriceLevel* pl = allocatePriceLevel(indexToPrice(i), askLevels_[i].quantity);
                if (pl) result.emplace_back(*pl);
            } else {
                result.emplace_back(indexToPrice(i), askLevels_[i].quantity);
            }
        }
    }
    for (const auto& [price, level] : askOverflow_) {
        if (level.quantity > 0 && result.size() < maxLevels_) {
            if (priceLevelPool_) {
                PriceLevel* pl = allocatePriceLevel(price, level.quantity);
                if (pl) result.emplace_back(*pl);
            } else {
                result.emplace_back(price, level.quantity);
            }
        }
    }
    if (!askOverflow_.empty()) {
        std::sort(result.begin(), result.end(), 
            [](const PriceLevel& a, const PriceLevel& b) {
                return a.price < b.price;
            });
    }
    return result;
}

std::vector<PriceLevel> OrderBook::getBidsAtomic() const {
    std::vector<PriceLevel> result;
    result.reserve(maxLevels_);
    if (priceLevelPool_) clearPriceLevelPool();
    for (size_t i = PRICE_LEVELS - 1; i != SIZE_MAX && result.size() < maxLevels_; --i) {
        if (bidActiveLevels_[i] && bidLevels_[i].quantity > 0) {
            if (priceLevelPool_) {
                PriceLevel* pl = allocatePriceLevel(indexToPrice(i), bidLevels_[i].quantity);
                if (pl) result.emplace_back(*pl);
            } else {
                result.emplace_back(indexToPrice(i), bidLevels_[i].quantity);
            }
        }
    }
    for (const auto& [price, level] : bidOverflow_) {
        if (level.quantity > 0 && result.size() < maxLevels_) {
            if (priceLevelPool_) {
                PriceLevel* pl = allocatePriceLevel(price, level.quantity);
                if (pl) result.emplace_back(*pl);
            } else {
                result.emplace_back(price, level.quantity);
            }
        }
    }
    if (!bidOverflow_.empty()) {
        std::sort(result.begin(), result.end(), 
            [](const PriceLevel& a, const PriceLevel& b) {
                return a.price > b.price;
            });
    }
    return result;
}

PriceLevel* OrderBook::allocatePriceLevel(double price, double quantity) const {
    if (!priceLevelPool_) return nullptr;
    void* mem = priceLevelPool_->allocate();
    if (!mem) return nullptr;
    PriceLevel* pl = new (mem) PriceLevel(price, quantity);
    return pl;
}

void OrderBook::clearPriceLevelPool() const {
    // No-op: In a real implementation, you might want to track and deallocate objects
}

void OrderBook::updateStatisticsAtomic() {
    // Calculate all statistics atomically
    double midPrice = getMidPriceAtomic();
    double spread = getSpreadAtomic();
    double imbalance = getImbalanceAtomic();
    
    // Update volatility with new mid price
    updateVolatilityAtomic(midPrice);
    double volatility = getVolatilityAtomic();
    
    // Store all values atomically
    cachedMidPrice_.store(midPrice, std::memory_order_release);
    cachedSpread_.store(spread, std::memory_order_release);
    cachedImbalance_.store(imbalance, std::memory_order_release);
    cachedVolatility_.store(volatility, std::memory_order_release);
}

void OrderBook::updateLevelsAtomic(
    const std::vector<PriceLevel>& levels,
    std::array<CachePriceLevel, PRICE_LEVELS>& bookLevels,
    std::bitset<PRICE_LEVELS>& activeLevels,
    std::unordered_map<double, CachePriceLevel>& overflow,
    bool isAsk,
    UpdateType type
) {
    // Clear existing levels if this is a snapshot
    if (type == UpdateType::SNAPSHOT) {
        activeLevels.reset();
        for (auto& level : bookLevels) {
            level.price = 0.0;
            level.quantity = 0.0;
            level.updateCount.store(0, std::memory_order_relaxed);
        }
        overflow.clear();
    }
    
    // Process each level from the update
    for (const auto& level : levels) {
        if (level.price <= 0) continue;
        
        size_t index = priceToIndex(level.price);
        
        if (isValidIndex(index)) {
            // Price fits in indexed array - O(1) access
            if (level.quantity > 0) {
                bookLevels[index].price = level.price;
                bookLevels[index].quantity = level.quantity;
                bookLevels[index].updateCount.fetch_add(1, std::memory_order_relaxed);
                activeLevels.set(index);
            } else {
                // Remove level
                bookLevels[index].price = 0.0;
                bookLevels[index].quantity = 0.0;
                activeLevels.reset(index);
            }
        } else {
            // Price outside indexed range - use overflow map
            if (level.quantity > 0) {
                overflow[level.price] = CachePriceLevel();
                overflow[level.price].price = level.price;
                overflow[level.price].quantity = level.quantity;
                overflow[level.price].updateCount.fetch_add(1, std::memory_order_relaxed);
            } else {
                overflow.erase(level.price);
            }
        }
    }
}

void OrderBook::updateVolatilityAtomic(double midPrice) {
    if (midPrice > 0) {
        size_t currentIndex = midPriceIndex_.load(std::memory_order_acquire);
        recentMidPrices_[currentIndex].store(midPrice, std::memory_order_release);
        
        // Atomically update the index
        size_t nextIndex = (currentIndex + 1) % VOLATILITY_WINDOW;
        midPriceIndex_.store(nextIndex, std::memory_order_release);
    }
}

void OrderBook::autoDetectTickSize(const std::vector<PriceLevel>& levels) {
    if (levels.size() < 2) return;
    
    // Find minimum price difference to determine tick size
    double minDiff = std::numeric_limits<double>::max();
    for (size_t i = 1; i < levels.size(); ++i) {
        double diff = std::abs(levels[i].price - levels[i-1].price);
        if (diff > 0 && diff < minDiff) {
            minDiff = diff;
        }
    }
    
    if (minDiff < std::numeric_limits<double>::max() && minDiff != tickSize_) {
        tickSize_ = minDiff;
        logger_.info("Auto-detected tick size: {}", tickSize_);
    }
}

void OrderBook::adjustBasePrice(double newBasePrice) {
    if (std::abs(newBasePrice - basePrice_) > tickSize_) {
        basePrice_ = newBasePrice;
        logger_.info("Adjusted base price to: {}", basePrice_);
        indicesValid_ = false;
    }
}

} // namespace orderbook
} // namespace kubera