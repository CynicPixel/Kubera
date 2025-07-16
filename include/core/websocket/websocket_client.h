////websocketclient.h
#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <memory>
#include <vector>
#include <optional>
#include <atomic>
#include <thread>
#include <mutex>

// Boost includes
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl/context.hpp>

#include "concurrentqueue.h"
#include "core/orderbook/orderbook.h"
#include "logging/logger.h"
#include "simdjson.h"

namespace kubera {
namespace websocket {

/**
 * @class WebSocketClient
 * @brief HFT-optimized WebSocket client with zero-copy processing and direct OrderBook updates
 */
class WebSocketClient {
public:
    /**
     * @brief Constructor
     * @param endpoint The WebSocket endpoint URL
     * @param logger The logger instance
     * @param coreId The CPU core to pin the thread to (handled by ThreadManager)
     * @param useMockMode Whether to use mock mode (no actual connection)
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
    void setOrderBook(std::shared_ptr<kubera::orderbook::OrderBook> orderBook);
    
    /**
     * @brief Check if connected
     * @return True if connected, false otherwise
     */
    bool isConnected() const;
    
    /**
     * @brief Send a test message to verify the connection
     * @return True if successful, false otherwise
     */
    bool sendTestMessage();
    
    /**
     * @brief Set the reconnection options
     */
    void setReconnectionOptions(int maxRetries, int initialDelayMs, int maxDelayMs, double backoffMultiplier);
    
private:
    // HFT optimization constants
    static constexpr size_t BUFFER_INITIAL_SIZE = 65536;  // 64KB
    static constexpr size_t BUFFER_MAX_SIZE = 1024 * 1024;  // 1MB max
    static constexpr size_t PARSER_CAPACITY = 1024 * 1024;  // 1MB parser capacity
    
    // Pre-allocated buffer for zero-allocation reads
    boost::beast::flat_buffer readBuffer_;
    
    // Thread-local JSON parser for zero-allocation parsing
    thread_local static simdjson::dom::parser jsonParser_;
    thread_local static bool parserInitialized_;
    
    // Boost.Beast components
    std::unique_ptr<boost::asio::io_context> ioc_;
    std::unique_ptr<boost::asio::ip::tcp::resolver> resolver_;
    std::unique_ptr<boost::beast::websocket::stream<boost::beast::tcp_stream>> ws_;
    std::unique_ptr<boost::beast::websocket::stream<boost::asio::ssl::stream<boost::beast::tcp_stream>>> ssl_ws_;
    std::unique_ptr<boost::asio::ssl::context> sslCtx_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> workGuard_;
    
    std::thread ioThread_;
    std::mutex mutex_;
    
    // WebSocket endpoint
    std::string endpoint_;
    std::string host_;
    std::string port_;
    std::string target_;
    bool secure_{false};
    
    // Connection state
    std::atomic<bool> connected_{false};
    std::atomic<bool> connecting_{false};
    
    // Reconnection options
    int maxRetries_{10};
    int initialDelayMs_{1000};
    int maxDelayMs_{30000};
    double backoffMultiplier_{1.5};
    int currentRetries_{0};
    
    // CPU core ID for thread pinning (handled by ThreadManager)
    int coreId_;
    
    // Message queue (for compatibility)
    moodycamel::ConcurrentQueue<orderbook::OrderBookUpdate> updateQueue_;
    
    // Logger
    logging::Logger& logger_;
    
    // Mock mode
    bool useMockMode_{false};
    std::thread mockThread_;
    std::atomic<bool> mockRunning_{false};
    
    // Direct OrderBook integration (HFT optimization)
    std::shared_ptr<kubera::orderbook::OrderBook> orderBook_;
    
    // HFT-optimized methods
    void initializeJsonParser();
    void startOptimizedReadLoop();
    void startOptimizedSslReadLoop();
    void handleMessageZeroCopy(std::string_view message);
    std::optional<orderbook::OrderBookUpdate> parseMessageOptimized(std::string_view message);
    
    // Original methods
    void parseUrl(const std::string& url, std::string& host, std::string& port, std::string& target, bool& secure);
    void startReadLoop();  // Legacy method
    void startSslReadLoop(); // Add missing SSL read loop declaration
    void handleMessage(const std::string& message);  // Legacy method
    void handleError(const std::string& error);
    void handleConnection();
    void handleDisconnection();
    bool reconnect();
    int calculateReconnectDelay() const;
    std::optional<kubera::orderbook::OrderBookUpdate> parseMessage(const std::string& message);
    void cleanup();
    
    // Add missing buffer management methods
    std::shared_ptr<boost::beast::flat_buffer> getReadBuffer();
    std::shared_ptr<boost::beast::flat_buffer> getSslReadBuffer();
    void resetBuffer(std::shared_ptr<boost::beast::flat_buffer>& buffer);
};

} // namespace websocket
} // namespace kubera