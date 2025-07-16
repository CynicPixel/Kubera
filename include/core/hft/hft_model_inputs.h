#pragma once

#include <array>
#include <atomic>
#include <memory>
#include "core/orderbook/orderbook.h"

namespace kubera {
namespace hft {

/**
 * @brief Lock-free model inputs structure for HFT performance
 * Replaces OrderBookSnapshot with direct atomic access
 */
struct alignas(64) HFTModelInputs {
    // Core price data (lock-free atomic updates)
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
     * @param ob OrderBook reference with latest data
     */
    void updateFromOrderBook(const orderbook::OrderBook& ob) {
        // Direct atomic updates - no locks, no copies
        mid_price.store(ob.getMidPrice(), std::memory_order_release);
        spread.store(ob.getSpread(), std::memory_order_release);
        volatility.store(ob.getVolatility(), std::memory_order_release);
        imbalance.store(ob.getImbalance(), std::memory_order_release);
        sequence_number.store(ob.getSequenceNumber(), std::memory_order_release);

        // Get orderbook levels for top 5
        auto asks = ob.getAsks();
        auto bids = ob.getBids();

        // Update top bid/ask prices and quantities
        if (!asks.empty()) {
            top_ask_price.store(asks[0].price, std::memory_order_release);
            top_ask_qty.store(asks[0].quantity, std::memory_order_release);
        }
        if (!bids.empty()) {
            top_bid_price.store(bids[0].price, std::memory_order_release);
            top_bid_qty.store(bids[0].quantity, std::memory_order_release);
        }

        // Update top 5 levels directly
        for (size_t i = 0; i < std::min(asks.size(), ask_prices.size()); ++i) {
            ask_prices[i].store(asks[i].price, std::memory_order_release);
            ask_quantities[i].store(asks[i].quantity, std::memory_order_release);
        }

        for (size_t i = 0; i < std::min(bids.size(), bid_prices.size()); ++i) {
            bid_prices[i].store(bids[i].price, std::memory_order_release);
            bid_quantities[i].store(bids[i].quantity, std::memory_order_release);
        }
    }

    /**
     * @brief Get current sequence number for cache validation
     */
    uint64_t getCurrentSequence() const {
        return sequence_number.load(std::memory_order_acquire);
    }

    /**
     * @brief Get mid price atomically
     */
    double getMidPrice() const {
        return mid_price.load(std::memory_order_acquire);
    }

    /**
     * @brief Get spread atomically  
     */
    double getSpread() const {
        return spread.load(std::memory_order_acquire);
    }
};

} // namespace hft
} // namespace kubera
