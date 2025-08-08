#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <regex>
#include <iomanip>
#include <sstream>

// Boost includes
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ssl.hpp>

#include "concurrentqueue.h"
#include "core/orderbook/orderbook.h"
#include "logging/logger.h"
#include "simdjson.h"

namespace kubera {
namespace websocket {

/**
 * @class WebSocketClient
 * @brief Binance-optimized WebSocket client with zero-copy processing and direct OrderBook updates
 */
class WebSocketClient {
public:
    /**
     * @brief Constructor
     * @param endpoint The WebSocket endpoint URL (empty = default Binance BTC-USDT)
     * @param logger The logger instance
     * @param coreId The CPU core to pin the thread to
     * @param useMockMode Whether to use mock mode (deprecated - always false)
     */
    WebSocketClient(const std::string& endpoint, logging::Logger& logger, int coreId = 1, bool useMockMode = false);
    
    /**
     * @brief Destructor
     */
    ~WebSocketClient();
    
    /**
     * @brief Connect to the WebSocket endpoint
     * @return True if successful, false otherwise
     */
    bool connect();
    
    /**
     * @brief Disconnect from the WebSocket endpoint
     */
    void disconnect();
    
    /**
     * @brief Process incoming messages (compatibility method)
     */
    void processMessages();
    
    /**
     * @brief Get the next orderbook update (compatibility method)
     * @return The next orderbook update, or nullopt if none available
     */
    std::optional<kubera::orderbook::OrderBookUpdate> getNextUpdate();
    
    /**
     * @brief Set OrderBook for direct updates (HFT optimization)
     * @param orderBook Shared pointer to OrderBook instance
     */
    void setOrderBook(std::shared_ptr<orderbook::OrderBook> orderBook);
    
    /**
     * @brief Check if connected
     * @return True if connected, false otherwise
     */
    bool isConnected() const;
    
    /**
     * @brief Set the reconnection options
     */
    void setReconnectionOptions(int maxRetries, int initialDelayMs, int maxDelayMs, double backoffMultiplier);

private:
    // HFT optimization constants
    static constexpr size_t BUFFER_INITIAL_SIZE = 65536; // 64KB
    static constexpr size_t BUFFER_MAX_SIZE = 1024 * 1024; // 1MB max
    
    // Boost.Beast components
    std::unique_ptr<boost::asio::io_context> ioc_;
    std::unique_ptr<boost::asio::ip::tcp::resolver> resolver_;
    std::unique_ptr<boost::beast::websocket::stream<boost::beast::ssl_stream<boost::asio::ip::tcp::socket>>> ssl_ws_;
    std::unique_ptr<boost::asio::ssl::context> sslCtx_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> workGuard_;
    std::thread ioThread_;
    std::mutex mutex_;
    
    // WebSocket endpoint configuration
    std::string endpoint_;
    std::string host_;
    std::string port_;
    std::string target_;
    std::string symbol_; // BTC-USDT, ETH-USDT, etc.
    
    // Connection state
    std::atomic<bool> connected_{false};
    std::atomic<bool> connecting_{false};
    
    // Reconnection options
    int maxRetries_{10};
    int initialDelayMs_{1000};
    int maxDelayMs_{30000};
    double backoffMultiplier_{1.5};
    int currentRetries_{0};
    
    // CPU core ID for thread pinning
    int coreId_;
    
    // Message queue (for compatibility)
    moodycamel::ConcurrentQueue<kubera::orderbook::OrderBookUpdate> updateQueue_;
    
    // Logger
    logging::Logger& logger_;
    
    // Direct OrderBook integration (HFT optimization)
    std::shared_ptr<orderbook::OrderBook> orderBook_;
    
    // Binance-specific methods
    void configureBinanceDefaults();
    void parseUrl(const std::string& url, std::string& host, std::string& port, std::string& target, std::string& symbol);
    std::string getCurrentTimestamp() const;
    
    // Connection and I/O methods
    void startSslReadLoop();
    void handleMessage(const std::string& message);
    void handleError(const std::string& error);
    void handleConnection();
    void handleDisconnection();
    bool reconnect();
    int calculateReconnectDelay() const;
    void cleanup();
    
    // Binance message parsing
    std::optional<kubera::orderbook::OrderBookUpdate> parseBinanceMessage(const std::string& message);
};

} // namespace websocket
} // namespace kubera
